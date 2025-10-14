ENTRY(_start)

SECTIONS
{
    . = 0;
    .multiboot : { *(.multiboot) }
    . = 1M;
    
    .text : ALIGN(4K) {
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