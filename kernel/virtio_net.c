//
// driver for qemu's virtio network device (an emulated Ethernet card).
// uses qemu's mmio interface to virtio, on the second virtio-mmio
// transport slot (the disk driver, virtio_disk.c, owns the first).
//
// qemu ... -netdev user,id=net0
//          -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.1
//
// unlike the disk, a NIC has two independent, asynchronous
// directions: the transmit queue is driven by us (net_tx queues a
// packet, waits for the device to say it's sent), while the receive
// queue is driven by the device (we pre-post empty buffers, and the
// device fills one in whenever a frame arrives on the wire, with no
// relation to when/whether anyone is calling net_rx()). See §5.6 of
// the xv6 book for the general driver structure this follows, and
// §5.1 of the virtio spec for the device-specific bits.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "virtio.h"
#include "net.h"

// the address of virtio mmio register r, on the network device's slot.
#define R(r) ((volatile uint32 *)(VIRTIO1 + (r)))

#define RXQ 0   // virtqueue 0: buffers the device fills with received frames
#define TXQ 1   // virtqueue 1: buffers we fill with frames to transmit

// how many frames net_rx() can have buffered, already received but
// not yet drained by any process.
#define NET_RXRING 8

// one virtqueue's worth of driver-visible state: the three rings
// from the spec, plus our own bookkeeping of which descriptors are
// in use. shared shape for both the rx and tx queues.
struct netq {
  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;
  char free[NUM];   // is descriptor i unused? (only meaningful for txq;
                     // rxq descriptors are permanently 1:1 with rx_buf[])
  uint16 used_idx;  // how far into used->ring we've consumed
};

static struct {
  int present;         // did we find and initialize the device?
  struct netq rxq;
  struct netq txq;
  uint8 mac[ETH_ADDR_LEN];

  // per-tx-descriptor completion flags, indexed by the head
  // descriptor of the (header, data) chain -- i.e. by the same
  // index net_tx() got back from alloc2_desc().
  char tx_done[NUM];

  // frames the interrupt handler has pulled off the rx virtqueue but
  // nobody has read yet, via net_rx(). a small software FIFO lets us
  // recycle hardware rx buffers back to the device immediately,
  // instead of holding them hostage until a process calls net_rx().
  struct {
    char data[NET_RXRING][NET_MAXFRAME];
    int  len[NET_RXRING];
    int  head, tail, count;
  } rxring;

  struct spinlock lock;
} net;

// DMA buffers. these must live in memory the kernel maps 1:1
// (virtual address == physical address), because the device does
// physical-address DMA into/out of them -- see the driver's own
// kalloc()'d queue-metadata pages below for the same reason. plain
// kernel .bss, like kalloc'd pages, satisfies this (kvminit direct-maps
// everything from the end of the kernel to PHYSTOP); a stack-resident
// buffer would NOT (kernel stacks live at the special high KSTACK
// virtual addresses; see memlayout.h and book §3.2).
static char net_rxbuf[NUM][PGSIZE];        // one per rx descriptor, forever
static char net_txbuf[NUM][PGSIZE];        // one per possible tx chain head
static struct virtio_net_hdr net_txhdr[NUM];

// find a free descriptor in queue q, mark it non-free, return its index.
static int
alloc_desc(struct netq *q)
{
  for(int i = 0; i < NUM; i++){
    if(q->free[i]){
      q->free[i] = 0;
      return i;
    }
  }
  return -1;
}

// mark a descriptor in queue q as free.
static void
free_desc(struct netq *q, int i)
{
  if(i >= NUM)
    panic("virtio_net free_desc 1");
  if(q->free[i])
    panic("virtio_net free_desc 2");
  q->desc[i].addr = 0;
  q->desc[i].len = 0;
  q->desc[i].flags = 0;
  q->desc[i].next = 0;
  q->free[i] = 1;
  wakeup(&q->free[0]);
}

// free a chain of descriptors in queue q, starting at i.
static void
free_chain(struct netq *q, int i)
{
  while(1){
    int flag = q->desc[i].flags;
    int nxt = q->desc[i].next;
    free_desc(q, i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

// a transmit request is always (header, data): two descriptors.
static int
alloc2_desc(int *idx)
{
  for(int i = 0; i < 2; i++){
    idx[i] = alloc_desc(&net.txq);
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(&net.txq, idx[j]);
      return -1;
    }
  }
  return 0;
}

// allocate the three ring structures for one virtqueue and tell the
// device about them. qsel selects which queue (RXQ or TXQ).
static void
netq_init(int qsel, struct netq *q)
{
  *R(VIRTIO_MMIO_QUEUE_SEL) = qsel;

  if(*R(VIRTIO_MMIO_QUEUE_READY))
    panic("virtio net queue should not be ready");

  uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0)
    panic("virtio net has no queue");
  if(max < NUM)
    panic("virtio net max queue too short");

  q->desc = kalloc();
  q->avail = kalloc();
  q->used = kalloc();
  if(!q->desc || !q->avail || !q->used)
    panic("virtio net kalloc");
  memset(q->desc, 0, PGSIZE);
  memset(q->avail, 0, PGSIZE);
  memset(q->used, 0, PGSIZE);
  for(int i = 0; i < NUM; i++)
    q->free[i] = 1;
  q->used_idx = 0;

  *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

  *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)q->desc;
  *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)q->desc >> 32;
  *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)q->avail;
  *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)q->avail >> 32;
  *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)q->used;
  *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)q->used >> 32;

  *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;
}

