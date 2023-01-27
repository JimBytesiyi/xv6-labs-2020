#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
// 这个pagetable_t是个指向root page-table page的指针
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

/*
 * create a direct-map page table for the kernel.
 */
void
kvminit()
{ // 保持kvminit
  kernel_pagetable = (pagetable_t) kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // 为内核页表增加映射, 最后一位就是要设定的标志位perm 
  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

pagetable_t // 返回内核页表的指针
kvmcreate(void)
{
  pagetable_t pagetable;
  pagetable = uvmcreate();
  int i;
  // 复制pte1~511, 没有分配内存(即增加映射)
  // 内核页表中KERNBASE以上的东西都是相同的, 可以共享
  // 映射是指VA与PA的对应关系, 增加映射就是分配了相应的物理内存
  // 只新建一个pte0用于覆盖设备的地址空间
  for(i = 1; i < 512; i++)
  {
      pagetable[i] = kernel_pagetable[i];
  }
  // 只在pte0中增加映射(pte0覆盖的地址空间大概是1GB, 2^30)
  // uart registers
  kvmmapkern(pagetable, UART0, UART0, PGSIZE, PTE_R | PTE_W);
  // virtio mmio disk interface
  kvmmapkern(pagetable, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
  // CLINT
  // kvmmapkern(pagetable, CLINT, CLINT, 0x10000, PTE_R | PTE_W);
  // PLIC
  kvmmapkern(pagetable, PLIC, PLIC, 0x400000, PTE_R | PTE_W);
  return pagetable;
}

// 释放进程内核页表
// 0级页表就是root pagetable, 依此类推
// 1级页表是intermediate pagetable
// 2级页表是leaf pages
void kvmfree(pagetable_t kpagetable, uint64 sz)
{ // 只用释放pte0(只有pte0不同, 内核页表的其他地方都是相同的)
  pte_t pte = kpagetable[0];
  // pte既是VA又是指向下一级页表的指针
  pagetable_t level1 = (pagetable_t)PTE2PA(pte);
  for(int i = 0; i < 512; i++) // 遍历一级页表
  {
    pte_t pte = level1[i];
    if(pte & PTE_V)
    {
      uint64 level2 = PTE2PA(pte);
      kfree((void*)level2);
      level1[i] = 0;
    }
  }
  kfree((void*)level1);
  kfree((void*)kpagetable);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
// 这是用来切换内核页表的函数
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      // 分配新页并没有增加映射
      // 如果alloc位=0或新页无法被分配, 返回0
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// 查看user program提供的VA是否在user page中有效
// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.(只能用于user pages的VA转换)
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

void 
kvmmapkern(pagetable_t pagetable, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(pagetable, va, sz, pa, perm) != 0)
    panic("kvmmapkern");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(pagetable_t kpagetable, uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;
  
  pte = walk(kpagetable, va, 0);
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{ 
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    // 用walk函数分配新页和物理内存
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    // 设定标志位
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0) // 如果有效页的pte不存在
      panic("uvmunmap: not mapped"); 
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  // 将该页全部设为0
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    // uvmdealloc调用了uvmunmap函数
    // 如果newsz小于oldsz就要解除映射并且释放newsz的物理内存
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  // 先释放用户内存页, 然后再释放page-table pages
  if(sz > 0)
    // 这个uvmunmap函数解除所有leaf pages的映射而且释放了内存
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  // 这个freewalk函数只负责释放所有page-table pages
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  /*uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    // 要先将user pointer对应的VA转换成PA
    pa0 = walkaddr(pagetable, va0); 
    if(pa0 == 0) // 如果物理地址无效
      return -1;
    n = PGSIZE - (srcva - va0); // 计算需要移动的字节数
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;*/
  // 直接替换成copyin_new
  return copyin_new(pagetable, dst, srcva, len);
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  /*uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }*/
  return copyinstr_new(pagetable, dst, srcva, max);
}

// 加上一个辅助参数level就可以追踪是哪一级的page-table page
// 当时确实没想到这个
void travel(pagetable_t pagetable, int level)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0)
    { 
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte); // 也是pte对应的PA
      printf("..");
      // 这个j是用来辅助打印第二个..
      for(int j = 0; j < level; ++j)
      {
          printf(" ..");
      }
      // 打印root page-table page
      printf("%d: pte %p pa %p fl %p\n", i, pte, child, PTE_FLAGS(child));
      // 这个递归调用会打印第二级page-table page
      travel((pagetable_t)child, level + 1);
    }
    else if(pte & PTE_V)
    { // 当递归满足终止条件(即遍历到第三级page-table page时)
      // 可以直接打印pte和pte对应的PA
      printf(".. .. ..");
      printf("%d: pte %p pa %p fl %p\n", i, pte, PTE2PA(pte), PTE_FLAGS(pte));
    }
  }
  return;
}

void vmPrint(pagetable_t pagetable)
{   // 打印调用vmPrint的参数
    printf("page table %p\n", pagetable);
    travel(pagetable, 0);
}

void kvmswitch(pagetable_t pagetable)
{
  // 仿照kvminithart将进程的内核页表加载进satp寄存器
  w_satp(MAKE_SATP(pagetable));
  // flush all TLB entries, 这是新的页表
  sfence_vma();
}

void kvmswitch_kernel()
{
  kvmswitch(kernel_pagetable);
}

// 将进程从user page映射到进程内核页表
// 由于可能有多个进程同时运行, 需要用pid区分是哪个进程的内核页表 
void
kvmmapuser(int pid, pagetable_t pagetable, pagetable_t kpagetable, uint64 oldsz, uint64 newsz)
{
  uint64 va;
  pte_t* upte;
  pte_t* kpte;

  if(newsz >= PLIC)
    panic("kvmmapuser: newsz too large");
  
  // if newsz < oldsz, clear ptes.
  // this is not really necessary
  // because copyin etc. will check against p->sz, so the kernel
  // wouldn't use the ptes. but this matches the user table
  /*if(newsz < oldsz)
  {
    for(va = newsz; va < oldsz; va += PGSIZE)
    {
      kpte = walk(pagetable, va, 1);
      *kpte &= ~PTE_V;
    }
  }*/

  for(va = oldsz; va < newsz; va += PGSIZE)
  {
      upte = walk(pagetable, va, 0);
      if(upte == 0)
      {
        printf("kvmmapuser: 0x%x 0x%x\n", va, newsz);
        panic("kvmmapuser: user page pte does not exist\n");
      }
      if((*upte & PTE_V) == 0)
      {
        printf("kvmmapuser: no valid pte 0x%x 0x%x\n", va, newsz);
        panic("kvmmapuser: no valid user pte\n");
      }
      // 可能会在进程内核页表中分配新页和对应的物理内存
      kpte = walk(kpagetable, va, 1);
      if(kpte == 0)
      {
        panic("kvmmapuser: no valid kernel pte");
      }
      *kpte = *upte; // 直接将user pte复制到kernel pte中
      *kpte &= ~(PTE_U | PTE_W | PTE_X);
  }
}