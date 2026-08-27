// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define BUDGET_IDX(blockno)     (blockno)%NBUDGET     // 将块号blockno进行哈希映射

struct {
  struct buf buf[NBUF];
  struct spinlock bdgt_lock[NBUDGET];       // 每个哈希桶的锁
  struct buf bdgt[NBUDGET];                 // 哈希桶, 初始为空
  struct spinlock bdgtfree_lock[NBUDGET];   // 每个哈希空桶的锁
  struct buf bdgt_free[NBUDGET];            // 哈希空桶, 初始为空
  struct spinlock freelist_lock;            // 空闲链表锁
  struct buf freelist;                      // 空闲链表, 所有buf初始时挂在上面, release时引用为0归还到freelist头部
} bcache;

// 头插法插入head
void insert(struct buf* head, struct buf* buf_t)
{
  buf_t->pre = head;
  buf_t->next = head->next;
  head->next->pre = buf_t;
  head->next = buf_t;
}

// 将指定的节点移出对应list
void buf_erease(struct buf* target)
{
  target->pre->next = target->next;
  target->next->pre = target->pre;
  target->pre = target;
  target->next = target;
}

void
binit(void)
{
  char        str[16] = {0};
  struct buf* b       = 0;

  // 初始化哈希桶和哈希桶锁
  for (int i = 0; i < NBUDGET; i++)
  {
    snprintf(str, 16, "bcache_bdgt%d", i);
    initlock(&bcache.bdgt_lock[i], str);

    bcache.bdgt[i].next = &bcache.bdgt[i];
    bcache.bdgt[i].pre = &bcache.bdgt[i];

    snprintf(str, 16, "bcache_bdgtfree%d", i);
    initlock(&bcache.bdgtfree_lock[i], str);

    bcache.bdgt_free[i].next = &bcache.bdgt_free[i];
    bcache.bdgt_free[i].pre = &bcache.bdgt_free[i];
  }

  // 初始化空闲链表
  initlock(&bcache.freelist_lock, "bcache_free");
  bcache.freelist.next = &bcache.freelist;
  bcache.freelist.pre = &bcache.freelist;
  for (b = bcache.buf; b < bcache.buf+NBUF; b++)
  {
    insert(&bcache.freelist, b);
    initsleeplock(&b->lock, "buffer");
  }
}

// 在bdgt_idx哈希桶中寻找对应blockno的buf
struct buf* search_bdgt_cached(struct buf* head, uint dev, uint blockno)
{
    struct buf* b = 0;

    for (b = head->next; b != head; b = b->next)
    {
      if(b->dev == dev && b->blockno == blockno){
        return b;
      }
    }
    return 0;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.

// 1.bdgt_idx哈希桶找cached -> 2.bdgt_idx哈希空桶找cached -> 3.bdgt_idx哈希空桶找free -> 4.freelist取新buf -> 5.从别的哈希空桶拿
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf* buf       = 0;
  uint8       bdgt_idx  = BUDGET_IDX(blockno);  // 原始哈希桶号

  acquire(&(bcache.bdgt_lock[bdgt_idx]));     // 需对对应的哈希桶全程进行上锁

  // 1.在bdgt_idx哈希桶中寻找是否有已映射到blockno的buf
  buf = search_bdgt_cached(&bcache.bdgt[bdgt_idx], dev, blockno);
  if (buf != 0)
  {
    buf->refcnt++;
    release(&(bcache.bdgt_lock[bdgt_idx]));
    acquiresleep(&buf->lock);
    return buf;
  }

  // 2.在bdgt_idx哈希空桶中找是否有已映射到blockno的buf
  acquire(&(bcache.bdgtfree_lock[bdgt_idx]));
  buf = search_bdgt_cached(&bcache.bdgt_free[bdgt_idx], dev, blockno);
  if (buf != 0)
  {
    // 从空桶中取出
    buf_erease(buf);
    buf->refcnt = 1;
    release(&(bcache.bdgtfree_lock[bdgt_idx]));
    // 插入到bdgt_idx哈希桶头部
    insert(&bcache.bdgt[bdgt_idx], buf);
    release(&(bcache.bdgt_lock[bdgt_idx]));
    acquiresleep(&buf->lock);
    return buf;
  }

