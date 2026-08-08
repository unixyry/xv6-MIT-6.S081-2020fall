#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

extern uint64 get_freemem();
extern uint64 get_proc_num();

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

// 追踪系统调用
uint64 sys_trace(void)
{
	struct proc *p = myproc();

  if(argint(0, &(p->trace_mask)) < 0)
	{
		return -1;
	}

	return 0;
}

// 收集系统信息
uint64 sys_sysinfo(void)
{
	uint64 u_sysinfo_addr = 0;
  struct sysinfo k_info = {0};
  struct proc *p = myproc();

  // 获取用户层的sysinfo结构体虚拟内存位置
  if(argaddr(0, &(u_sysinfo_addr)) < 0)
	{
		return -1;
	}

  // 获取系统信息
  k_info.freemem = get_freemem();
  k_info.nproc = get_proc_num();

  // 将内容拷贝回用户层的sysinfo结构体虚拟内存位置
  if (copyout(p->pagetable, u_sysinfo_addr, (char*)&k_info, sizeof(k_info)) < 0)
    return -1;
  
	return 0;
}
