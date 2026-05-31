/* SecOS user ELF linker script. SPDX-License-Identifier: MIT
 * Code at USER_CODE_BASE (0x100000000), data at USER_DATA_BASE (0x200000000),
 * matching the kernel loader's range checks (mm/elf.c). */
ENTRY(_start)

PHDRS {
  text PT_LOAD   FLAGS(5);   /* R + X */
  note PT_NOTE   FLAGS(4);   /* R     */
  data PT_LOAD   FLAGS(6);   /* R + W */
}

SECTIONS {
  . = 0x100000000;
  .text : { *(.text*) *(.rodata*) } :text
  .note.secos : { KEEP(*(.note.secos)) } :note

  . = 0x200000000;
  .data : { *(.data*) } :data
  .bss  : { *(.bss*) *(COMMON) } :data
}
