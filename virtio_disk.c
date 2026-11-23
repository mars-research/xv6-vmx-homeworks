// HW3: the paravirtualized guest's disk driver, in place of ide.c.
//
// The guest kernel (xv6 built with -DPARAVIRT) has no IDE controller.
// Its disk is a virtio block device (virtio.h), which runpv emulates,
// and the Makefile builds the guest with this file instead of ide.c.
// The functions keep ide.c's names, so the buffer cache and trap() do
// not change.
//
// A request is a chain of three descriptors in a queue in our memory:
// a header (read or write, and where), the buffer, and a status byte
// for the device to fill. We offer the chain in the available ring and
// write the queue's notify register: one exit, however much the request
// moves. The device returns the chain in the used ring and interrupts
// us (IRQ_IDE). Its registers would be memory-mapped on hardware; we
// reach them with hypercalls.
//
// After xv6-riscv's virtio_disk.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"

#define NUM VIRTQ_MAX   // descriptors in our queue

static struct {
  // The queue, which the device finds by the guest-physical addresses
  // we give it, and reads and writes there. The spec's alignments.
  struct virtq_desc desc[NUM] __attribute__((aligned(16)));
  struct virtq_avail avail __attribute__((aligned(2)));
  struct virtq_used used __attribute__((aligned(4)));
  char free[NUM];      // is the descriptor free?
  ushort usedidx;      // the next used entry we will look at
  // For each request in flight, by its first descriptor: its buffer,
  // and the header and status byte its chain points to.
  struct {
    struct buf *b;
    struct virtio_blk_req req;
    uchar status;
  } info[NUM];
  struct spinlock lock;
} disk;

static uint
vioread(int off)
{
  return hypercall(HC_VIOREAD, off, 0, 0);
}

static void
viowrite(int off, uint val)
{
  hypercall(HC_VIOWRITE, off, val, 0);
}

void
ideinit(void)
{
  uint status;
  uint64 cap;
  int i;

  initlock(&disk.lock, "virtio");
  if(vioread(VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MAGIC ||
     vioread(VIRTIO_MMIO_VERSION) != 2 ||
     vioread(VIRTIO_MMIO_DEVICE_ID) != VIRTIO_ID_BLOCK)
    panic("ideinit: no virtio disk");

  // The steps of the spec's "Device Initialization": reset the device,
  // tell it we see it and can drive it, agree on features, set up the
  // queue, and tell it to go.
  status = 0;
  viowrite(VIRTIO_MMIO_STATUS, status);
  status |= VIRTIO_STATUS_ACKNOWLEDGE;
  viowrite(VIRTIO_MMIO_STATUS, status);
  status |= VIRTIO_STATUS_DRIVER;
  viowrite(VIRTIO_MMIO_STATUS, status);

  // We need nothing beyond VIRTIO_F_VERSION_1, and accept nothing else.
  viowrite(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
  if(!(vioread(VIRTIO_MMIO_DEVICE_FEATURES) & (1 << (VIRTIO_F_VERSION_1-32))))
    panic("ideinit: not a virtio 1.x device");
  viowrite(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
  viowrite(VIRTIO_MMIO_DRIVER_FEATURES, 0);
  viowrite(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
  viowrite(VIRTIO_MMIO_DRIVER_FEATURES, 1 << (VIRTIO_F_VERSION_1-32));
  status |= VIRTIO_STATUS_FEATURES_OK;
  viowrite(VIRTIO_MMIO_STATUS, status);
  // The device clears FEATURES_OK if it does not accept them.
  if(!(vioread(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK))
    panic("ideinit: features not accepted");

  // Queue 0, the only one a block device needs.
  viowrite(VIRTIO_MMIO_QUEUE_SEL, 0);
  if(vioread(VIRTIO_MMIO_QUEUE_READY))
    panic("ideinit: queue in use");
  if(vioread(VIRTIO_MMIO_QUEUE_NUM_MAX) < NUM)
    panic("ideinit: queue too short");
  viowrite(VIRTIO_MMIO_QUEUE_NUM, NUM);
  viowrite(VIRTIO_MMIO_QUEUE_DESC_LOW, (uint)V2P(disk.desc));
  viowrite(VIRTIO_MMIO_QUEUE_DESC_HIGH, V2P(disk.desc) >> 32);
  viowrite(VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint)V2P(&disk.avail));
  viowrite(VIRTIO_MMIO_QUEUE_DRIVER_HIGH, V2P(&disk.avail) >> 32);
  viowrite(VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint)V2P(&disk.used));
  viowrite(VIRTIO_MMIO_QUEUE_DEVICE_HIGH, V2P(&disk.used) >> 32);
  viowrite(VIRTIO_MMIO_QUEUE_READY, 1);
  for(i = 0; i < NUM; i++)
    disk.free[i] = 1;

  status |= VIRTIO_STATUS_DRIVER_OK;
  viowrite(VIRTIO_MMIO_STATUS, status);

  // A block device's configuration starts with its capacity in sectors.
  cap = vioread(VIRTIO_MMIO_CONFIG) |
        (uint64)vioread(VIRTIO_MMIO_CONFIG + 4) << 32;
  if(cap < (uint64)FSSIZE * (BSIZE / 512))
    panic("ideinit: disk too small");
}

// Take three free descriptors for a request. Returns 0, or -1 if
// fewer than three are free.
static int
alloc3(int *idx)
{
  int i, n;

  n = 0;
  for(i = 0; i < NUM && n < 3; i++)
    if(disk.free[i])
      idx[n++] = i;
  if(n < 3)
    return -1;
  for(i = 0; i < 3; i++)
    disk.free[idx[i]] = 0;
  return 0;
}

static void
free3(int *idx)
{
  int i;

  for(i = 0; i < 3; i++)
    disk.free[idx[i]] = 1;
  wakeup(&disk.free[0]);
}

static void
setdesc(int i, uint64 addr, uint len, int flags, int next)
{
  disk.desc[i].addr = addr;
  disk.desc[i].len = len;
  disk.desc[i].flags = flags;
  disk.desc[i].next = next;
}

// Sync buf with disk.
// If B_DIRTY is set, write buf to disk, clear B_DIRTY, set B_VALID.
// Else if B_VALID is not set, read buf from disk, set B_VALID.
void
iderw(struct buf *b)
{
  int idx[3], write;
  struct virtio_blk_req *req;

  /* HW3 Part 5: write this function; see HW3.md. */
}

// Interrupt handler: finish the requests the device has returned.
void
ideintr(void)
{
  struct buf *b;
  int id;

  /* HW3 Part 5: write this function; see HW3.md. */
}
