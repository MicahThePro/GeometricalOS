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

; BIOS enters with IP = 0. VirtualBox only promises load segment 0x07C0 and may
; not load the whole image, so on CD boot this loader (which must fit in the
; first 2048 bytes) reads the full file to 0x10000 with INT 13h, then jumps
; there. The hard-disk MBR has already loaded the image and set the GEO1 mark.
relocate:
    cli
    cld
    xor ax, ax
    mov ss, ax
    mov sp, 0x7000
    mov ds, ax
    push dx
    ; COM1, so a hang is visible even when the screen is still blank.
    mov dx, 0x3F8 + 3
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8
    mov al, 1
    out dx, al
    mov dx, 0x3F8 + 1
    xor al, al
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 0x03
    out dx, al
    mov al, 'L'
    call early_putc
    ; SeaBIOS rewrites load segment 0x07C0 as 0000:7C00. Rebuild a segment
    ; that actually points at this image before touching CS-relative data.
    call .ip
.ip:
    pop ax
    xor ebx, ebx
    mov bx, cs
    shl ebx, 4
    movzx eax, ax
    add ebx, eax
    sub ebx, (.ip - $$)
    shr ebx, 4
    mov ax, bx
    push ax
    push word after_norm
    retf

after_norm:
    pop dx
    mov [cs:saved_drive], dl
    push dx
    mov al, 'N'
    call early_putc
    ; BIOS disk services scratch 0000:7C00. Run the loader from 0x60000.
    mov ax, cs
    mov ds, ax
    xor si, si
    mov ax, 0x6000
    mov es, ax
    xor di, di
    mov cx, 1024
    rep movsw
    jmp 0x6000:after_move

after_move:
    pop dx
    xor ax, ax
    mov ds, ax
    mov [0x0510], dl
    mov al, 'M'
    call early_putc
    mov es, ax
    sti
    cmp dword [0x0500], 0x314F4547
    jne cd_boot
    jmp 0x1000:already_loaded

cd_boot:
    mov al, 'C'
    call early_putc
    mov al, [cs:saved_drive]
    call early_hex
    mov ax, 0x0003
    int 0x10
    mov si, msg_cd
    call bios_puts
    call boot_prompt
    jnc .continue_cd
    call boot_hard_disk
    mov si, msg_no_hdd
    call bios_puts
.continue_cd:

    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [cs:saved_drive]
    int 0x13
    jnc .ext_ok
    mov al, 'e'
    call early_putc
    mov al, ah
    call early_hex
    jmp .have_lba
.ext_ok:
    cmp bx, 0xAA55
    je .have_lba
    mov al, 'b'
    call early_putc
.have_lba:

    mov eax, [cs:bi_length]
    test eax, eax
    jz cd_fail
    mov [cs:saved_len], eax
    mov al, [cs:0x200]
    mov [cs:saved_mark], al
    mov eax, [cs:bi_file]
    mov [cs:saved_lba], eax

    ; BIOS CD reads are 2048 bytes/sector on SeaBIOS and 512 on the Bochs
    ; BIOS that VirtualBox ships. A one-byte signature cannot tell them apart,
    ; so compare offset 0x200 of a candidate read with this loader.
    mov eax, [cs:saved_lba]
    mov cx, 1
    mov bx, 0x0900
    call read_lba
    mov al, 0
    cmp byte [cs:read_err], 0
    jne .no2048
    cmp byte [0x9000], 0xEB
    jne .no2048
    mov al, [0x9200]
    cmp al, [cs:saved_mark]
    jne .no2048
    mov byte [cs:saved_mode], 1
    mov eax, [cs:saved_lba]
    mov [cs:saved_pos], eax
    mov al, '2'
    call early_putc
    jmp .load
.no2048:
    mov eax, [cs:saved_lba]
    shl eax, 2
    mov [cs:saved_pos], eax
    mov cx, 4
    mov bx, 0x0980
    call read_lba
    cmp byte [cs:read_err], 0
    jne cd_fail
    cmp byte [0x9800], 0xEB
    jne cd_fail
    mov al, [0x9A00]
    cmp al, [cs:saved_mark]
    jne cd_fail
    mov byte [cs:saved_mode], 0
    mov al, '5'
    call early_putc