// pre-post all NUM rx buffers, permanently: descriptor i always
// points at net_rxbuf[i]. we never call alloc_desc/free_desc for the
// rx queue -- virtio_net_intr() just re-posts the same descriptor
// after copying its contents out.
static void
netq_fill_rx(void)
{
  for(int i = 0; i < NUM; i++){
    net.rxq.desc[i].addr = (uint64) net_rxbuf[i];
    net.rxq.desc[i].len = PGSIZE;
    net.rxq.desc[i].flags = VRING_DESC_F_WRITE; // device writes into it
    net.rxq.desc[i].next = 0;
    net.rxq.avail->ring[net.rxq.avail->idx % NUM] = i;
    __sync_synchronize();
    net.rxq.avail->idx += 1;
  }
  __sync_synchronize();
  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = RXQ;
}

void
virtio_net_init(void)
{
  uint32 status = 0;

  initlock(&net.lock, "virtio_net");

  if(*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R(VIRTIO_MMIO_VERSION) != 2 ||
     *R(VIRTIO_MMIO_DEVICE_ID) != 1 ||   // 1 == network card, 2 == disk
     *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551){
    // no NIC wired up (e.g. QEMUOPTS without -netdev/-device virtio-net-device).
    // don't panic: the rest of the OS should boot fine without a network card.
    printf("virtio_net: device not found, networking disabled\n");
    return;
  }

  // reset device
  *R(VIRTIO_MMIO_STATUS) = status;

  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(VIRTIO_MMIO_STATUS) = status;

  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(VIRTIO_MMIO_STATUS) = status;

  // negotiate features: we want only VIRTIO_NET_F_MAC (so we can
  // read a real MAC out of config space). everything else --
  // checksum/GSO offloads, mergeable rx buffers, the control queue,
  // multiqueue -- we deliberately don't ack, which keeps every
  // packet's virtio_net_hdr the plain 10-byte struct defined in
  // virtio.h and keeps us to exactly two virtqueues (rx, tx).
  uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  int have_mac = (features & (1 << VIRTIO_NET_F_MAC)) != 0;
  features &= (1 << VIRTIO_NET_F_MAC);
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  status = *R(VIRTIO_MMIO_STATUS);
  if(!(status & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio net FEATURES_OK unset");

  netq_init(RXQ, &net.rxq);
  netq_init(TXQ, &net.txq);

  // tell device we're completely ready.
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  // read the MAC address out of the device-specific config space, or
  // make one up (locally-administered, so it can't collide with a
  // real vendor's address) if the device didn't offer VIRTIO_NET_F_MAC.
  if(have_mac){
    volatile uint8 *cfg = (volatile uint8*)(VIRTIO1 + VIRTIO_MMIO_CONFIG);
    for(int i = 0; i < ETH_ADDR_LEN; i++)
      net.mac[i] = cfg[i];
  } else {
    static const uint8 fallback[ETH_ADDR_LEN] = {0x02, 0x00, 0x00, 0x78, 0x76, 0x36};
    memmove(net.mac, fallback, ETH_ADDR_LEN);
  }

  // give the device somewhere to put incoming frames before anything
  // can arrive.
  netq_fill_rx();

  net.present = 1;

  printf("virtio_net: mac %x:%x:%x:%x:%x:%x\n",
         net.mac[0], net.mac[1], net.mac[2],
         net.mac[3], net.mac[4], net.mac[5]);

  // plic.c and trap.c arrange for interrupts from VIRTIO1_IRQ.
}

// Queue up an Ethernet frame for transmission and wait for the
// device to confirm it has been sent. uaddr/len describe the frame
// (starting at the destination MAC, i.e. no virtio_net_hdr) in the
// calling process's address space. Returns len on success, -1 on
// error (no NIC, bad length, or the frame couldn't be copied in).
int
net_tx(uint64 uaddr, int len)
{
  struct proc *p = myproc();

  if(!net.present)
    return -1;
  if(len <= 0 || len > NET_MAXFRAME)
    return -1;

  acquire(&net.lock);

  int idx[2];
  while(1){
    if(alloc2_desc(idx) == 0)
      break;
    sleep(&net.txq.free[0], &net.lock);
  }

  if(copyin(p->pagetable, net_txbuf[idx[0]], uaddr, len) < 0){
    // NB: can't use free_chain() here -- the descriptors aren't
    // linked into a chain yet (that happens below), so it would only
    // ever see desc[idx[0]] and leak idx[1]. free each individually.
    free_desc(&net.txq, idx[0]);
    free_desc(&net.txq, idx[1]);
    release(&net.lock);
    return -1;
  }

  // no checksum/segmentation offload requested: an all-zero header.
  memset(&net_txhdr[idx[0]], 0, sizeof(struct virtio_net_hdr));

  net.txq.desc[idx[0]].addr = (uint64) &net_txhdr[idx[0]];
  net.txq.desc[idx[0]].len = sizeof(struct virtio_net_hdr);
  net.txq.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  net.txq.desc[idx[0]].next = idx[1];

  net.txq.desc[idx[1]].addr = (uint64) net_txbuf[idx[0]];
  net.txq.desc[idx[1]].len = len;
  net.txq.desc[idx[1]].flags = 0; // device only reads the outbound frame
  net.txq.desc[idx[1]].next = 0;

  net.tx_done[idx[0]] = 0;

  net.txq.avail->ring[net.txq.avail->idx % NUM] = idx[0];
  __sync_synchronize();
  net.txq.avail->idx += 1;
  __sync_synchronize();

  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = TXQ;

  // wait for virtio_net_intr() to say this chain has been transmitted.
  while(!net.tx_done[idx[0]])
    sleep(&net.tx_done[idx[0]], &net.lock);

  free_chain(&net.txq, idx[0]);

  release(&net.lock);
  return len;
}

// Copy out the oldest received-but-unread frame, if any, into
// uaddr/maxlen in the calling process's address space. Never blocks:
// returns 0 immediately if nothing has arrived yet, the frame length
// on success, or -1 on error (no NIC, or the copy failed).
int
net_rx(uint64 uaddr, int maxlen)
{
  struct proc *p = myproc();

  if(!net.present)
    return -1;

  acquire(&net.lock);

  if(net.rxring.count == 0){
    release(&net.lock);
    return 0;
  }

  int slot = net.rxring.tail;
  int n = net.rxring.len[slot];
  if(n > maxlen)
    n = maxlen;

  int ok = copyout(p->pagetable, uaddr, net.rxring.data[slot], n) == 0;

  net.rxring.tail = (net.rxring.tail + 1) % NET_RXRING;
  net.rxring.count -= 1;

  release(&net.lock);
  return ok ? n : -1;
}

// Copy the driver's MAC address out to uaddr (6 bytes). Returns 0 on
// success, -1 if there's no NIC or the copy failed.
int
net_getmac(uint64 uaddr)
{
  struct proc *p = myproc();

  if(!net.present)
    return -1;

  acquire(&net.lock);
  int ok = copyout(p->pagetable, uaddr, (char*)net.mac, ETH_ADDR_LEN) == 0;
  release(&net.lock);

  return ok ? 0 : -1;
}

void
virtio_net_intr(void)
{
  acquire(&net.lock);

  // ack the interrupt before draining the rings, same race-is-harmless
  // reasoning as virtio_disk_intr(): if the device adds more entries
  // after we ack but before we finish draining, we just handle them
  // now instead of in a (redundant) next interrupt.
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  __sync_synchronize();

  // transmit completions: free the two descriptors and wake net_tx().
  while(net.txq.used_idx != net.txq.used->idx){
    __sync_synchronize();
    int id = net.txq.used->ring[net.txq.used_idx % NUM].id;
    net.tx_done[id] = 1;
    wakeup(&net.tx_done[id]);
    net.txq.used_idx += 1;
  }

  // receive completions: stash each frame in the software ring (if
  // there's room -- otherwise drop it, like a real NIC under load
  // would), then immediately give the same hardware buffer back to
  // the device so it never runs out of places to put frames.
  while(net.rxq.used_idx != net.rxq.used->idx){
    __sync_synchronize();
    int id = net.rxq.used->ring[net.rxq.used_idx % NUM].id;
    uint32 len = net.rxq.used->ring[net.rxq.used_idx % NUM].len;

    if(len > sizeof(struct virtio_net_hdr) && net.rxring.count < NET_RXRING){
      int flen = len - sizeof(struct virtio_net_hdr);
      if(flen > NET_MAXFRAME)
        flen = NET_MAXFRAME;
      int slot = net.rxring.head;
      memmove(net.rxring.data[slot],
              net_rxbuf[id] + sizeof(struct virtio_net_hdr), flen);
      net.rxring.len[slot] = flen;
      net.rxring.head = (net.rxring.head + 1) % NET_RXRING;
      net.rxring.count += 1;
      wakeup(&net.rxring);
    }

    // re-post the (unchanged) buffer for the next incoming frame.
    net.rxq.desc[id].addr = (uint64) net_rxbuf[id];
    net.rxq.desc[id].len = PGSIZE;
    net.rxq.desc[id].flags = VRING_DESC_F_WRITE;
    net.rxq.desc[id].next = 0;
    net.rxq.avail->ring[net.rxq.avail->idx % NUM] = id;
    __sync_synchronize();
    net.rxq.avail->idx += 1;

    net.rxq.used_idx += 1;
  }
  __sync_synchronize();
  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = RXQ;

  release(&net.lock);
}
