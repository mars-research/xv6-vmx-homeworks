/*
 * Hypervisor interface shared by the kernel (vmx.c, sysvmx.c)
 * and user programs (runvm.c).
 *
 * The design follows KVM. The kernel runs the guest and handles only
 * the exits it must; everything else is reported to a user-level
 * "virtual machine monitor" (runvm) through the three system calls
 *
 *   int vmcreate(char *image, int len, int memsz);
 *   int vmsetreg(int vm, int reg, uint64 val);
 *   int vmrun(int vm, struct vmexit *e);
 *
 * vmrun() enters the guest and returns when the guest does something
 * the kernel cannot or should not handle. The reason is described in
 * struct vmexit, defined below. The monitor emulates the event (for
 * example, a serial-port write), then calls vmrun() again, which
 * resumes the guest.
 *
 * This file is included on both sides of the system-call boundary, so
 * it must stay free of kernel-only definitions (those are in vmx.h).
 */

/*
 * Guest-physical address where vmcreate() loads the guest image, and
 * where runvm points the guest's RIP. 1 MB is the conventional start
 * of "extended memory" on a PC, which keeps the low 1 MB free.
 * helloguest.ld links the guest at this address; the two must agree.
 */
#define VM_LOAD_ADDR  0x100000

/*
 * Largest guest memory size vmcreate() accepts. Each 4 KB page is
 * allocated from the kernel's own memory and mapped through EPT.
 */
#define VM_MAXMEM     (64*1024*1024)

/*
 * Registers for vmsetreg(). The first fifteen follow the field order
 * of struct guestregs in vmx.h (which is not the hardware register
 * numbering: RBX comes second, RSI and RDI before RBP), because the
 * kernel stores the value at that index of the structure. RSP, RIP,
 * and RFLAGS are kept in the VMCS instead, so they get separate
 * cases. VMREG_NREGS is the count, used for range checking.
 */
enum {
  VMREG_RAX, VMREG_RBX, VMREG_RCX, VMREG_RDX, VMREG_RSI, VMREG_RDI,
  VMREG_RBP, VMREG_R8, VMREG_R9, VMREG_R10, VMREG_R11, VMREG_R12,
  VMREG_R13, VMREG_R14, VMREG_R15,
  VMREG_RSP, VMREG_RIP, VMREG_RFLAGS,
  VMREG_NREGS
};

/*
 * Basic VM exit reasons (SDM Appendix C) that vmrun() reports. They
 * are the low 16 bits of the VMCS exit-reason field. Exits that the
 * kernel handles itself (host interrupts, NMIs, RDMSR/WRMSR of EFER)
 * are normally not seen by user space. Only a subset of reasons is
 * named here; runvm treats any other as an error.
 */

/*
 * An exception or NMI. A guest exception arrives here because the
 * exception bitmap is all ones; the vector is in vmexit.intrinfo.
 */
#define EXIT_EXCEPTION_NMI      0

/* A host external interrupt arrived while the guest was running. */
#define EXIT_EXTERNAL_INTR      1

/* The guest triple-faulted (a fault while handling a double fault). */
#define EXIT_TRIPLE_FAULT       2

/* The guest executed CPUID. Always causes an exit; not handled yet. */
#define EXIT_CPUID              10

/* The guest executed HLT. runvm treats it as the guest finishing. */
#define EXIT_HLT                12

/* The guest executed VMCALL (a hypercall). Not handled yet. */
#define EXIT_VMCALL             18

/* A control-register access that the CR guest/host masks intercept. */
#define EXIT_CR_ACCESS          28

/*
 * IN/OUT/INS/OUTS. The kernel decodes the exit qualification into
 * vmexit.io; user space emulates the device.
 */
#define EXIT_IO                 30

/* RDMSR and WRMSR. The kernel emulates IA32_EFER only. */
#define EXIT_RDMSR              31
#define EXIT_WRMSR              32

/*
 * VM entry failed because of invalid guest state. It is reported with
 * vmexit.entryfail set, and the exit qualification says which check
 * failed (SDM 29.8).
 */
#define EXIT_INVALID_GUEST      33

/*
 * EPT violation: the guest accessed a guest-physical address that
 * the EPT does not map, or mapped without the needed permission.
 * vmexit.gpa holds the address and vmexit.qual the access type.
 * Here all guest memory is mapped, so this signals a wild access.
 */
#define EXIT_EPT_VIOLATION      48

/* EPT misconfiguration: an EPT entry has an invalid format. */
#define EXIT_EPT_MISCONFIG      49

/*
 * Not a hardware exit reason: VMLAUNCH itself failed; see 'error'.
 * 0xFFFF is outside the 16-bit basic reasons the CPU can produce, so
 * it cannot be confused with a real one.
 */
#define EXIT_VMFAIL             0xFFFF

/*
 * Filled in by vmrun() each time the guest exits to user space.
 * The kernel never reads it back: to complete an IN, user space
 * writes the result into the guest's RAX with vmsetreg().
 */
struct vmexit {
  uint reason;       /* basic exit reason (EXIT_*) */
  uint entryfail;    /* 1 if the exit is a failed VM entry */
  uint64 qual;       /* exit qualification: exit-specific detail */
  uint64 rip;        /* guest RIP of the exiting instruction */
  uint error;        /* VM-instruction error, for EXIT_VMFAIL */
  uint intrinfo;     /* exception exits: interruption information */
  uint intrerr;      /* exception exits: error code */
  uint64 gpa;        /* EPT exits: guest-physical address */
  struct {           /* EXIT_IO */
    ushort port;     /* I/O port number */
    uchar size;      /* 1, 2 or 4 bytes */
    uchar in;        /* 1 for IN, 0 for OUT */
    uint data;       /* OUT: value written (unused for IN) */
  } io;
};
