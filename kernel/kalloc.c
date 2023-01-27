// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.
// 每个链表都指向上一个可用空间
struct run { // run是一个链表
  struct run *next; // 指向下一个节点
};
// kmem中的freelist是一个尾部指针
// kmem就是一个保存最后链表的变量
struct {
  struct spinlock lock;
  struct run *freelist; // 关于尾部指针的初始化问题: freelist默认初始化为NULL
  // 不用担心访问越界
} kmem; // kmem是一个结构体

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void 
freerange(void *pa_start, void *pa_end)
{
  char *p; // p是一个char类型的指针
  p = (char*)PGROUNDUP((uint64)pa_start); // 这应该是计算physical address的开头
  // 这是一页一页释放内存
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void // 参数由kalloc返回的指针提供
kfree(void *pa) // 释放kalloc函数分配的内存页
{
  struct run *r; // 当前页节点

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE); // 将该页的内容全部写1, 代表该页可用

  r = (struct run*)pa; // 将物理地址类型转化为链表节点

  acquire(&kmem.lock); // 这里有一个锁
  // 当前节点的next指针指向上一页空闲页
  r->next = kmem.freelist;
  kmem.freelist = r; // 尾部指针更新为链表当前节点(正在被释放的当前页)
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void) // 这个kalloc函数是用来分配可用物理内存页的
{
  struct run *r;

  acquire(&kmem.lock); // 加锁
  r = kmem.freelist; 
  if(r) // 如果仍有空闲页
    kmem.freelist = r->next; // 尾部指针更新为下一个空闲页
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r; // 返回一个kernel可用的指针 
}

// 返回free memory的数量
uint64 num_of_freeMemory(void)
{
  uint64 num = 0;
  struct run* page = kmem.freelist;
  while(page) // 简单地遍历链表, 没有加锁; 当page不为空(即遍历到尾节点)时
  {
      // count for numbers of page
      num++;
      page = page->next; // 指向下一页
  }
  return PGSIZE * num;
}