#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

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

uint64 sys_mmap(void)
{
  /* 入参 */
  int    addr    = 0;    // 无效, 应由内核决定映射的虚拟地址
  int    length  = 0;    // 映射的总长度
  int    prot    = 0;    // 读写权限 (PROT_READ 可读 | PROT_WRITE 可写)
  int    flags   = 0;    // 映射的可见性 (MAP_PRIVATE 修改仅当前进程可见,且不写入磁盘 | MAP_SHARED 写回磁盘)
  int    fd      = 0;    // 需要映射的文件的文件描述符
  int    offset  = 0;    // 从文件的此处偏移开始映射
  /* 返回值 */
  uint64 addr_va = 0;    // 文件映射的虚拟地址

  if(argint(0, &addr) < 0 || argint(1, &length) || argint(2, &prot) || argint(3, &flags) || argint(4, &fd) || argint(5, &offset))
    return MAP_FAILED;

  // 找到文件的inode
  struct proc*    proc_p  = myproc(); // 进程pcb
  struct inode*   inode_p = 0;        // 文件inode
  struct file*    file_p  = 0;

  if (fd < 0 || fd >= NOFILE || (file_p = proc_p->ofile[fd]) == 0)
    return MAP_FAILED;

  if (((file_p->readable == 0) && (prot&PROT_READ)) || ((file_p->writable == 0) && ((prot&PROT_WRITE)) && !(flags&MAP_PRIVATE)))    // fd和mmap权限不匹配
  {
    return MAP_FAILED;
  }
  
  if ((inode_p = file_p->ip) == 0)
    return MAP_FAILED;

  inode_p->ref++;   // 增加文件的引用计数

  // 为了配合懒分配, 维护一个结构体数组, 保存mmap的信息
  mmap_info* mmap_info = 0;

  for (mmap_info = proc_p->mmap_info; mmap_info < proc_p->mmap_info+NOFILE; mmap_info++)
  {
    if (0 == mmap_info->valid)
      break;
  }

  mmap_info->valid      = 1;
  mmap_info->addr_va    = proc_p->sz;
  mmap_info->file_inode = inode_p;
  mmap_info->offset     = offset;
  mmap_info->length     = length;
  mmap_info->prot       = prot;
  mmap_info->flags      = flags;

  addr_va = proc_p->sz;

  // 增加进程堆空间大小, 但不分配物理页, 等待触发懒分配
  proc_p->sz += length;

  return addr_va;
}

int sys_munmap(void)
{
  /* 入参 */
  int addr_va = 0;    // 取消映射的虚拟地址首地址
  int length  = 0;    // 取消映射的总长度

  /* 返回值 */
  int ret = 0;

  if(argint(0, &addr_va) < 0 || argint(1, &length))
    return MAP_FAILED;
  
  ret = munmap(addr_va, length);

  return ret;
}