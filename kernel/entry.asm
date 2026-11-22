[bits 32]
section .note.GNU-stack noalloc noexec nowrite progbits
section .text.entry
global _start
extern kmain
extern bss_start
extern bss_end

_start:
    cld
    mov edi, bss_start
    mov ecx, bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb
    mov esp, 0x90000
    and esp, 0xFFFFFFF0
    call kmain
.hang:
    cli
    hlt
    jmp .hang

global irq1_stub
extern irq1_handler
irq1_stub:
    pushad
    cld
    call irq1_handler
    popad
    iret

global irq_eoi_stub
irq_eoi_stub:
    push eax
    mov al, 0x20
    out 0x20, al
    pop eax
    iret
