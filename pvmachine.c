// HW3: the machine under a paravirtualized guest kernel.
//
// The guest kernel, xv6pvkernel, is xv6 built with -DPARAVIRT. It runs
// in ring 3 on the hypervisor in pv.c, which gives it one CPU, memory,
// interrupts and a console, but none of a PC's hardware. The Makefile
// builds it without the files that drive that hardware (mp.c, acpi.c,
// picirq.c, ioapic.c, uart.c and ide.c), and with this file and
// virtio_disk.c in their place. The functions here keep the names and
// the jobs of the ones they replace, so the rest of xv6 does not change.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

struct cpu cpus[NCPU];
int ncpu;

// What the guest kernel shares with the hypervisor (pvapi.h). 16 bytes,
// aligned to 16, so it does not cross a page.
__attribute__((__aligned__(16)))
volatile struct pvcpu pvcpu;

// In place of mp.c and acpi.c: there are no MP or ACPI tables to read.
// The guest has one virtual CPU and no local APIC: lapic stays 0, so
// lapic.c does nothing, and lapicid() returns 0, cpus[0]'s APIC ID.
// The virtual CPU's interrupt flag is in pvcpu; tell the hypervisor
// where it is.
void
mpinit(void)
{
  ncpu = 1;
  hypercall(HC_SETVCPU, (uint64)&pvcpu, 0, 0);
}

// In place of picirq.c and ioapic.c: there are no interrupt
// controllers to program. The hypervisor raises the guest's interrupt
// lines itself (pvintr() in pv.c).
void
picinit(void)
{
}

void
ioapicinit(void)
{
}

void
ioapicenable(int irq, int cpu)
{
}

// In place of uart.c: the console is the hypervisor's, reached with
// hypercalls. A typed character raises IRQ_COM1, as on the serial
// port, and trap() calls uartintr().
void
uartinit(void)
{
}

void
uartputc(int c)
{
  hypercall(HC_CONSPUTC, c, 0, 0);
}

// The next typed character, or -1 if there is none.
static int
pvgetc(void)
{
  return hypercall(HC_CONSGETC, 0, 0, 0);
}

void
uartintr(void)
{
  consoleintr(pvgetc);
}
