OUTPUT_FORMAT("elf32-bigmips")
OUTPUT_ARCH(mips)
ENTRY(_start)
SECTIONS
{
    . = 0x80000400;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) *(.sdata*) }
    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.sbss*) *(.bss*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;
    /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) }
}
