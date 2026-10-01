/* Link the hello-world guest at VM_LOAD_ADDR (vmxapi.h). */
/*
 * The guest is a flat binary: the Makefile runs objcopy -O binary on
 * the linked ELF, so the file's first byte is placed at guest-physical
 * address VM_LOAD_ADDR by vmcreate(), and runvm starts the guest with
 * RIP = VM_LOAD_ADDR. The addresses assigned below must therefore be
 * the real guest-physical addresses the code runs at. The page tables
 * and the GDT in helloguestasm.S contain absolute addresses of
 * their own labels, which only work if this link address is right.
 *
 * KEEP IN SYNC: 0x100000 here must equal VM_LOAD_ADDR in vmxapi.h.
 * (1 MB, the traditional start of "extended memory", above the PC's
 * low-memory area.)
 */

/* Output an ELF; objcopy turns it into the flat binary later. */
OUTPUT_FORMAT("elf64-x86-64")
/* Entry symbol, in helloguestasm.S. Informational only: the monitor
 * does not read ELF headers, it jumps to the first byte of the image. */
ENTRY(start)

SECTIONS
{
  /* Location counter: everything below is placed from here upward. */
  . = 0x100000;
  /* Code. .text.start (the trampoline, label "start") goes first so it
   * is at exactly VM_LOAD_ADDR, the guest's initial RIP. */
  .text : { *(.text.start) *(.text .text.*) }
  /* Read-only data, e.g. the "Hello..." string. */
  .rodata : { *(.rodata .rodata.*) }
  /* Initialized data: the page tables and GDT in helloguestasm.S. */
  .data : { *(.data .data.*) }
  /* Zero-initialized data, including the guest stack. Since objcopy
   * emits it last, it adds nothing to the file; guest memory is zeroed
   * by vmcreate() anyway. */
  .bss : { *(.bss .bss.*) *(COMMON) }
  /* Unwind tables, notes and compiler ident are not needed. */
  /DISCARD/ : { *(.eh_frame) *(.note*) *(.comment) }
}
