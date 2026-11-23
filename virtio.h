// HW3: a virtio block device, shared by the guest's driver (ide.c, built
// with -DPARAVIRT) and the device that runpv.c emulates.
//
// Virtio is the device interface that guests use under KVM and QEMU,
// and under many other hypervisors. A device has a few registers, and one or
// more queues in the guest's memory through which the driver passes it
// requests. We use the "split" queues of virtio 1.x and the register
// layout of virtio over MMIO; the names below follow the virtio spec,
// whose section names are given in the comments.

#ifndef VIRTIO_H
#define VIRTIO_H

// Registers ("MMIO Device Register Layout"): byte offsets, each 32 bits.
// On real hardware they are memory-mapped; a paravirtualized guest
// reaches them with HC_VIOREAD and HC_VIOWRITE (pvapi.h).
#define VIRTIO_MMIO_MAGIC_VALUE         0x000  // VIRTIO_MAGIC
#define VIRTIO_MMIO_VERSION             0x004  // 2: virtio 1.x
#define VIRTIO_MMIO_DEVICE_ID           0x008  // VIRTIO_ID_BLOCK
#define VIRTIO_MMIO_VENDOR_ID           0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010  // 32 of the features it offers,
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014  //  selected by this: 0 or 1
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020  // the ones the driver accepts
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL           0x030  // the queue the next four are for
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034  // its largest size; 0: no queue
#define VIRTIO_MMIO_QUEUE_NUM           0x038  // the size the driver chose
#define VIRTIO_MMIO_QUEUE_READY         0x044  // 1: the queue is set up
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050  // write the queue's number:
                                               //  it has new requests
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060  // why it interrupted
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064  // write those bits back
#define VIRTIO_MMIO_STATUS              0x070  // VIRTIO_STATUS_*
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080  // guest-physical addresses of
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084  //  the descriptor table,
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW    0x090  //  the available ring,
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH   0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW    0x0a0  //  and the used ring
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH   0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION   0x0fc
#define VIRTIO_MMIO_CONFIG              0x100  // the device's own registers

#define VIRTIO_MAGIC    0x74726976  // "virt"
#define VIRTIO_ID_BLOCK 2

// Device status ("Device Status Field"): the driver sets these bits one
// by one as it initializes the device; writing 0 resets it.
#define VIRTIO_STATUS_ACKNOWLEDGE  1   // the driver saw the device
#define VIRTIO_STATUS_DRIVER       2   // and can drive it
#define VIRTIO_STATUS_DRIVER_OK    4   // the device is set up: go
#define VIRTIO_STATUS_FEATURES_OK  8   // the device accepts the features

// The one feature we use ("Reserved Feature Bits"): the virtio 1.x
// interface, which a version 2 device requires. It is bit 32, so it is
// bit 0 of the second word of features.
#define VIRTIO_F_VERSION_1  32

// INTERRUPT_STATUS: the device put requests in the used ring.
#define VIRTIO_INT_USED  1

// A split queue ("Split Virtqueues") has three parts, which the driver
// allocates in its memory, and whose guest-physical addresses it gives
// the device. Requests are chains of descriptors. The driver offers a
// chain by putting its first descriptor's index in the available ring;
// the device returns it, done, in the used ring. Each ring has an index
// that counts entries forever (it wraps at 65536), and only its writer
// changes it.
#define VIRTQ_MAX  8  // the largest queue our device has

struct virtq_desc {
  uint64 addr;     // guest-physical address of a buffer
  uint len;
  ushort flags;
  ushort next;     // the next descriptor, with VIRTQ_DESC_F_NEXT
};
#define VIRTQ_DESC_F_NEXT   1   // the chain goes on at next
#define VIRTQ_DESC_F_WRITE  2   // the device writes the buffer (else reads)

struct virtq_avail {         // written by the driver
  ushort flags;
  ushort idx;                // where the driver will put the next entry
  ushort ring[VIRTQ_MAX];    // first descriptors of offered chains
  ushort unused;
};

struct virtq_used_elem {
  uint id;                   // the chain's first descriptor
  uint len;                  // bytes the device wrote into the chain
};

struct virtq_used {          // written by the device
  ushort flags;
  ushort idx;
  struct virtq_used_elem ring[VIRTQ_MAX];
  ushort unused;
};

// Byte offsets in the rings, for the device, which reads them with
// pvmem() and so cannot use the structures' fields. ring[] has as many
// entries as the queue's size.
#define VIRTQ_AVAIL_IDX   2
#define VIRTQ_AVAIL_RING  4   // 2 bytes per entry
#define VIRTQ_USED_IDX    2
#define VIRTQ_USED_RING   4   // 8 bytes per entry

// A block device ("Block Device"). Its configuration registers start
// with its capacity, a 64-bit count of 512-byte sectors. A request is
// a header, the data, and a status byte that the device writes; xv6's
// driver puts each in a descriptor of its own.
struct virtio_blk_req {
  uint type;       // VIRTIO_BLK_T_*
  uint reserved;
  uint64 sector;   // where on the disk, in 512-byte sectors
};
#define VIRTIO_BLK_T_IN   0   // read from the disk
#define VIRTIO_BLK_T_OUT  1   // write to the disk

#define VIRTIO_BLK_S_OK      0
#define VIRTIO_BLK_S_IOERR   1
#define VIRTIO_BLK_S_UNSUPP  2

#endif // VIRTIO_H
