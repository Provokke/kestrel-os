; Interrupt/exception entry stubs, descriptor-table loaders, context switch
; and the ring-3 entry trampoline.
section .text
extern isr_dispatch

%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

; exceptions that push an error code: 8, 10-14, 17, 21, 29, 30
%assign i 0
%rep 32
  %if i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30
    ISR_ERR i
  %else
    ISR_NOERR i
  %endif
  %assign i i+1
%endrep
%assign i 32
%rep 16
    ISR_NOERR i
  %assign i i+1
%endrep
ISR_NOERR 128

isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp            ; struct regs *
    call isr_dispatch
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8          ; int_no, err_code
    iret

global gdt_flush
gdt_flush:                ; void gdt_flush(void *gdtr)
    mov eax, [esp + 4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.reload
.reload:
    ret

global tss_flush
tss_flush:
    mov ax, 0x28
    ltr ax
    ret

global idt_flush
idt_flush:
    mov eax, [esp + 4]
    lidt [eax]
    ret

; void switch_context(uint32_t *old_esp, uint32_t new_esp)
; Saves callee-saved registers on the current stack, stores the stack pointer
; into *old_esp, loads new_esp and pops the next thread's registers.
global switch_context
switch_context:
    push ebp
    push ebx
    push esi
    push edi
    mov eax, [esp + 20]
    mov [eax], esp
    mov esp, [esp + 24]
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; First code a new kernel thread runs (switch_context "returns" here).
extern thread_start_c
global thread_trampoline
thread_trampoline:
    call thread_start_c   ; never returns
    ud2

; void enter_usermode(uint32_t eip, uint32_t esp, uint32_t arg)
global enter_usermode
enter_usermode:
    cli
    mov ebx, [esp + 4]    ; eip
    mov ecx, [esp + 8]    ; user esp
    mov edx, [esp + 12]   ; arg -> eax in ring 3
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push dword 0x23       ; ss
    push ecx              ; esp
    pushfd
    pop eax
    or eax, 0x200         ; IF=1 in user mode
    push eax
    push dword 0x1b       ; cs
    push ebx              ; eip
    mov eax, edx
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    iret

section .rodata
global user_bin_start, user_bin_end
user_bin_start:
    incbin USER_BIN
user_bin_end:
