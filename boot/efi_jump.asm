; MS x64: RCX is the page-table root. Does not return.
bits 64
section .text
global efi_jump_low

efi_jump_low:
    mov rax, rcx
    mov rsp, 0x80000
    mov rdx, 0x7100
    jmp rdx