.load:
    mov eax, [cs:saved_len]
    cmp byte [cs:saved_mode], 0
    jne .sz2048
    add eax, 511
    shr eax, 9
    jmp .have_count
.sz2048:
    add eax, 2047
    shr eax, 11
.have_count:
    cmp ax, 256
    jbe .count_cap
    mov ax, 256
.count_cap:
    mov [cs:saved_count], ax
    xor bp, bp
.chunk:
    cmp bp, [cs:saved_count]
    jae .loaded
    mov ax, bp
    mov cl, 7
    cmp byte [cs:saved_mode], 0
    jne .shift
    mov cl, 5
.shift:
    shl ax, cl
    add ax, 0x1000
    mov bx, ax
    mov eax, [cs:saved_pos]
    movzx edx, bp
    add eax, edx
    mov cx, 1
    call read_lba
    jc cd_fail
    inc bp
    jmp .chunk

.loaded:
    mov ax, 0x1000
    mov ds, ax
    cmp byte [ds:0], 0xEB
    jne cd_fail
    mov ax, 0x1200
    mov ds, ax
    cmp byte [ds:0], 0xFC
    jne cd_fail
    xor ax, ax
    mov ds, ax
    mov si, msg_ok
    call bios_puts
    mov al, 'K'
    call early_putc
    jmp 0x1000:already_loaded

cd_fail:
    mov al, 'X'
    call early_putc
    mov bl, ah
    xor ax, ax
    mov ds, ax
    mov [0x04F2], bl
    mov si, msg_err
    call bios_puts
    mov al, bl
    call put_hex8
    cli
.hang:
    hlt
    jmp .hang

; EAX = LBA, CX = sector count, BX = destination segment, DL = drive.
; DS must be 0. Preserves EAX. Returns CF set on error, AH = BIOS status.
; EAX = LBA, CX = count, BX = destination segment. Returns CF set on error.
; Returns with DS = 0 and EAX preserved.
read_lba:
    push ds
    push si
    push eax
    mov si, cs
    mov ds, si
    pop eax
    mov word [dap_pkt], 0x0010
    mov [dap_pkt + 2], cx
    mov word [dap_pkt + 4], 0
    mov [dap_pkt + 6], bx
    mov [dap_pkt + 8], eax
    mov dword [dap_pkt + 12], 0
    push eax
    mov si, dap_pkt
    mov dl, [cs:saved_drive]
    mov ah, 0x42
    int 0x13
    mov [cs:read_status], ah
    mov byte [cs:read_err], 0
    jnc .read_ok
    mov byte [cs:read_err], 1
.read_ok:
    pop eax
    pushf
    xor si, si
    mov ds, si
    popf
    pop si
    pop ax
    ret

boot_prompt:
    mov ax, 0x0003
    int 0x10
    mov si, msg_boot_prompt
    call bios_puts
    xor ax, ax
    mov es, ax
    mov eax, [es:0x046C]
    mov [cs:boot_tick_start], eax
.wait:
    mov ah, 0x01
    int 0x16
    jnz .key
    xor ax, ax
    mov es, ax
    mov eax, [es:0x046C]
    sub eax, [cs:boot_tick_start]
    cmp eax, 91
    jb .wait
    stc
    ret
.key:
    xor ah, ah
    int 0x16
    clc
    ret

boot_hard_disk:
    mov bx, 0x55AA
    mov dl, 0x80
    mov ah, 0x41
    int 0x13
    jc .failed
    cmp bx, 0xAA55
    jne .failed
    test cx, 1
    jz .failed

    push ds
    mov ax, cs
    mov ds, ax
    mov word [dap_pkt], 0x0010
    mov word [dap_pkt + 2], 1
    mov word [dap_pkt + 4], 0x7C00
    mov word [dap_pkt + 6], 0
    mov dword [dap_pkt + 8], 0
    mov dword [dap_pkt + 12], 0
    mov si, dap_pkt
    mov dl, 0x80
    mov ah, 0x42
    int 0x13
    pop ds
    jc .failed

    xor ax, ax
    mov es, ax
    cmp word [es:0x7DFE], 0xAA55
    jne .failed
    mov dl, 0x80
    jmp 0x0000:0x7C00
