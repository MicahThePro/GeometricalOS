; BIOS boot sector for a raw (DD) copy of the ISO onto a USB stick.
; Relocates to 0x0600, finds the El Torito catalog, and loads the first 8 KiB
; of the BIOS boot image to 0x7C00. LBA reads, so a 64 GB stick is fine.
bits 16
org 0x0600

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov si, 0x7C00
    mov di, 0x0600
    mov cx, 256
    cld
    rep movsw
    jmp 0x0000:main

main:
    mov [drive], dl
    sti
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [drive]
    int 0x13
    jc fail
    cmp bx, 0xAA55
    jne fail

    mov eax, 68
    mov bx, 0x1000
    mov cx, 1
    call read_lba
    jc fail
    cmp dword [0x1001], 0x30304443
    jne fail
    mov eax, [0x1047]
    shl eax, 2
    mov bx, 0x1200
    mov cx, 1
    call read_lba
    jc fail
    cmp byte [0x1220], 0x88
    jne fail
    mov eax, [0x1228]
    shl eax, 2
    mov bx, 0x7C00
    mov cx, 16
    call read_lba
    jc fail
    mov dl, [drive]
    jmp 0x0000:0x7C00

fail:
    mov ax, 0xB800
    mov es, ax
    xor di, di
    mov si, msg
.p:
    lodsb
    test al, al
    jz .h
    mov ah, 0x07
    stosw
    jmp .p
.h:
    cli
    hlt
    jmp .h

; EAX = 512-byte LBA, ES:BX = buffer, CX = sector count. CF on error.
read_lba:
    pushad
    mov byte [pkt], 16
    mov byte [pkt + 1], 0
    mov [pkt + 2], cx
    mov [pkt + 4], bx
    mov word [pkt + 6], 0
    mov [pkt + 8], eax
    mov dword [pkt + 12], 0
    mov si, pkt
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    popad
    ret

drive: db 0
pkt: times 16 db 0
msg: db "GeometricalOS: USB BIOS read failed", 0

times 440 - ($ - $$) db 0
