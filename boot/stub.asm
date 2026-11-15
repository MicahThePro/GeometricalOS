; Real-mode entry for GeometricalOS.
; The whole image (this stub, padded to 8 KiB, plus the kernel) is loaded at
; physical 0x10000, so CS = 0x1000 and IP = 0 on El Torito entry.
; Assembled with org 0 and concatenated in front of the protected-mode kernel.

[bits 16]
[org 0]

_start:
    jmp short relocate
    nop
    times 8 - ($ - $$) db 0
    ; El Torito / xorriso boot-info table (patched in the ISO). Bytes 8..23.
bi_pvd:    dd 0
bi_file:   dd 0
bi_length: dd 0
bi_csum:   dd 0
    ; Byte count of the whole image, patched by the Makefile. Not part of the table.
image_bytes: dd 0
    times 64 - ($ - $$) db 0

; BIOS enters at the load segment with IP = 0. Copy the image to 0x10000 if
; it was not already loaded there, then continue with CS = 0x1000.
relocate:
    cli
    cld
    xor eax, eax
    mov ax, cs
    shl eax, 4
    cmp eax, 0x10000
    je already_loaded

    mov bx, image_bytes
    mov ecx, [cs:bx]
    add ecx, 511
    and ecx, 0xFFFFFE00
    test ecx, ecx
    jz already_loaded

    mov esi, eax
    mov edi, 0x10000
    xor ax, ax
    mov ss, ax
    mov sp, 0x700

.loop:
    test ecx, ecx
    jz .copied
    mov dword [ss:0x500], esi
    mov dword [ss:0x504], edi
    mov dword [ss:0x508], ecx

    mov eax, esi
    shr eax, 4
    mov ds, ax
    xor si, si
    mov ax, 0x0080
    mov es, ax
    xor di, di
    mov cx, 256
    rep movsw

    mov eax, [ss:0x504]
    shr eax, 4
    mov es, ax
    xor di, di
    mov ax, 0x0080
    mov ds, ax
    xor si, si
    mov cx, 256
    rep movsw

    mov esi, [ss:0x500]
    mov edi, [ss:0x504]
    mov ecx, [ss:0x508]
    add esi, 512
    add edi, 512
    sub ecx, 512
    jmp .loop

.copied:
    jmp 0x1000:already_loaded

already_loaded:
    cli
    mov ax, cs
    mov ds, ax
    mov es, ax
    xor ax, ax
    mov ss, ax
    mov sp, 0x5000
    sti
    cld

    call serial_init
    call sputc_A

    ; Preserve the BIOS boot drive, then consume the HDD-boot signature.
    mov [boot_drive], dl
    xor ax, ax
    mov es, ax
    mov eax, [es:0x0500]
    cmp eax, 0x314F4547
    jne .not_hdd
    mov byte [from_hdd], 1
    mov dword [es:0x0500], 0
    jmp .after_flag
.not_hdd:
    mov byte [from_hdd], 0
.after_flag:
    mov ax, cs
    mov es, ax

    call sputc_B
    call copy_font
    call sputc_C
    call set_video
    cmp byte [video_ok], 1
    je .video_good
    call video_fail
.video_good:
    call sputc_D
    call enable_a20
    call fill_bootinfo
    call enter_pm

; ---------------------------------------------------------------------------
; Serial (COM1, 115200 8N1) so a failed boot is visible from QEMU -serial.
serial_init:
    mov dx, 0x3F8 + 1
    xor al, al
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8
    mov al, 0x01
    out dx, al
    mov dx, 0x3F8 + 1
    xor al, al
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 0x03
    out dx, al
    mov dx, 0x3F8 + 2
    mov al, 0xC7
    out dx, al
    mov dx, 0x3F8 + 4
    mov al, 0x0B
    out dx, al
    ret

sputc:
    push dx
    push ax
    mov dx, 0x3F8 + 5
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    pop ax
    mov dx, 0x3F8
    out dx, al
    pop dx
    ret

sputc_A:
    mov al, 'A'
    call sputc
    ret
sputc_B:
    mov al, 'B'
    call sputc
    ret
sputc_C:
    mov al, 'C'
    call sputc
    ret
sputc_D:
    mov al, 'D'
    call sputc
    ret

; Print AX as four hex characters.
sput_hex16:
    push ax
    push cx
    mov cx, 4
.nyb:
    rol ax, 4
    push ax
    and al, 0x0F
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .out
.digit:
    add al, '0'
