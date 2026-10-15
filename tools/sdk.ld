/* op1-sdk: everything in one contiguous image at the load address (one LDR data block).
  * .bss is folded into the loaded image so the boot loader zeroes it (no startup code needed).
  * The trailing LONG(0) keeps the output PROGBITS even when the tail is all .bss,
  * so objcopy -O binary does not truncate it. */
ENTRY(_sdk_entry)
SECTIONS
{
  . = SDK_LOAD;
  .text   : { KEEP(*(.text.entry)) *(.text .text.*) }
  .rodata : ALIGN(4) { *(.rodata .rodata.*) }
  .data   : ALIGN(4) { *(.data .data.*) *(.bss .bss.*) *(COMMON) . = ALIGN(4); LONG(0); }
  _sdk_end = .;
  /DISCARD/ : { *(.comment) *(.note*) }
}
