; GeometricalOS hard-disk master boot record.
; Loaded by the BIOS at 0000:7C00. Reads the kernel image with INT 13h
; extensions (LBA) to physical 0x10000 and jumps to it.
; The installer patches the GEOL parameter block.

[bits 16]
[org 0x7C00]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    mov [drive], dl

    ; Tell the kernel it was launched from the hard disk, not the CD.
    mov dword [0x0500], 0x314F4547   ; 'GEO1'

    mov eax, [kernel_lba]
    mov cx, [kernel_count]
    mov bx, 0x1000                   ; destination segment (0x10000)

.read_loop:
    test cx, cx
    jz .jump
    mov dx, cx
    cmp dx, 32                       ; 16 KiB chunks, safe for BIOS
    jbe .count_ok
    mov dx, 32
.count_ok:
    mov [chunk], dx

    mov word [dap], 0x0010
    mov word [dap + 2], dx
    mov word [dap + 4], 0
    mov word [dap + 6], bx
    mov dword [dap + 8], eax
    mov dword [dap + 12], 0

    pusha
    mov si, dap
    mov dl, [drive]
    mov ah, 0x42
    int 0x13
    jc .fail
    popa

    movzx edx, word [chunk]
    add eax, edx
    mov dx, [chunk]
    shl dx, 5                        ; segments: sectors * 512 / 16
    add bx, dx
    mov dx, [chunk]
    sub cx, dx
    jmp .read_loop

.jump:
    jmp 0x1000:0x0000

.fail:
    mov si, msg
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .print
.halt:
    cli
    hlt
    jmp .halt

msg: db "GeometricalOS: disk read failed. Use an IDE disk.", 0

drive: db 0
chunk: dw 0

align 4
    db "GEOL"
kernel_lba:   dd 64
kernel_count: dw 0

dap: times 16 db 0

times 446 - ($ - $$) db 0
part: times 64 db 0
dw 0xAA55
