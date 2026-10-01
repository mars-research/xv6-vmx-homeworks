# Trampoline for the hello-world VT-x guest.
#
# The hypervisor starts the guest here, at VM_LOAD_ADDR, in flat
# 32-bit protected mode with paging off. Like entry.S, we enable PAE,
# load a page table, set EFER.LME, turn on paging, and far-jump into
# a 64-bit code segment. The page table is static data in the image:
# it identity-maps the first 1 GB with 2 MB pages.
#
# WHY THE GUEST STARTS IN 32-BIT PROTECTED MODE
#
# A real PC starts in 16-bit real mode, and a boot loader (BIOS, GRUB)
# eventually hands the kernel a 32-bit protected-mode CPU, with paging
# off and flat segments. xv6-64's entry.S assumes exactly this state.
# Our hypervisor has no BIOS and no boot loader, so vmcsinit() (vmx.c)
# builds that state directly in the VMCS. VT-x "unrestricted guest" mode
# lets it do so, since it allows CR0.PE = 1 with CR0.PG = 0, which plain
# VMX does not. The initial state is:
#   - CR0 = PE | NE | ET: protected mode, paging off
#   - CS = 0x08 (flat 32-bit code), DS/ES/SS/FS/GS = 0x10 (flat data)
#   - the VMCS GDTR is empty (base 0, limit 0), so we load our own GDT
#   - RFLAGS = 0x2 (only the reserved bit), so interrupts are off
#   - RIP = VM_LOAD_ADDR (set by runvm.c); RSP is not set, so we must
#     not use the stack until start64.
#
# THE TRANSITION TO 64-BIT MODE (needs, in this order)
#   1. CR4.PAE = 1   long mode only works with PAE-format page tables
#   2. CR3 = PML4    the top-level page table (must exist before paging)
#   3. EFER.LME = 1  "long mode enable" (a model-specific register)
#   4. a GDT with a 64-bit code segment (L bit set)
#   5. CR0.PG = 1    turning paging on with LME set activates long mode
#                    (the CPU sets EFER.LMA, "long mode active"); the CPU
#                    is then in 32-bit "compatibility mode"
#   6. far jump to the 64-bit code segment: reloading CS with an L=1
#      descriptor switches to true 64-bit mode.
#
# This is the same sequence as in entry.S. The difference is that WRMSR
# causes a VM exit here (see below).

#include "mmu.h"

# Assemble the next instructions as 32-bit code (default operand size 32).
.code32
# Placed first by helloguest.ld, so "start" is the byte at VM_LOAD_ADDR.
.section .text.start
.globl start
start:
  # Step 1: set CR4.PAE (Physical Address Extension). CR4 cannot be
  # modified directly, so read-modify-write through EAX.
  # (CR4.VMXE is host-owned: the guest reads it as 0, so it is not
  # disturbed by this write.)
  movl    %cr4, %eax
  orl     $(CR4_PAE), %eax
  movl    %eax, %cr4

  # Step 2: point CR3 at the top-level page table (PML4), defined
  # below. Its address is a link-time constant that fits in 32 bits
  # because the guest is loaded low (0x100000 plus a few KB). Nothing
  # is translated yet: paging is still off.
  movl    $pml4, %eax
  movl    %eax, %cr3

  # Step 3: set EFER.LME (long mode enable). EFER is a model-specific
  # register, accessed with RDMSR/WRMSR: ECX selects the MSR, and the
  # 64-bit value is in EDX:EAX. Read it, set the bit, write it back.
  #
  # The hypervisor does not give the guest direct access to MSRs
  # (there is no MSR bitmap), so both RDMSR and WRMSR cause a VM exit.
  # vmx.c (vmxmsr) emulates them: it keeps the guest's EFER in
  # struct vm, copies it into the VMCS before entering the guest, and
  # advances RIP. To the guest, the exits are invisible; the guest just
  # sees the instructions complete. The hypervisor also sets the
  # VM-entry control "IA-32e mode guest" later, when the guest's EFER
  # has LMA set, so that the processor state stays consistent.
  movl    $EFER_MSR, %ecx
  rdmsr                               # EDX:EAX = EFER (exits, emulated)
  orl     $EFER_MSR_LME, %eax
  wrmsr                               # EFER = EDX:EAX (exits, emulated)

  # Step 4: load the GDT register from the descriptor at gdtdesc (limit
  # and 32-bit base). The VMCS gave us an empty GDT, and the far jump
  # below needs a valid 64-bit code descriptor.
  lgdt    gdtdesc

  # Step 5: set CR0.PG to turn on paging. Because CR4.PAE, CR3 and
  # EFER.LME are all set, this activates long mode: EFER.LMA becomes 1
  # (the processor sets it; the hypervisor preserves it). The
  # instruction after this one is still fetched from the same address
  # because the page tables identity-map it.
  movl    %cr0, %eax
  orl     $(CR0_PG), %eax
  movl    %eax, %cr0

  # Now in compatibility mode; reload CS to enter 64-bit mode.
  # A far jump loads CS:EIP. Selector 8 is the 64-bit code descriptor
  # of our GDT (below); since its L bit is set, the CPU switches to
  # 64-bit mode. The target is start64.
  ljmp    $8, $start64

