/*
 * runpv: run a paravirtualized xv6 as a guest, without VT-x (HW3).
 *
 *   runpv xv6pvkernel xv6fs.img
 *
 * Like runxv6 in HW2, we load the guest kernel, run it with the
 * kernel's help, and emulate its disk. The kernel (pv.c) virtualizes
 * the CPU and serves the guest's console itself, so pvrun() returns to
 * us only when the guest touches the disk's registers, or when Ctrl-]
 * stops it (pvapi.h).
 *
 * The disk is a virtio block device (virtio.h). HW2's IDE disk needs
 * about ten port accesses, each an exit, per block. virtio was designed
 * for hypervisors: the guest puts a request in a queue in its own
 * memory and writes the queue's notify register, one exit. We read the
 * queue and the buffers with pvmem(), which copies guest-physical
 * memory, do the requests, and interrupt the guest with pvinject().
 * The guest is stopped while we work, so its memory does not change
 * under us.
 *
 * The disk image is kept in memory, as runxv6 keeps it: the guest's
 * writes are lost when it stops, and xv6fs.img never changes.
 */

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "elf.h"
#include "traps.h"
#include "pvapi.h"
#include "virtio.h"

static char *disk;         /* the disk image */
static uint64 nsectors;    /* its size in 512-byte sectors */

/* The features we offer, by word: only VIRTIO_F_VERSION_1 (bit 32). */
static uint features[2] = { 0, 1 << (VIRTIO_F_VERSION_1 - 32) };

/* The device's registers, and the state of its one queue. */
static struct {
  uint status;
  uint featsel;            /* DEVICE_FEATURES_SEL */
  uint drvfeatsel;         /* DRIVER_FEATURES_SEL */
  uint drvfeatures[2];     /* the features the driver accepts */
  uint qsel;               /* QUEUE_SEL */
  uint qnum;               /* QUEUE_NUM */
  uint qready;             /* QUEUE_READY */
  uint64 desc, avail, used;  /* guest-physical addresses of the queue */
  ushort lastavail;        /* the next available entry to do */
  ushort usedidx;          /* the used ring's idx */
  uint isr;                /* INTERRUPT_STATUS */
} dev;

static void
die(char *msg)
{
  printf(2, "runpv: %s\n", msg);
  exit();
}

/*
 * Copy between guest-physical memory and ours. pvmem() fails only for
 * addresses outside guest memory, which the guest gave us: a driver
 * that does that is broken, and we stop it.
 */
static void
gread(uint64 gpa, void *buf, int n)
{
  if(pvmem(gpa, buf, n, 0) < 0)
    die("virtio: the guest gave a bad address");
}

static void
gwrite(uint64 gpa, void *buf, int n)
{
  if(pvmem(gpa, buf, n, PVMEM_WRITE) < 0)
    die("virtio: the guest gave a bad address");
}

/*
 * Do the request whose chain starts at descriptor head, and return how
 * many bytes we wrote into its buffers, for the used ring. Everything
 * here comes from the guest, so we check it all before use, and copy
 * each descriptor out once: we never reach outside guest memory or the
 * disk, whatever the guest wrote.
 */
static uint
dorequest(uint head)
{
  struct virtq_desc d[3];
  struct virtio_blk_req req;
  uint idx, n;
  uchar status;
  int i;

  /* HW3 Part 5: write this function; see HW3.md. */
  die("dorequest: not written");
  return 0;
}

/*
 * The driver wrote QUEUE_NOTIFY: do every request it has made
 * available since the last time, return each in the used ring, and
 * interrupt it.
 */
static void
notify(void)
{
  struct virtq_used_elem e;
  ushort aidx, head;

  /* HW3 Part 5: write this function; see HW3.md. */
  die("notify: not written");
}

/*
 * The driver wrote STATUS. 0 resets the device. Setting FEATURES_OK
 * asks us to accept the features it chose: only ones we offer, and
 * VIRTIO_F_VERSION_1, which a version 2 device requires. If we refuse,
 * the bit does not stick, and the driver sees that when it reads
 * STATUS back.
 */
static void
setstatus(uint val)
{
  if(val == 0){
    memset(&dev, 0, sizeof(dev));
    return;
  }
  if((val & VIRTIO_STATUS_FEATURES_OK) &&
     !(dev.status & VIRTIO_STATUS_FEATURES_OK) &&
     ((dev.drvfeatures[0] & ~features[0]) ||
      (dev.drvfeatures[1] & ~features[1]) ||
      !(dev.drvfeatures[1] & features[1])))
    val &= ~VIRTIO_STATUS_FEATURES_OK;
  dev.status = val;
}

/* Set the low or high half of a 64-bit address register. */
static void
sethalf(uint64 *a, uint val, int high)
{
  if(high)
    *a = (*a & 0xFFFFFFFF) | (uint64)val << 32;
  else
    *a = (*a & ~0xFFFFFFFFULL) | val;
}

