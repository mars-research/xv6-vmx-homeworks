// HW3: a hypervisor without VT-x, for a paravirtualized xv6.
//
//   runpv xv6pvkernel xv6fs.img
//
// The guest is xv6 built with -DPARAVIRT. Its kernel runs in ring 3,
// in the process that runs runpv, like user code, but on page tables
// of its own. It never executes a privileged instruction: it asks us
// with a hypercall (pvapi.h). Any trap from the guest (a hypercall, a
// page fault, the guest's own system calls, a host interrupt) comes to
// the host's IDT and trap(), which hands it to pvexit(): our VM exit.
// pvloop() handles it and enters the guest again with pv_enter()
// (pvasm.S), an IRETQ to ring 3: our VM entry.
//
// As vmx.c and runxv6 do in HW2, we split the work the way KVM and QEMU
// do: we virtualize the CPU, and runpv emulates the guest's disk, a
// virtio device. pvrun() returns to runpv when the guest touches the
// disk's registers; runpv reaches guest memory with pvmem().
//
// In place of the hardware's help, we do in software:
//
//  - memory: shadow page tables, which the CPU uses while the guest
//    runs. They map the guest's virtual addresses to our pages, and
//    are filled in from the guest's own page tables when the guest
//    faults (pvfill()). The guest tells us when it loads CR3, and xv6
//    reloads CR3 after every change to the current page table, as the
//    TLB requires, so the shadows are flushed exactly when needed;
//  - privilege: the guest's kernel and its user programs both run in
//    ring 3, so we keep two shadows, and the one for the guest's user
//    mode maps only the guest's PTE_U pages;
//  - traps: we deliver the guest's exceptions, system calls and
//    interrupts through the guest's IDT, as the CPU would (pvtrap()),
//    and the guest returns with a hypercall instead of IRETQ;
//  - the console, served by hypercalls, as Xen's paravirtual console is.
//
// One guest exists at a time; it owns the console until its process
// exits.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "pvapi.h"

#define NPVPAGES (PV_MEM / PGSIZE)

// Ctrl-] stops the guest, as in runxv6 (HW2).
#define QUITKEY 0x1d

// The RFLAGS bits the guest may set with HC_IRET: CF, PF, AF, ZF, SF,
// DF and OF. Never IOPL (the guest could use IN and OUT) or IF (it
// could keep the host's timer from interrupting it).
#define FL_GUEST 0xCD5

// Exceptions for which the CPU pushes an error code (SDM 7.13).
#define HASERR(v) ((v) == 8 || ((v) >= 10 && (v) <= 14) || (v) == 17 || \
                   (v) == 21)

// Everything we keep about the guest.
static struct {
  struct trapframe tf;   // its registers while it does not run
  int user;              // it is in its user mode (virtual CPL 3)
  uint64 cr3;            // guest-physical PML4; 0 before the first HC_LCR3
  uint64 idt;            // guest-virtual IDT; 0 before HC_LIDT
  uint64 kstack;         // its stack for traps from its user mode
  struct pvcpu *vcpu;    // the shared struct pvcpu; 0 before HC_SETVCPU
  uint64 cr2;            // the address of a page-fault exit
  pml4e_t *shadow[2];    // shadow page tables: [0] kernel mode, [1] user
  char *mem[NPVPAGES];   // guest-physical page n is our page mem[n]
  struct pvexit exit;    // why pvrun() returns to runpv
  uint64 hostrsp;        // our stack pointer in pv_enter(), for pv_exit()
  uint lastticks;        // host ticks at its last timer interrupt
  // Exits by kind, printed when the guest is freed.
  uint nexit, nhcall, nfill, nflush, ntrap, nuser;
  // Also used by the console interrupt; protected by pvlock.
  uint pending;          // interrupts to deliver: bit n is IRQ n
  char in[128];          // typed characters it has not read yet
  uint inr, inw;
  int stop;              // Ctrl-] was typed
} pv;

static struct spinlock pvlock = { .name = "pv" };
// The process that created the guest. Only that process sets pvowner to
// itself and clears it again, so it can check pvowner == myproc()
// without the lock.
static struct proc *pvowner;

// pvasm.S
void pv_enter(struct trapframe*, uint64 *hostrsp, struct taskstate*);
void pv_exit(uint64 hostrsp) __attribute__((noreturn));

// Translate guest-virtual address va as the guest's MMU would: through
// the guest's page tables (4-level paging, SDM 5.5), which are in guest
// memory. Set *gpa to the guest-physical address, and *perm to the
// PTE_W and PTE_U bits that every level grants. Returns -1 if va is not
// mapped, or not to guest memory.
static int
pvwalk(uint64 va, uint64 *gpa, int *perm)
{
  uint64 table, e, size;
  int level;

  /* HW3 Part 2: write this function; see HW3.md. */
  *gpa = 0;
  *perm = 0;
  return -1;
}

