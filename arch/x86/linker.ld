ENTRY(_start)
SECTIONS
{
    . = 1M;
    .text ALIGN(4K) : { KEEP(*(.multiboot)) *(.text*) }
    .rodata ALIGN(4K) : { *(.rodata*) }
    .data ALIGN(4K) : {
        *(.data*)
        . = ALIGN(4);
        __start_ktests = .;
        KEEP(*(ktests))
        __stop_ktests = .;
    }
    .bss ALIGN(4K) : { *(COMMON) *(.bss*) }
    . = ALIGN(4K);
    kernel_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