.out:
    call sputc
    pop ax
    loop .nyb
    pop cx
    pop ax
    ret

; ---------------------------------------------------------------------------
copy_font:
    push es
    mov ax, 0x1130
    mov bh, 0x06                 ; 8x16 VGA font
    xor bl, bl
    int 0x10
    mov ax, cs
    mov ds, ax
    ; ES:BP = font, CX = bytes per character
    mov ax, cx
    mov [font_height], ax
    cmp ax, 8
    jb .done
    cmp ax, 16
    jbe .hok
    mov ax, 16
.hok:
    mov [font_height], ax
    ; Copy 256 glyphs.
    push ds
    mov ax, es
    mov ds, ax
    mov si, bp
    xor ax, ax
    mov es, ax
    mov di, 0x8000
    mov cx, [cs:font_height]
    shl cx, 8                    ; * 256
    rep movsb
    pop ds
.done:
    pop es
    mov ax, cs
    mov es, ax
    ret

; ---------------------------------------------------------------------------
set_video:
    mov byte [video_ok], 0
    xor ax, ax
    mov es, ax
    mov di, 0x2000
    mov dword [es:di], 0x32454256    ; 'VBE2'
    mov ax, 0x4F00
    int 0x10
    mov bx, cs
    mov ds, bx
    cmp ax, 0x004F
    jne .no_list

    ; Mode list far pointer at offset 0x0E.
    mov si, [es:0x2000 + 14]
    mov ax, [es:0x2000 + 16]
    mov fs, ax
    mov word [best_score], 0
    mov word [best_mode], 0

    mov word [mode_guard], 400
.mode_loop:
    dec word [mode_guard]
    jz .list_done
    mov ax, [fs:si]
    cmp ax, 0xFFFF
    je .list_done
    add si, 2
    call consider_mode
    jmp .mode_loop

.no_list:
    ; Scan the common VBE mode number range.
    mov cx, 0x100
.scan:
    mov ax, cx
    call consider_mode
    inc cx
    cmp cx, 0x200
    jb .scan

.list_done:
    cmp word [best_mode], 0
    je .fail
    mov al, 'M'
    call sputc
    mov ax, [best_mode]
    call sput_hex16
    mov bx, [best_mode]
    or bx, 0x4000                ; linear framebuffer, clear display
    mov ax, 0x4F02
    int 0x10
    push ax
    mov ax, cs
    mov ds, ax
    pop ax
    cmp ax, 0x004F
    jne .fail
    ; Re-read the chosen mode so pitch/LFB match what was set.
    mov cx, [best_mode]
    call read_mode_info
    mov byte [video_ok], 1
    ret
.fail:
    mov byte [video_ok], 0
    ret

; AX = mode number. Preserves CX, FS, and SI.
consider_mode:
    push si
    push fs
    push cx
    mov [cand_mode], ax
    mov cx, ax
    call read_mode_info
    cmp ax, 0x004F
    jne .skip
    xor ax, ax
    mov es, ax
    ; Supported graphics mode with a linear framebuffer.
    mov al, [es:0x3000]
    test al, 0x01
    jz .skip
    test al, 0x10
    jz .skip
    test al, 0x80
    jz .skip
    mov al, [es:0x3000 + 0x1B]
    cmp al, 6                    ; direct-color packed pixel
    jne .skip
    mov al, [es:0x3000 + 0x19]   ; bpp
    mov bl, 0
    cmp al, 32
    je .b32
    cmp al, 24
    je .b24
    cmp al, 16
    je .b16
    cmp al, 15
    je .b15
    jmp .skip
.b32:
    mov bl, 40
    jmp .have_bpp
.b24:
    mov bl, 30
    jmp .have_bpp
.b16:
    mov bl, 20
    jmp .have_bpp
.b15:
    mov bl, 10
.have_bpp:
    mov ax, [es:0x3000 + 0x12]
    mov dx, [es:0x3000 + 0x14]
    cmp ax, 800
    jne .c1024
    cmp dx, 600
    jne .c1024
    add bl, 50
    jmp .score
.c1024:
    cmp ax, 1024
    jne .c640
    cmp dx, 768
    jne .c640
    add bl, 45
    jmp .score
.c640:
    cmp ax, 640
    jne .cother
    cmp dx, 480
    jne .cother
    add bl, 40
    jmp .score
.cother:
    cmp ax, 640
    jb .skip
    cmp dx, 480
    jb .skip
    cmp ax, 1280
    ja .skip
    cmp dx, 1024
    ja .skip
    add bl, 5
