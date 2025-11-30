; Multiboot (v1) entry point. QEMU's -kernel loader hands us 32-bit protected
; mode with paging off; we set up a stack and jump to C.
MB_MAGIC    equ 0x1BADB002
MB_FLAGS    equ (1 << 0) | (1 << 1)      ; page-align modules, provide memory info
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM

section .bss
align 16
global boot_stack_bottom, boot_stack_top
boot_stack_bottom:
    resb 16384
boot_stack_top:

section .text
global _start
extern kmain
_start:
    cli
    mov esp, boot_stack_top
    xor ebp, ebp
    push ebx            ; multiboot info pointer
    push eax            ; multiboot magic
    call kmain
.hang:
    cli
    hlt
    jmp .hang
