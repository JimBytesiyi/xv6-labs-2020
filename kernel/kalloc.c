// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define index(x) (((uint64)x - KERNBASE) >> PGSHIFT)

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

int ref_cnt[PHYSTOP / PGSIZE]; // 用于对页面进行引用计数

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
  {
    ref_cnt[(uint64)p / PGSIZE] = 1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  // kfree会对pa作有效性检查
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // kfree()只应在引用计数为零时将页面放回空闲列表
  acquire(&kmem.lock); // 防止多个CPU中不同进程同时释放同一页
  int pn = (uint64)pa / PGSIZE;
  if(ref_cnt[pn] < 1) // 还未自减之前已经小于1, 有异常
    panic("kfree ref\n");
  ref_cnt[pn] -= 1;
  int tmp = ref_cnt[pn]; // 保留一个临时变量判断-1后是否大于0
  release(&kmem.lock);

  if(tmp > 0) // 引用计数不为0, 不需要释放页面
    return;

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist; // 获取空闲内存
  if(r)
  {
    kmem.freelist = r->next;
    int pn = (uint64)r / PGSIZE;
    if(ref_cnt[pn] != 0)
      panic("kalloc ref\n");
    ref_cnt[pn] = 1;
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk

  return (void*)r;
}

void
incr_cnt(uint64 pa) // 增加页面的引用计数
{
  int pn = pa / PGSIZE;
  acquire(&kmem.lock);
  if(pa >= PHYSTOP || ref_cnt[pn] < 1)
    panic("incr_cnt\n");
  ref_cnt[pn] += 1;
  release(&kmem.lock);
}