// Copy n bytes between buf and guest-virtual address va: into the
// guest if write is set. Returns -1 if some page is not mapped.
static int
pvcopy(uint64 va, void *buf, uint n, int write)
{
  uint64 gpa;
  int perm;
  uint m;
  char *p, *g;

  p = buf;
  while(n > 0){
    if(pvwalk(va, &gpa, &perm) < 0)
      return -1;
    // To the end of the page: the next one may be anywhere in our memory.
    m = PGSIZE - va % PGSIZE;
    if(m > n)
      m = n;
    g = pv.mem[gpa / PGSIZE] + gpa % PGSIZE;
    if(write)
      memmove(g, p, m);
    else
      memmove(p, g, m);
    n -= m;
    p += m;
    va += m;
  }
  return 0;
}

// Forget every translation of the guest's: free the page tables in the
// lower half of both shadows. The upper half maps the host kernel.
static void
pvflush(void)
{
  int s, i;

  for(s = 0; s < 2; s++)
    for(i = 0; i < PML4X(KERNBASE); i++)
      if(pv.shadow[s][i] & PTE_P){
        // freepgdir() frees page tables only, never the pages they map.
        freepgdir((uint64*)P2V(PTE_ADDR(pv.shadow[s][i])), 3);
        pv.shadow[s][i] = 0;
      }
}

// The guest faulted at va: if its own page tables allow the access in
// its current mode, copy the translation into the shadow and return 0;
// the guest retries and never knows. Otherwise return -1: the fault is
// the guest's. err is the page-fault error code (bit 1: a write).
static int
pvfill(uint64 va, uint64 err)
{
  uint64 gpa;
  int perm;
  pte_t *pte;

  /* HW3 Part 2: write this function; see HW3.md. */
  return -1;
}

// The guest's virtual RFLAGS.IF.
static int
pvif(void)
{
  return pv.vcpu && pv.vcpu->vif;
}

// Deliver vector vec to the guest, as the CPU delivers an exception or
// interrupt in 64-bit mode (SDM 7.14): through the guest's IDT, on the
// guest's kernel stack, in its kernel mode. haserr says whether the
// vector pushes an error code. Returns -1 if the guest has no handler
// for it or no usable stack.
static int
pvtrap(int vec, int haserr, uint64 err)
{
  struct gatedesc g;
  uint64 f[6], rsp, rip;
  int n;

  /* HW3 Part 4: write this function; see HW3.md. */
  return -1;
}

// HC_IRET: return from a trap, as IRETQ would. The guest's stack holds
// rax, rip, cs, rflags, rsp and ss (pvapi.h).
static int
pviret(void)
{
  uint64 f[6];

  /* HW3 Part 4: write this function; see HW3.md. */
  return -1;
}

// Deliver the highest pending interrupt, if the guest has interrupts
// enabled (a local APIC also picks the highest vector), and tell the
// guest whether more are waiting, so that its sti() asks for them.
static int
pvintr(void)
{
  int irq;

  /* HW3 Part 4: write this function; see HW3.md. */
  return 0;
}

// A hypercall (pvapi.h). Returns 1 if runpv must handle it (pv.exit
// says how), -1 if the guest asked for something it cannot have, and 0
// otherwise.
static int
pvhcall(void)
{
  struct trapframe *tf;
  uint64 gpa;
  int perm;

  // The gate has DPL 3, so the guest's user programs can reach it
  // too. Answer as the CPU does for an INT through a gate they may not
  // use: a general-protection fault, whose error code names the IDT
  // entry (SDM 7.13, 7.12.1.1).
  if(pv.user)
    return pvtrap(T_GPFLT, 1, T_HCALL * 8 + 2);
  pv.nhcall++;
  tf = &pv.tf;
  switch(tf->rax){
  case HC_CONSPUTC:
    consoleputc(tf->rdi & 0xFF);
    tf->rax = 0;
    break;
  case HC_CONSGETC:
    acquire(&pvlock);
    if(pv.inr == pv.inw)
      tf->rax = -1;
    else
      tf->rax = (uchar)pv.in[pv.inr++ % sizeof(pv.in)];
    release(&pvlock);
    break;
  case HC_VIOREAD:
  case HC_VIOWRITE:
    // The disk is runpv's. For a read, it puts the value in rax with
    // pvsetreg() before it runs the guest again.
    pv.exit.reason = PVEXIT_VIO;
    pv.exit.write = tf->rax == HC_VIOWRITE;
    pv.exit.off = tf->rdi;
    pv.exit.val = tf->rsi;
    tf->rax = 0;
    pv.nuser++;
    return 1;
  case HC_LCR3:
    if(tf->rdi % PGSIZE != 0 || tf->rdi >= PV_MEM)
      return -1;
    pv.cr3 = tf->rdi;
    // The guest's TLB is the shadow: a new page table empties it.
    pvflush();
    pv.nflush++;
    tf->rax = 0;
    break;
  case HC_LIDT:
    pv.idt = tf->rdi;
    tf->rax = 0;
    break;
  case HC_SETKSTACK:
    pv.kstack = tf->rdi;
    tf->rax = 0;
    break;
  case HC_SETVCPU:
    // We keep a pointer into the guest's page; guest memory never moves.
    if(tf->rdi % PGSIZE + sizeof(struct pvcpu) > PGSIZE ||
       pvwalk(tf->rdi, &gpa, &perm) < 0)
      return -1;
    pv.vcpu = (struct pvcpu*)(pv.mem[gpa / PGSIZE] + gpa % PGSIZE);
    tf->rax = 0;
    break;
  case HC_IRET:
    return pviret();
  case HC_STI:
    // Nothing to do: pvloop() delivers pending interrupts at every entry.
    tf->rax = 0;
    break;
  default:
    return -1;
  }
  return 0;
}

// Handle the guest's last exit, whose trap frame is in pv.tf. Returns
// 0 to run the guest again, 1 to return to runpv, -1 to stop it.
static int
pvhandle(void)
{
  uint64 trapno, err;

  trapno = pv.tf.trapno;
  err = pv.tf.err;
  pv.nexit++;
  // A host device interrupted the guest, and trap() has handled it. At
  // a timer interrupt we give up the CPU, as trap() does for a user
  // process; we would hold it as long as the guest runs otherwise.
  if(trapno >= T_IRQ0 && trapno < T_IRQ0 + 32){
    if(trapno == T_IRQ0 + IRQ_TIMER)
      yield();
    return 0;
  }
  if(trapno == T_HCALL)
    return pvhcall();
  if(trapno == T_PGFLT){
    if(pvfill(pv.cr2, err) == 0)
      return 0;
    // The guest's own fault. It finds the address with rcr2().
    if(pv.vcpu)
      pv.vcpu->cr2 = pv.cr2;
    // The CPU set U/S (bit 2) because we run the guest in ring 3; the
    // guest's trap() must see the guest's mode.
    err = (err & ~4ULL) | (pv.user ? 4 : 0);
  }
  // Everything else is the guest's: an exception, or a system call
  // (INT $64) from its user mode. Deliver it as the CPU would have.
  return pvtrap(trapno, HASERR(trapno), err);
}

// Run the guest until runpv must act. Returns 0 then, with the reason
// in pv.exit, or -1 if the guest failed or we were killed.
static int
pvloop(void)
{
  struct proc *p;
  int r;

  p = myproc();
  for(;;){
    if(p->killed)
      return -1;
    acquire(&pvlock);
    if(pv.stop){
      release(&pvlock);
      pv.exit.reason = PVEXIT_STOP;
      return 0;
    }
    // The guest's timer: one interrupt per host tick, as in HW2.
    if(ticks != pv.lastticks){
      pv.lastticks = ticks;
      pv.pending |= 1 << IRQ_TIMER;
    }
    release(&pvlock);
    if(pvintr() < 0){
      cprintf("pv: cannot deliver an interrupt (rip 0x%x)\n", pv.tf.rip);
      return -1;
    }

    // Enter the guest as trapret enters user space: with interrupts off
    // until IRETQ, but not with pushcli(), since the guest then runs
    // like user code, with ncli 0. A trap from it may come through the
    // host's system-call gate, which leaves interrupts on.
    cli();
    lcr3(V2P(pv.shadow[pv.user]));
    p->inguest = 1;
    pv_enter(&pv.tf, &pv.hostrsp, &mycpu()->ts);
    // pvexit() comes back here, with interrupts off and the guest's
    // registers in pv.tf. Undo what pv_enter() changed.
    lcr3(V2P(p->pml4));
    mycpu()->ts.rsp0_31_0 = (uint)((uint64)p->kstack + KSTACKSIZE);
    mycpu()->ts.rsp0_63_32 = (uint)(((uint64)p->kstack + KSTACKSIZE) >> 32);
    sti();

    if((r = pvhandle()) < 0){
      cprintf("pv: guest stopped: trap %d, rip 0x%x, rax 0x%x\n",
              (int)pv.tf.trapno, pv.tf.rip, pv.tf.rax);
      return -1;
    }
    if(r > 0)
      return 0;
  }
}

// Free the guest's memory and shadows, and the guest.
static void
pvfree(void)
{
  int i;

  // freepgdir() frees the shadows' page tables but not the pages they
  // map, which are the guest's memory and the host kernel's.
  for(i = 0; i < 2; i++)
    if(pv.shadow[i])
      freepgdir(pv.shadow[i], 4);
  for(i = 0; i < NPVPAGES; i++)
    if(pv.mem[i])
      kfree(pv.mem[i]);
  acquire(&pvlock);
  pvowner = 0;
  release(&pvlock);
}

// The system calls of runpv (pvapi.h).

int
pvcreate(void)
{
  int i;

  acquire(&pvlock);
  if(pvowner){
    release(&pvlock);
    return -1;
  }
  memset(&pv, 0, sizeof(pv));
  pvowner = myproc();
  release(&pvlock);

  for(i = 0; i < NPVPAGES; i++){
    if((pv.mem[i] = kalloc()) == 0)
      goto bad;
    memset(pv.mem[i], 0, PGSIZE);
  }
  // setupkvm() maps the host kernel in the upper half; the guest's
  // half starts empty and fills in as the guest faults.
  if((pv.shadow[0] = setupkvm()) == 0 || (pv.shadow[1] = setupkvm()) == 0)
    goto bad;
  // The guest starts at the rip runpv gives it, in 64-bit mode, in its
  // kernel mode with interrupts off. To the CPU it is user code: ring 3
  // segments and interrupts on.
  pv.tf.cs = (SEG_UCODE << 3) | DPL_USER;
  pv.tf.ss = (SEG_UDATA << 3) | DPL_USER;
  pv.tf.rflags = FL_IF;
  pv.lastticks = ticks;
  return 0;

bad:
  pvfree();
  return -1;
}

int
pvmem(uint64 gpa, char *buf, int len, int flags)
{
  char *g;
  int n;

  if(pvowner != myproc() || len < 0 || gpa >= PV_MEM || len > PV_MEM - gpa)
    return -1;
  // sys_pvmem() checked buf with argptr(), and our page table is loaded,
  // so buf can be used directly, as in vmmem().
  while(len > 0){
    // To the end of the page: the next one may be anywhere in our memory.
    n = PGSIZE - gpa % PGSIZE;
    if(n > len)
      n = len;
    g = pv.mem[gpa / PGSIZE] + gpa % PGSIZE;
    if(flags & PVMEM_WRITE)
      memmove(g, buf, n);
    else
      memmove(buf, g, n);
    len -= n;
    buf += n;
    gpa += n;
  }
  return 0;
}

int
pvsetreg(int reg, uint64 val)
{
  if(pvowner != myproc())
    return -1;
  switch(reg){
  case PVREG_RAX:
    pv.tf.rax = val;
    return 0;
  case PVREG_RIP:
    // As in pviret(): our IRETQ to a non-canonical rip would fault in
    // the host kernel.
    if(val >= KERNBASE)
      return -1;
    pv.tf.rip = val;
    return 0;
  }
  return -1;
}

int
pvinject(int irq)
{
  if(pvowner != myproc() || irq < 0 || irq >= 32)
    return -1;
  acquire(&pvlock);
  pv.pending |= 1U << irq;
  release(&pvlock);
  return 0;
}

int
pvrun(struct pvexit *e)
{
  if(pvowner != myproc() || pvloop() < 0)
    return -1;
  // sys_pvrun() checked e with argptr().
  *e = pv.exit;
  return 0;
}

// Called by exit(): free p's guest, if it has one. p is not in
// pvrun(), so the guest is not running.
void
pvfreeproc(struct proc *p)
{
  if(pvowner != p)
    return;
  cprintf("pv: %d exits: %d hypercalls (%d CR3 loads, %d to runpv), "
          "%d shadow fills, %d traps delivered\n", pv.nexit, pv.nhcall,
          pv.nflush, pv.nuser, pv.nfill, pv.ntrap);
  pvfree();
}

// Called by trap() for every trap from the guest: the guest's exit. It
// does not return: pv_exit() goes back to pvloop(), into pv_enter()'s
// caller, on the stack pv_enter() saved.
void
pvexit(struct trapframe *tf)
{
  // The guest's system calls come through the host's system-call gate,
  // a trap gate, with interrupts on.
  cli();
  myproc()->inguest = 0;
  pv.tf = *tf;
  if(tf->trapno == T_PGFLT)
    pv.cr2 = rcr2();
  pv_exit(pv.hostrsp);
}

// Called by consoleintr() for each typed character. While a guest runs,
// the console is the guest's: keep the character for HC_CONSGETC and
// raise the guest's COM1 interrupt; Ctrl-] stops the guest. Returns 0
// if no guest runs.
int
pvconsole(int c)
{
  acquire(&pvlock);
  if(pvowner == 0){
    release(&pvlock);
    return 0;
  }
  if(c == QUITKEY)
    pv.stop = 1;
  else if(pv.inw - pv.inr < sizeof(pv.in)){
    pv.in[pv.inw++ % sizeof(pv.in)] = c;
    pv.pending |= 1 << IRQ_COM1;
  }
  release(&pvlock);
  return 1;
}