.score:
    movzx ax, bl
    cmp ax, [best_score]
    jbe .skip
    mov [best_score], ax
    mov ax, [cand_mode]
    mov [best_mode], ax
.skip:
    pop cx
    pop fs
    pop si
    mov ax, cs
    mov ds, ax
    ret

; CX = mode. Returns AX = 0x004F on success. Mode info at 0000:0x3000.
read_mode_info:
    push cx
    push di
    push es
    xor ax, ax
    mov es, ax
    mov di, 0x3000
    mov ax, 0x4F01
    int 0x10
    pop es
    pop di
    pop cx
    mov bx, cs
    mov ds, bx
    ret

video_fail:
    mov si, failmsg
.p:
    mov al, [cs:si]
    inc si
    test al, al
    jz .h
    mov ah, 0x0E
    xor bx, bx
    mov bl, 0x07
    int 0x10
    jmp .p
.h:
    mov al, 'F'
    call sputc
.hang:
    cli
    hlt
    jmp .hang

failmsg:
    db "GeometricalOS needs a VESA linear framebuffer (640x480 or larger).", 13, 10
    db "In VirtualBox use VBoxVGA or VBoxSVGA with 3D acceleration off.", 13, 10, 0

; ---------------------------------------------------------------------------
enable_a20:
    in al, 0x92
    or al, 00000010b
    and al, 11111110b
    out 0x92, al
    ret

fill_bootinfo:
    cld
    xor ax, ax
    mov es, ax
    mov di, 0x6000
    mov ecx, 64
    xor eax, eax
    rep stosd
    mov di, 0x6000
    mov dword [es:di], 0xB007DA7A
    mov al, [from_hdd]
    movzx eax, al
    mov [es:di + 4], eax
    mov ax, [es:0x3000 + 0x12]
    movzx eax, ax
    mov [es:di + 8], eax          ; width
    mov ax, [es:0x3000 + 0x14]
    movzx eax, ax
    mov [es:di + 12], eax         ; height
    mov ax, [es:0x3000 + 0x10]
    movzx eax, ax
    mov [es:di + 16], eax         ; pitch
    mov al, [es:0x3000 + 0x19]
    movzx eax, al
    mov [es:di + 20], eax         ; bpp
    mov eax, [es:0x3000 + 0x28]
    mov [es:di + 24], eax         ; lfb
    mov al, [es:0x3000 + 0x1F]
    movzx eax, al
    mov [es:di + 28], eax         ; red size
    mov al, [es:0x3000 + 0x20]
    movzx eax, al
    mov [es:di + 32], eax         ; red pos
    mov al, [es:0x3000 + 0x21]
    movzx eax, al
    mov [es:di + 36], eax
    mov al, [es:0x3000 + 0x22]
    movzx eax, al
    mov [es:di + 40], eax
    mov al, [es:0x3000 + 0x23]
    movzx eax, al
    mov [es:di + 44], eax
    mov al, [es:0x3000 + 0x24]
    movzx eax, al
    mov [es:di + 48], eax
    mov ah, 0x88
    int 0x15
    movzx eax, ax
    mov [es:di + 52], eax         ; extended memory KB
    mov dword [es:di + 56], 0x8000
    mov ax, [font_height]
    movzx eax, ax
    mov [es:di + 60], eax
    mov ax, cs
    mov es, ax
    ret

enter_pm:
    lgdt [gdtr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword 0x08:(pm_entry + 0x10000)

[bits 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    mov dx, 0x3F8 + 5
.wait_tx:
    in al, dx
    test al, 0x20
    jz .wait_tx
    mov dx, 0x3F8
    mov al, 'P'
    out dx, al
    ; Near jumps are relative to the stub's org 0, so use an absolute register jump.
    mov eax, 0x12000
    jmp eax

[bits 16]
align 8
gdt:
    dq 0
    dw 0xFFFF, 0
    db 0, 10011010b, 11001111b, 0
    dw 0xFFFF, 0
    db 0, 10010010b, 11001111b, 0
gdt_end:

gdtr:
    dw gdt_end - gdt - 1
    dd gdt + 0x10000

boot_drive:   db 0
from_hdd:     db 0
video_ok:     db 0
best_mode:    dw 0
best_score:   dw 0
cand_mode:    dw 0
mode_guard:   dw 0
font_height:  dw 16

times 8192 - ($ - $$) db 0
