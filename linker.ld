ENTRY(_start)

SECTIONS
{
    . = 2M;
    .text : ALIGN(8) {
        mb2_header_start = .;
        KEEP(*(.multiboot))
        _text_start = .;
        *(.text*)
        _text_end = .;
    }
    
    .rodata : ALIGN(4K) {
        _rodata_start = .;
        *(.rodata*)
        _rodata_end = .;
    }
    
    .data : ALIGN(4K) {
        _data_start = .;
        *(.data*)
        _data_end = .;
    }
    
    .bss : ALIGN(4K) {
        *(COMMON)
        _bss_start = .;
        *(.bss*)
        _bss_end = .;
    }
    
    /* Simbolo che indica la fine del kernel */
    _kernel_end = .;
}