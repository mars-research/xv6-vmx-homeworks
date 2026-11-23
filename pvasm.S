# HW3: entering and leaving a paravirtualized guest (pv.c).
#
# These play the parts of vmx_enter and vmx_exit (vmxasm.S). Without
# VT-x, our VM entry is an IRETQ to ring 3, the way the kernel returns
# to user space, and our VM exit is any trap from ring 3, which arrives
# through the IDT at trap(), and from there at pvexit() and pv_exit.

# sizeof(struct trapframe) in x86.h: 22 eight-byte slots.
#define TFSIZE 176

# void pv_enter(struct trapframe *tf, uint64 *hostrsp, struct taskstate *ts)
#
# Run the guest with the registers in *tf. Returns when pv_exit() is
# called with *hostrsp, after the guest's next trap.
.globl pv_enter
pv_enter:
  # Save the callee-saved registers, which pv_exit restores, and the
  # stack pointer that leads back to them.
  pushq %rbp
  pushq %rbx
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15
  movq %rsp, (%rsi)

  # A trap from ring 3 switches to the stack in TSS.RSP0, normally the
  # top of the process's kernel stack, where our callers' frames are.
  # Point it just below ours, aligned to 16 bytes as the CPU aligns it
  # (SDM 7.14.2), so that the guest's trap frames do not overwrite them.
  # RSP0 is two 32-bit halves at offset 4 of the TSS (mmu.h).
  movq %rsp, %rax
  andq $-16, %rax
  movq %rax, %rsp
  movl %eax, 4(%rdx)
  shrq $32, %rax
  movl %eax, 8(%rdx)

  # Enter the guest as trapret returns to user space: copy *tf into a
  # trap frame right below RSP0, where the guest's next trap frame will
  # be, and pop it.
  subq $TFSIZE, %rsp
  movq %rdi, %rsi
  movq %rsp, %rdi
  movq $(TFSIZE/8), %rcx
  cld
  rep movsq
  jmp trapret

# void pv_exit(uint64 hostrsp)
#
# Return from pv_enter: back to the stack it saved, abandoning the trap
# frames below it.
.globl pv_exit
pv_exit:
  movq %rdi, %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rbx
  popq %rbp
  ret
