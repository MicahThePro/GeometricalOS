; 32-bit entry, copied to physical 0x7000. CS is already a 32-bit segment.
bits 32
org 0x7000

mov ax, 0x10
mov ds, ax
mov es, ax
mov fs, ax
mov gs, ax
mov ss, ax
mov esp, 0x90000
jmp 0x12000
