# GeometricalOS — freestanding 32-bit hobby OS.
# Needs: nasm, gcc -m32 (gcc-multilib), xorriso, ld, python3.

CC      := gcc
NASM    := nasm
OBJCOPY := objcopy
PYTHON  := python3

BUILD   := build
CFLAGS  := -std=gnu11 -m32 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables \
           -mno-sse -mno-mmx -msoft-float -Os -Wall -Wextra \
           -Wno-unused-parameter -Wno-unused-function -Ikernel
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib --build-id=none
LIBGCC  := $(shell $(CC) -m32 -print-libgcc-file-name)

C_SRCS := \
	kernel/string.c kernel/serial.c kernel/gfx.c kernel/input.c kernel/rtc.c \
	kernel/ata.c kernel/fs.c kernel/gui.c kernel/icons.c kernel/power.c \
	kernel/app_notepad.c kernel/app_calc.c kernel/app_clock.c kernel/app_paint.c \
	kernel/app_mines.c kernel/app_files.c kernel/app_term.c kernel/app_sol.c kernel/app_snake.c \
	kernel/app_recycle.c kernel/app_write.c kernel/app_charmap.c kernel/app_cardfile.c kernel/app_recovery.c \
	kernel/sound.c kernel/wallpaper.c kernel/login.c kernel/font8x16_fallback.c \
	kernel/desktop.c kernel/setup.c kernel/pci.c kernel/vmm.c \
	kernel/settings.c kernel/app_control.c kernel/app_player.c \
	kernel/main.c

ASM_OBJS := $(BUILD)/entry.o
C_OBJS   := $(patsubst kernel/%.c,$(BUILD)/%.o,$(C_SRCS)) $(BUILD)/mbr_embed.o

.PHONY: all iso run clean

all: dist/GeometricalOS.iso

$(BUILD):
	mkdir -p $(BUILD) dist iso_root

$(BUILD)/entry.o: kernel/entry.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/stub.bin: boot/stub.asm | $(BUILD)
	$(NASM) -f bin $< -o $@
	@test $$(stat -c%s $@) -eq 8192

$(BUILD)/mbr.bin: boot/mbr.asm | $(BUILD)
	$(NASM) -f bin $< -o $@
	@test $$(stat -c%s $@) -eq 512

$(BUILD)/mbr_embed.c: $(BUILD)/mbr.bin tools/bin2c.py
	$(PYTHON) tools/bin2c.py $(BUILD)/mbr.bin $@ mbr_template

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/mbr_embed.o: $(BUILD)/mbr_embed.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(ASM_OBJS) $(C_OBJS) linker.ld
	ld -m elf_i386 -T linker.ld -nostdlib --build-id=none --hash-style=sysv \
	  -o $@ $(ASM_OBJS) $(C_OBJS) $(LIBGCC)

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/bootimg.bin: $(BUILD)/stub.bin $(BUILD)/kernel.bin
	cat $(BUILD)/stub.bin $(BUILD)/kernel.bin > $@
	$(PYTHON) tools/patch_image.py $@
	@sz=$$(stat -c%s $@); \
	  echo "boot image $$sz bytes"; \
	  test $$sz -lt 393216

dist/GeometricalOS.iso: $(BUILD)/bootimg.bin | $(BUILD)
	rm -rf iso_root
	mkdir -p iso_root dist
	cp $(BUILD)/bootimg.bin iso_root/bootimg.bin
	bytes=$$(stat -c%s iso_root/bootimg.bin); \
	sectors=$$(( (bytes + 2047) / 2048 * 4 )); \
	echo "El Torito load sectors (512-byte, multiple of 4): $$sectors"; \
	genisoimage -quiet -V GEOMETRICALOS \
	  -b bootimg.bin -c boot.cat \
	  -no-emul-boot -boot-load-size $$sectors -boot-load-seg 0x07C0 \
	  -boot-info-table \
	  -o dist/GeometricalOS.iso iso_root
	@echo "ISO dist/GeometricalOS.iso"

iso: dist/GeometricalOS.iso

run: dist/GeometricalOS.iso
	qemu-system-i386 -m 64 -vga std -cdrom dist/GeometricalOS.iso -boot d -serial stdio

clean:
	rm -rf $(BUILD) iso_root dist/GeometricalOS.iso
