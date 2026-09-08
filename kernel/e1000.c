#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

#define TX_SUCCESS  0
#define TX_FAILED   1

int
e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //
  
  uint64            desc_idx  = 0;  // tx环中TDT下标
  struct tx_desc*   desc_it   = 0;  // tx环中描述符对象

  acquire(&e1000_lock);

  desc_idx = regs[E1000_TDT];
  desc_it = tx_ring+desc_idx;

  // 1. tx环中是否有可用的描述符
  if ((desc_it->status & E1000_TXD_STAT_DD) == 0) // 无可用, 返回失败
  {
    printf("[e1000_transmit] ring full!\n");
    release(&e1000_lock);
    return TX_FAILED;
  }

  // 2. 释放上次发送的mbuf
  if(tx_mbufs[desc_idx] != 0) 
  { 
    mbuffree(tx_mbufs[desc_idx]);
    tx_mbufs[desc_idx] = 0;
  }
  tx_mbufs[desc_idx] = m;
  memset(desc_it, 0, sizeof(struct tx_desc));

  // 3. 填充描述符
  desc_it->addr = (uint64)(tx_mbufs[desc_idx]->head);
  desc_it->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  desc_it->length = tx_mbufs[desc_idx]->len;

  // 4. 交给e1000网卡
  regs[E1000_TDT] = (desc_idx+1)%TX_RING_SIZE;  // 修改TDT, e1000主动感知tail更新

  // 交给e1000网卡后, 释放锁返回, 不需要再等待处理完毕释放mbuf, 下一次发送时再释放上次的mbuf即可

  release(&e1000_lock);
  
  return TX_SUCCESS;
}

static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //
  uint64            desc_idx        = 0;    // rx环中TDT下标
  struct rx_desc*   desc_it         = 0;    // rx环中描述符对象
  struct mbuf*      m[TX_RING_SIZE] = {0};  // 驱动传回网络栈的包
  uint8             m_cnt           = 0;    // 本次中断驱动传回网络栈的包数量

  acquire(&e1000_lock);

  desc_idx = (regs[E1000_RDT]+1)%RX_RING_SIZE;
  desc_it = rx_ring+desc_idx;

  // 1. rx环中是否有可用的描述符, 一次中断可能对应多个包
  for (; desc_idx != (regs[E1000_RDH]+1)%RX_RING_SIZE; desc_it++, desc_idx++)
  {
    if ((desc_it->status & E1000_RXD_STAT_DD) && (desc_it->status & E1000_RXD_STAT_EOP)) // 这里只考虑一个描述符一个包的情况
    {
      // 2. 将从e1000网卡接收到的包进行拷贝
      m[m_cnt] = rx_mbufs[desc_idx];
      m[m_cnt]->len = desc_it->length; // e1000网卡写入的长度
      m[m_cnt]->head = (char*)desc_it->addr; // e1000网卡写入的地址

      // 3. 为rx环重新分配mbuf
      rx_mbufs[desc_idx] = mbufalloc(0);
      if (!rx_mbufs[desc_idx])
        panic("e1000");
      memset(desc_it, 0, sizeof(struct rx_desc));
      desc_it->addr = (uint64) rx_mbufs[desc_idx]->head;

      m_cnt++;  // 增加包数
    }
    else  // e1000网卡收包是连续的, 遇见第一个无效描述符就退出
    {
      desc_idx = (desc_idx-1+RX_RING_SIZE)%RX_RING_SIZE;
      desc_it = rx_ring+desc_idx;
      break;
    }
      
  }

  if (m_cnt == 0)
  {
    release(&e1000_lock);
    return ;
  }

  // 4. 处理完毕, 告知e1000网卡
  regs[E1000_RDT] = desc_idx;  // 修改RDT, 更新rx环中可用描述符, 让e1000有效的描述符可以继续接收包

  release(&e1000_lock);

  // 5. 将包交给网络栈处理
  for(int i = 0; i < m_cnt; i++)
    net_rx(m[i]);
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