.failed:
    stc
    ret

dap_pkt: times 16 db 0
saved_drive: db 0
saved_mode:  db 0
saved_mark:  db 0
read_status: db 0
read_err:    db 0
boot_tick_start: dd 0
saved_lba:   dd 0
saved_pos:   dd 0
saved_len:   dd 0
saved_count: dw 0

early_putc:
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

early_hex:
    push ax
    push cx
    mov ah, al
    mov cx, 2
.n:
    rol ah, 4
    mov al, ah
    and al, 0x0F
    cmp al, 10
    jb .d
    add al, 'A' - 10
    jmp .o
.d:
    add al, '0'
.o:
    call early_putc
    loop .n
    pop cx
    pop ax
    ret

bios_puts:
    push ax
    push bx
    push si
.char:
    mov al, [cs:si]
    inc si
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .char
.done:
    pop si
    pop bx
    pop ax
    ret

put_hex8:
    push ax
    push cx
    mov ah, al
    mov cx, 2
.nyb:
    rol ah, 4
    mov al, ah
    and al, 0x0F
    cmp al, 10
    jb .dig
    add al, 'A' - 10
    jmp .out
.dig:
    add al, '0'
.out:
    push ax
    push bx
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    pop bx
    pop ax
    loop .nyb
    pop cx
    pop ax
    ret

msg_cd:  db "GeometricalOS: installer disk detected.", 13, 10, 0
msg_boot_prompt:
    db "Press any key to boot from CD/DVD.", 13, 10
    db "Wait 5 seconds to boot from the hard drive.", 13, 10, 0
msg_no_hdd:
    db "No bootable hard drive found; starting the installer.", 13, 10, 0
msg_ok:  db "GeometricalOS: CD image OK", 13, 10, 0
msg_err: db "GeometricalOS: CD read failed, BIOS status ", 0

loader_end:
%if (loader_end - $$) > 2048
%error "CD loader must fit in the first 2048 bytes"
%endif

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
    mov si, msg_start
    call bios_puts

    ; Preserve the BIOS boot drive, then consume the HDD-boot signature.
    xor ax, ax
    mov ds, ax
    mov dl, [0x0510]
    mov ax, cs
    mov ds, ax
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
    mov si, msg_font
    call bios_puts
    call copy_font
    call sputc_C
    mov si, msg_video
    call bios_puts
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
    xor bx, bx
    mov es, bx
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
    test al, 0x80                ; linear framebuffer
    jz .skip
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
    cmp ax, 1280
    jne .c1024
    cmp dx, 720
    jne .c1024
    add bl, 60
    jmp .score
.c1024:
    cmp ax, 1024
    jne .c800
    cmp dx, 768
    jne .c800
    add bl, 55
    jmp .score
.c800:
    cmp ax, 800
    jne .c640
    cmp dx, 600
    jne .c640
    add bl, 50
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
    mov ax, 0x0003
    int 0x10
    mov ax, cs
    mov ds, ax
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

msg_start: db "GeometricalOS: starting kernel", 13, 10, 0
msg_font:  db "GeometricalOS: copying font", 13, 10, 0
msg_video: db "GeometricalOS: setting VESA mode", 13, 10, 0
failmsg:
    db "GeometricalOS needs a VESA linear framebuffer (640x480 or larger).", 13, 10
    db "Use a video adapter that supports this display mode.", 13, 10, 0

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
    push es
    push di
    mov ah, 0x88
    int 0x15
    jc .no_extended_memory
    movzx eax, ax
    jmp .store_extended_memory
.no_extended_memory:
    xor eax, eax
.store_extended_memory:
    pop di
    pop es
    mov [es:di + 52], eax         ; extended memory KB
    mov dword [es:di + 56], 0x8000
    mov ax, [font_height]
    movzx eax, ax
    mov [es:di + 60], eax
    movzx eax, byte [boot_drive]
    mov [es:di + 64], eax
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
