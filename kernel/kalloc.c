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

struct run {
  struct run *next;
};

struct KMEM{
  struct spinlock lock;
  struct run *freelist;
}typedef kmem;

static kmem freelists[NCPU];

void
kinit()
{
  char str[16] = {0};
  for (int i = 0; i < NCPU; i++)
  {
    snprintf(str, 16, "kmem_cpu%d", i);
    initlock(&freelists[i].lock, str);
  }
  
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int id = 0;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  id = cpuid();
  pop_off();

  acquire(&(freelists[id].lock));
  r->next = freelists[id].freelist;
  freelists[id].freelist = r;
  release(&(freelists[id].lock));
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  int id = 0;
  uint8 find = 0;

  push_off();
  id = cpuid();
  pop_off();

  acquire(&(freelists[id].lock));
  r = freelists[id].freelist;
  if(r)
  {
    freelists[id].freelist = r->next;
    release(&(freelists[id].lock));
  }
  else  // 当前cpu的freelist没有,则从别的cpu上拿
  {
    release(&(freelists[id].lock)); // 防止死锁，后面的操作和id无关
    for (int i = 0; i < NCPU; i++)
    {
      if (i == id)
        continue;
      acquire(&(freelists[i].lock));
      r = freelists[i].freelist;
      if (r)
      {
        find = 1;
        freelists[i].freelist = r->next;
      }
      release(&(freelists[i].lock));
      if (find)
        break;
    }
  }

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
