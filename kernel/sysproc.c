#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_trace(void)
{
    int mask; // user space的trace(int)会接受一个int参数
    // 主要的问题是如何将user space中的函数参数传递给system call function
    // mask的值保存在寄存器a0中, 所以传递给argint的第一个参数是0
    if(argint(0, &mask) < 0) // 读取trapframe中a0寄存器的值
      return -1;
    myproc()->trace_mask = mask; // 将读取的值赋给当前进程
    return 0;
}

uint64 // 打印系统信息
sys_sysinfo(void)
{
    struct proc* p = myproc();
    struct sysinfo s;
    uint64 addr; // 用来存放VA
    s.freemem = num_of_freeMemory();
    s.nproc = num_of_proc();

    // 获取virtual address
    if(argaddr(0, &addr) < 0)
      return -1;
    // Copy from kernel to user.
    // Copy len bytes from src to virtual address dstva in a given page table.
    // 第三个参数是src, 第四个参数是复制数据的大小
    // 第二个参数是destination VA, 目的虚拟地址
    if(copyout(p->pagetable, addr, (char *)(&s), sizeof(s)) < 0)
      return -1;
    return 0;
}