# From here on, assemble as 64-bit code.
.code64
start64:
  # Load the data segment registers with selector 16, the data
  # descriptor. In 64-bit mode DS/ES/SS bases and limits are ignored,
  # but they should hold a valid selector (a null or stale one in SS
  # can fault). FS/GS are left alone; they are unused here.
  movw    $16, %ax
  movw    %ax, %ds
  movw    %ax, %es
  movw    %ax, %ss
  # The stack: set RSP to the top of the 4 KB reserved in .bss below
  # (stacks grow down). Now C code can run.
  movq    $stacktop, %rsp
  # Run the C program (helloguest.c). It prints through the serial port.
  call    guestmain
  # guestmain returned: halt. HLT causes a VM exit that runvm.c reports
  # as "guest halted". The loop guards against the guest being resumed.
1:
  hlt
  jmp     1b

# Initialized data. The page tables must be 4 KB aligned (CR3 and the
# table entries hold physical page numbers).
.data
.p2align 12
# Static, identity-mapped (virtual address = guest-physical address)
# page tables for the first 1 GB, in the 4-level format of long mode:
# PML4 -> PDPT -> PD, with 2 MB pages in the PD (no level-1 tables).
# Entry flag bits: 0x1 present, 0x2 writable, 0x80 PS (this entry maps
# a large page instead of pointing to a lower table).
#
# Level 4, PML4: each entry covers 512 GB. Only entry 0 is used; it
# points to the PDPT. The address is the link-time (= physical) address
# of the table. The other 511 entries are zero (not present).
pml4:
  .quad   pdpt + 0x3              # present, writable
  .fill   511, 8, 0
# Level 3, PDPT: each entry covers 1 GB. Entry 0 points to the PD.
pdpt:
  .quad   pd + 0x3
  .fill   511, 8, 0
# Level 2, PD: each entry covers 2 MB. All 512 entries map 2 MB pages
# with entry i mapping physical addresses i*2 MB .. i*2 MB + 2 MB - 1,
# for a total of 512 * 2 MB = 1 GB. The assembler loop computes
# (i << 21) = i * 2 MB. Since the guest has at most 64 MB (VM_MAXMEM),
# this covers all of its memory, and then some.
pd:
  .set    i, 0
  .rept   512
  .quad   (i << 21) + 0x83        # 2 MB page: present, writable, PS
  .set    i, i + 1
  .endr

# The GDT, with three 8-byte descriptors. The selector is the byte
# offset of the descriptor: 0, 8, 16.
.p2align 3
gdt:
  .quad   0                       # null
  # 0x08: 64-bit code. Access byte 0x9A = present, ring 0, code,
  # readable. Flags byte 0x20 = L (64-bit) bit; D must be 0. Base and
  # limit are ignored in 64-bit mode.
  .quad   0x00209A0000000000      # 0x08: 64-bit code
  # 0x10: data. Access byte 0x92 = present, ring 0, data, writable.
  .quad   0x0000920000000000      # 0x10: data
# The operand of LGDT: a 16-bit limit (size of the GDT minus 1) and the
# address of the GDT. The base is 32 bits because lgdt is executed in
# 32-bit mode.
gdtdesc:
  .word   gdtdesc - gdt - 1
  .long   gdt

# Zero-initialized memory (not stored in the image). 4 KB of stack for
# guestmain; stacktop is its highest address, as the stack grows down.
.bss
.p2align 4
  .space  4096
stacktop:
