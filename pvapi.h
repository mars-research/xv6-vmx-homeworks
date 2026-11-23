/*
 * HW3: the paravirtual interface, shared by the host kernel (pv.c), the
 * guest kernel (xv6 built with -DPARAVIRT) and runpv.c.
 *
 * A paravirtualized guest knows it runs on a hypervisor. Its kernel runs
 * in ring 3, so it cannot execute privileged instructions (CLI, LIDT,
 * MOV to CR3, IN and OUT, ...); instead it asks the host with a
 * hypercall, the guest's version of a system call:
 *
 *   rax = HC_xxx, arguments in rdi, rsi, rdx; int $T_HCALL;
 *   the result comes back in rax, every other register is preserved.
 *
 * The vector is Xen's (32-bit PV guests use int $0x82). Its gate in the
 * host's IDT has DPL 3, so ring 3 may use it; the host refuses hypercalls
 * that come from the guest's user mode.
 */
#ifndef PVAPI_H
#define PVAPI_H

#define T_HCALL       0x82

/* Console (Xen's console device, without the shared ring). */
#define HC_CONSPUTC   1   /* rdi = character to print */
#define HC_CONSGETC   2   /* returns the next typed character, or -1 */
/*
 * The disk is a virtio block device (virtio.h), which runpv emulates.
 * Its registers would be memory-mapped on hardware; the guest reaches
 * them with these, and the kernel hands them to runpv (PVEXIT_VIO).
 * rdi = register offset, rsi = the value to write; HC_VIOREAD returns
 * the register's value.
 */
#define HC_VIOREAD    3
#define HC_VIOWRITE   4
/* The privileged parts of the CPU. */
#define HC_LCR3       5   /* rdi = guest-physical address of a PML4 */
#define HC_LIDT       6   /* rdi = guest-virtual address of a 256-gate IDT */
#define HC_SETKSTACK  7   /* rdi = kernel stack for traps from user mode */
#define HC_SETVCPU    8   /* rdi = guest-virtual address of struct pvcpu */
/*
 * Return from a trap, in place of IRETQ. rsp points to the frame IRETQ
 * would pop, preceded by the guest's rax, which the hypercall number
 * displaces: rax, rip, cs, rflags, rsp, ss (Xen's HYPERVISOR_iret).
 */
#define HC_IRET       9
/* Interrupts were just enabled and one is waiting: deliver it now. */
#define HC_STI        10

/*
 * The guest's memory, starting at guest-physical address 0. Must match
 * PHYSTOP of the guest kernel (the Makefile builds it with
 * -DPHYSTOP=0x2000000).
 */
#define PV_MEM        (32*1024*1024)

/*
 * The system calls with which runpv runs the guest, as runxv6 runs a
 * VT-x guest with vmrun() (vmxapi.h). As there, the kernel virtualizes
 * the CPU, and a user program emulates the devices: here only the
 * disk, since the kernel serves the console itself.
 *
 *   int pvcreate(void);
 *     Create the guest: PV_MEM bytes of zeroed memory, nothing loaded.
 *     There is one guest at a time; only the process that created it
 *     may use the calls below, and it is freed when that process exits.
 *   int pvmem(uint64 gpa, void *buf, int len, int flags);
 *     Copy len bytes from guest-physical address gpa to buf, or, with
 *     PVMEM_WRITE, from buf to the guest. Like vmmem() without
 *     VMMEM_VIRT.
 *   int pvsetreg(int reg, uint64 val);
 *     Set the guest's rip (to boot it) or rax (to answer HC_VIOREAD).
 *   int pvinject(int irq);
 *     Raise the guest's interrupt line irq (0-31); the kernel delivers
 *     it when the guest has interrupts on.
 *   int pvrun(struct pvexit *e);
 *     Run the guest until runpv must act; e says why. Returns -1 if
 *     the guest failed (the kernel prints why).
 *
 * Every call returns 0, or -1 on bad arguments.
 */
#define PVMEM_WRITE   1

#define PVREG_RAX     0
#define PVREG_RIP     1

#define PVEXIT_VIO    1   /* HC_VIOREAD or HC_VIOWRITE */
#define PVEXIT_STOP   2   /* Ctrl-] was typed */

#ifndef __ASSEMBLER__
struct pvexit {
  uint reason;    /* PVEXIT_* */
  uint write;     /* PVEXIT_VIO: 1 for HC_VIOWRITE */
  uint off;       /* PVEXIT_VIO: the register */
  uint val;       /* PVEXIT_VIO: the value written */
};

/*
 * State the guest kernel and the host share, without hypercalls: the
 * guest registers it with HC_SETVCPU, and the host reads and writes it
 * in place (Xen's shared_info page). It must not cross a page boundary.
 */
struct pvcpu {
  uint vif;       /* virtual RFLAGS.IF: interrupts enabled */
  uint pending;   /* the host has an interrupt waiting for vif */
  uint64 cr2;     /* virtual CR2: the address of the last page fault */
};
#endif

#endif /* PVAPI_H */