  // 3. 在bdgt_idx哈希空桶中找LRU的buf
  if (bcache.bdgt_free[bdgt_idx].pre != &bcache.bdgt_free[bdgt_idx])
  {
    buf = bcache.bdgt_free[bdgt_idx].pre;
    // 从空桶中取出
    buf_erease(buf);
    buf->dev = dev;
    buf->blockno = blockno;
    buf->valid = 0;
    buf->refcnt = 1;
    release(&(bcache.bdgtfree_lock[bdgt_idx]));
    // 插入到bdgt_idx哈希桶头部
    insert(&bcache.bdgt[bdgt_idx], buf);
    release(&(bcache.bdgt_lock[bdgt_idx]));
    acquiresleep(&buf->lock);
    return buf;
  }
  release(&(bcache.bdgtfree_lock[bdgt_idx]));
  
  // 4.在freelist中找一个新buf放入到哈希桶中, bdgt_idx哈希桶的锁继续保持
  acquire(&(bcache.freelist_lock));
  if (bcache.freelist.next != &bcache.freelist)  // freelist不为空
  {
    buf = bcache.freelist.next;
    buf->dev = dev;
    buf->blockno = blockno;
    buf->valid = 0;
    buf->refcnt = 1;
    // 从freelist中取出
    buf_erease(buf);
    release(&(bcache.freelist_lock));
    // 插入到bdgt_idx哈希桶头部
    insert(&bcache.bdgt[bdgt_idx], buf);
    release(&(bcache.bdgt_lock[bdgt_idx]));
    acquiresleep(&buf->lock);
    return buf;
  }
  release(&(bcache.freelist_lock));

  // 5. 从别的哈希空桶中取LRU
  for (uint8 bdgt_idx_t = (bdgt_idx+1)%NBUDGET; bdgt_idx_t != bdgt_idx; bdgt_idx_t = (bdgt_idx_t+1)%NBUDGET)
  {
    acquire(&(bcache.bdgtfree_lock[bdgt_idx_t]));
    if (bcache.bdgt_free[bdgt_idx_t].pre != &bcache.bdgt_free[bdgt_idx_t])
    {
      buf = bcache.bdgt_free[bdgt_idx_t].pre;
      buf->dev = dev;
      buf->blockno = blockno;
      buf->valid = 0;
      buf->refcnt = 1;
      buf_erease(buf);
      release(&(bcache.bdgtfree_lock[bdgt_idx_t]));
      // 插入到bdgt_idx哈希桶头部
      insert(&bcache.bdgt[bdgt_idx], buf);
      release(&(bcache.bdgt_lock[bdgt_idx]));
      acquiresleep(&buf->lock);
      return buf;
    }
    release(&(bcache.bdgtfree_lock[bdgt_idx_t]));
  }

  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  uint8 bdgt_idx = BUDGET_IDX(b->blockno);    // 哈希桶号

  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&(bcache.bdgt_lock[bdgt_idx]));
  b->refcnt--;
  if (b->refcnt == 0) {
    // 归还给哈希空桶
    buf_erease(b);
    acquire(&(bcache.bdgtfree_lock[bdgt_idx]));
    insert(&bcache.bdgt_free[bdgt_idx], b);
    release(&(bcache.bdgtfree_lock[bdgt_idx]));
  }
  
  release(&(bcache.bdgt_lock[bdgt_idx]));
}

void
bpin(struct buf *b) {
  uint8 bdgt_idx = BUDGET_IDX(b->blockno);               // 哈希桶号

  acquire(&(bcache.bdgt_lock[bdgt_idx]));
  b->refcnt++;
  release(&(bcache.bdgt_lock[bdgt_idx]));
}

void
bunpin(struct buf *b) {
  uint8 bdgt_idx = BUDGET_IDX(b->blockno);               // 哈希桶号

  acquire(&(bcache.bdgt_lock[bdgt_idx]));
  b->refcnt--;
  release(&(bcache.bdgt_lock[bdgt_idx]));
}