/* The guest read register off (HC_VIOREAD). */
static uint
regread(uint off)
{
  switch(off){
  case VIRTIO_MMIO_MAGIC_VALUE:
    return VIRTIO_MAGIC;
  case VIRTIO_MMIO_VERSION:
    return 2;
  case VIRTIO_MMIO_DEVICE_ID:
    return VIRTIO_ID_BLOCK;
  case VIRTIO_MMIO_DEVICE_FEATURES:
    return dev.featsel < 2 ? features[dev.featsel] : 0;
  case VIRTIO_MMIO_QUEUE_NUM_MAX:
    /* There is only queue 0; 0 says a queue does not exist. */
    return dev.qsel == 0 ? VIRTQ_MAX : 0;
  case VIRTIO_MMIO_QUEUE_READY:
    return dev.qsel == 0 ? dev.qready : 0;
  case VIRTIO_MMIO_INTERRUPT_STATUS:
    return dev.isr;
  case VIRTIO_MMIO_STATUS:
    return dev.status;
  case VIRTIO_MMIO_CONFIG:          /* capacity, in sectors */
    return nsectors;
  case VIRTIO_MMIO_CONFIG + 4:
    return nsectors >> 32;
  }
  /* The vendor, the configuration generation, write-only registers. */
  return 0;
}

/* The guest wrote val to register off (HC_VIOWRITE). */
static void
regwrite(uint off, uint val)
{
  switch(off){
  case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
    dev.featsel = val;
    break;
  case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
    dev.drvfeatsel = val;
    break;
  case VIRTIO_MMIO_DRIVER_FEATURES:
    if(dev.drvfeatsel < 2)
      dev.drvfeatures[dev.drvfeatsel] = val;
    break;
  case VIRTIO_MMIO_QUEUE_SEL:
    dev.qsel = val;
    break;
  case VIRTIO_MMIO_QUEUE_NUM:
    if(dev.qsel == 0)
      dev.qnum = val;
    break;
  case VIRTIO_MMIO_QUEUE_READY:
    if(dev.qsel == 0)
      dev.qready = val & 1;
    break;
  case VIRTIO_MMIO_QUEUE_DESC_LOW:
  case VIRTIO_MMIO_QUEUE_DESC_HIGH:
    if(dev.qsel == 0)
      sethalf(&dev.desc, val, off == VIRTIO_MMIO_QUEUE_DESC_HIGH);
    break;
  case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
  case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
    if(dev.qsel == 0)
      sethalf(&dev.avail, val, off == VIRTIO_MMIO_QUEUE_DRIVER_HIGH);
    break;
  case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
  case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
    if(dev.qsel == 0)
      sethalf(&dev.used, val, off == VIRTIO_MMIO_QUEUE_DEVICE_HIGH);
    break;
  case VIRTIO_MMIO_QUEUE_NOTIFY:
    if(val == 0)
      notify();
    break;
  case VIRTIO_MMIO_INTERRUPT_ACK:
    dev.isr &= ~val;
    break;
  case VIRTIO_MMIO_STATUS:
    setstatus(val);
    break;
  }
}

/* Read a whole file into memory; return the buffer and its size. */
static char*
readfile(char *name, int *size)
{
  struct stat st;
  char *buf;
  int fd, n, off;

  if((fd = open(name, O_RDONLY)) < 0 || fstat(fd, &st) < 0)
    return 0;
  if((buf = malloc(st.size)) == 0)
    return 0;
  for(off = 0; off < st.size; off += n)
    if((n = read(fd, buf + off, st.size - off)) <= 0)
      return 0;
  close(fd);
  *size = st.size;
  return buf;
}

/*
 * Copy each loadable segment of the kernel to its physical address in
 * guest memory, as runxv6's loadkernel() does, and return the entry
 * point. Guest memory starts zeroed, so the bss is zero already.
 */
static uint64
loadkernel(char *image, int size)
{
  struct elfhdr *elf = (struct elfhdr*)image;
  struct proghdr *ph;
  int i;

  if(size < sizeof(*elf) || elf->magic != ELF_MAGIC)
    die("the kernel is not an ELF file");
  for(i = 0; i < elf->phnum; i++){
    ph = (struct proghdr*)(image + elf->phoff + i * sizeof(*ph));
    if(ph->type != ELF_PROG_LOAD || ph->filesz == 0)
      continue;
    if(ph->off + ph->filesz > size || ph->paddr + ph->memsz > PV_MEM)
      die("a kernel segment does not fit");
    if(pvmem(ph->paddr, image + ph->off, ph->filesz, PVMEM_WRITE) < 0)
      die("cannot write guest memory");
  }
  return elf->entry;
}

int
main(int argc, char *argv[])
{
  struct pvexit e;
  char *image;
  int size;

  if(argc != 3){
    printf(2, "usage: runpv kernel disk-image\n");
    exit();
  }
  if((disk = readfile(argv[2], &size)) == 0)
    die("cannot read the disk image");
  nsectors = size / 512;
  if((image = readfile(argv[1], &size)) == 0)
    die("cannot read the kernel");
  if(pvcreate() < 0)
    die("pvcreate failed (is another guest running?)");
  /*
   * The kernel's entry point is a physical address (entry.S), where the
   * guest starts with paging off, as far as it can tell (pv.c).
   */
  if(pvsetreg(PVREG_RIP, loadkernel(image, size)) < 0)
    die("bad entry point");
  free(image);

  printf(1, "runpv: booting %s (type Ctrl-] to stop)\n", argv[1]);
  for(;;){
    if(pvrun(&e) < 0)
      die("the guest failed");
    switch(e.reason){
    case PVEXIT_VIO:
      /* HC_VIOREAD returns what we put in the guest's rax. */
      if(e.write)
        regwrite(e.off, e.val);
      else if(pvsetreg(PVREG_RAX, regread(e.off)) < 0)
        die("pvsetreg failed");
      break;
    case PVEXIT_STOP:
      printf(1, "\nrunpv: stopped (Ctrl-])\n");
      exit();
    default:
      die("unknown exit");
    }
  }
}
