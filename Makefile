# GeometricalOS — freestanding 32-bit hobby OS.
# Needs: nasm, gcc -m32 (gcc-multilib), xorriso, ld, python3,
# mingw-w64 x86_64 gcc, mkfs.fat, and mtools.

CC      := gcc
NASM    := nasm
OBJCOPY := objcopy
PYTHON  := python3
MINGW   := x86_64-w64-mingw32-gcc
MINGWOBJ := x86_64-w64-mingw32-objcopy

BUILD   := build
CFLAGS  := -std=gnu11 -m32 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables \
           -mno-sse -mno-mmx -msoft-float -Os -Wall -Wextra \
           -Wno-unused-parameter -Wno-unused-function -Ikernel
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib --build-id=none
LIBGCC  := $(shell $(CC) -m32 -print-libgcc-file-name)

C_SRCS := \
	kernel/string.c kernel/serial.c kernel/gfx.c kernel/input.c kernel/rtc.c \
	kernel/ata.c kernel/fs.c kernel/gui.c kernel/icons.c kernel/power.c kernel/pcnet.c kernel/net.c \
	kernel/image_memory.c kernel/stb_image.c kernel/app_image.c \
	kernel/app_notepad.c kernel/app_calc.c kernel/app_clock.c kernel/app_paint.c \
	kernel/app_mines.c kernel/app_files.c kernel/app_term.c kernel/app_sol.c kernel/app_snake.c \
	kernel/app_recycle.c kernel/app_write.c kernel/app_charmap.c kernel/app_cardfile.c kernel/app_recovery.c \
	kernel/app_tools.c kernel/app_ftp.c \
	kernel/sound.c kernel/wallpaper.c kernel/login.c kernel/font8x16_fallback.c \
	kernel/desktop.c kernel/setup.c kernel/pci.c kernel/xhci.c kernel/vmm.c \
	kernel/settings.c kernel/app_control.c kernel/app_player.c \
	kernel/main.c

WEBP_C_SRCS := \
	kernel/third_party/libwebp/src/dec/alpha_dec.c \
	kernel/third_party/libwebp/src/dec/buffer_dec.c \
	kernel/third_party/libwebp/src/dec/frame_dec.c \
	kernel/third_party/libwebp/src/dec/idec_dec.c \
	kernel/third_party/libwebp/src/dec/io_dec.c \
	kernel/third_party/libwebp/src/dec/quant_dec.c \
	kernel/third_party/libwebp/src/dec/tree_dec.c \
	kernel/third_party/libwebp/src/dec/vp8_dec.c \
	kernel/third_party/libwebp/src/dec/vp8l_dec.c \
	kernel/third_party/libwebp/src/dec/webp_dec.c \
	kernel/third_party/libwebp/src/dsp/alpha_processing.c \
	kernel/third_party/libwebp/src/dsp/cpu.c \
	kernel/third_party/libwebp/src/dsp/dec.c \
	kernel/third_party/libwebp/src/dsp/dec_clip_tables.c \
	kernel/third_party/libwebp/src/dsp/filters.c \
	kernel/third_party/libwebp/src/dsp/lossless.c \
	kernel/third_party/libwebp/src/dsp/rescaler.c \
	kernel/third_party/libwebp/src/dsp/upsampling.c \
	kernel/third_party/libwebp/src/dsp/yuv.c \
	kernel/third_party/libwebp/src/utils/bit_reader_utils.c \
	kernel/third_party/libwebp/src/utils/color_cache_utils.c \
	kernel/third_party/libwebp/src/utils/filters_utils.c \
	kernel/third_party/libwebp/src/utils/huffman_utils.c \
	kernel/third_party/libwebp/src/utils/palette.c \
	kernel/third_party/libwebp/src/utils/quant_levels_dec_utils.c \
	kernel/third_party/libwebp/src/utils/rescaler_utils.c \
	kernel/third_party/libwebp/src/utils/thread_utils.c \
	kernel/third_party/libwebp/src/utils/utils.c

ASM_OBJS := $(BUILD)/entry.o
C_OBJS   := $(patsubst kernel/%.c,$(BUILD)/%.o,$(C_SRCS)) $(BUILD)/mbr_embed.o
WEBP_OBJS := $(patsubst kernel/third_party/libwebp/src/%.c,$(BUILD)/webp/%.o,$(WEBP_C_SRCS))

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

$(BUILD)/stb_image.o: kernel/stb_image.c | $(BUILD)
	$(CC) $(CFLAGS) -DNDEBUG -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -c $< -o $@

$(BUILD)/image_memory.o: kernel/image_memory.c | $(BUILD)
	$(CC) $(CFLAGS) -DNDEBUG -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
	  -Ikernel/third_party/libwebp -c $< -o $@

$(BUILD)/app_image.o: kernel/app_image.c | $(BUILD)
	$(CC) $(CFLAGS) -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -c $< -o $@

$(BUILD)/webp/%.o: kernel/third_party/libwebp/src/%.c | $(BUILD)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -DNDEBUG -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
	  -Ikernel/third_party/libwebp -Ikernel/third_party/libwebp/src \
	  -include kernel/image_memory.h \
	  -Dmalloc=gos_image_malloc -Dcalloc=gos_image_calloc \
	  -Drealloc=gos_image_realloc -Dfree=gos_image_free -c $< -o $@

$(BUILD)/mbr_embed.o: $(BUILD)/mbr_embed.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(ASM_OBJS) $(C_OBJS) $(WEBP_OBJS) linker.ld
	ld -m elf_i386 -T linker.ld -nostdlib --build-id=none --hash-style=sysv \
	  -o $@ $(ASM_OBJS) $(C_OBJS) $(WEBP_OBJS) $(LIBGCC)

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/bootimg.bin: $(BUILD)/stub.bin $(BUILD)/kernel.bin
	cat $(BUILD)/stub.bin $(BUILD)/kernel.bin > $@
	$(PYTHON) tools/patch_image.py $@
	@sz=$$(stat -c%s $@); \
	  echo "boot image $$sz bytes"; \
	  test $$sz -lt 393216

$(BUILD)/switch64.bin: boot/switch64.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

$(BUILD)/enter32.bin: boot/enter32.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

$(BUILD)/usb.bin: boot/usb.asm | $(BUILD)
	$(NASM) -f bin $< -o $@
	@test $$(stat -c%s $@) -eq 440

$(BUILD)/efi_jump.o: boot/efi_jump.asm | $(BUILD)
	$(NASM) -f win64 $< -o $@

$(BUILD)/bootimg.o: $(BUILD)/bootimg.bin
	cd $(BUILD) && $(MINGWOBJ) -I binary -O pe-x86-64 -B i386:x86-64 bootimg.bin bootimg.o

$(BUILD)/switch64.o: $(BUILD)/switch64.bin
	cd $(BUILD) && $(MINGWOBJ) -I binary -O pe-x86-64 -B i386:x86-64 switch64.bin switch64.o

$(BUILD)/enter32.o: $(BUILD)/enter32.bin
	cd $(BUILD) && $(MINGWOBJ) -I binary -O pe-x86-64 -B i386:x86-64 enter32.bin enter32.o

$(BUILD)/efi.o: boot/efi.c | $(BUILD)
	$(MINGW) -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone \
	  -mno-stack-arg-probe -mgeneral-regs-only -O2 -Wall -Wextra \
	  -Wno-unused-parameter -c boot/efi.c -o $@

$(BUILD)/bootx64.efi: $(BUILD)/efi.o $(BUILD)/efi_jump.o $(BUILD)/bootimg.o $(BUILD)/switch64.o $(BUILD)/enter32.o tools/patch_efi.py
	$(MINGW) -nostdlib -shared -Wl,-e,efi_main -Wl,--subsystem,10 \
	  -Wl,--image-base,0x4000000 -o $@ $(BUILD)/efi.o $(BUILD)/efi_jump.o \
	  $(BUILD)/bootimg.o $(BUILD)/switch64.o $(BUILD)/enter32.o
	$(PYTHON) tools/patch_efi.py $@

$(BUILD)/efiboot.img: $(BUILD)/bootx64.efi | $(BUILD)
	rm -f $@
	dd if=/dev/zero of=$@ bs=1M count=2 status=none
	mkfs.fat -F 12 $@ >/dev/null
	mmd -i $@ ::/EFI ::/EFI/BOOT
	mcopy -i $@ $(BUILD)/bootx64.efi ::/EFI/BOOT/BOOTX64.EFI

dist/GeometricalOS.iso: $(BUILD)/bootimg.bin $(BUILD)/bootx64.efi $(BUILD)/efiboot.img $(BUILD)/usb.bin | $(BUILD)
	rm -rf iso_root
	mkdir -p iso_root/EFI/BOOT dist
	cp $(BUILD)/bootimg.bin iso_root/bootimg.bin
	cp $(BUILD)/bootx64.efi iso_root/EFI/BOOT/BOOTX64.EFI
	cp $(BUILD)/efiboot.img iso_root/EFI/BOOT/efiboot.img
	bytes=$$(stat -c%s iso_root/bootimg.bin); \
	sectors=$$(( (bytes + 2047) / 2048 * 4 )); \
	echo "El Torito load sectors (512-byte, multiple of 4): $$sectors"; \
	dd if=$(BUILD)/usb.bin of=$(BUILD)/isoh.bin bs=432 count=1 status=none; \
	xorriso -as mkisofs -quiet -V GEOMETRICALOS -R -J \
	  -b bootimg.bin -c boot.cat \
	  -no-emul-boot -boot-load-size $$sectors \
	  -boot-info-table \
	  -eltorito-alt-boot -e EFI/BOOT/efiboot.img -no-emul-boot \
	  -isohybrid-mbr $(BUILD)/isoh.bin \
	  -isohybrid-gpt-basdat \
	  -o dist/GeometricalOS.iso iso_root
	$(PYTHON) tools/patch_mbr.py dist/GeometricalOS.iso $(BUILD)/usb.bin
	@echo "ISO dist/GeometricalOS.iso"

iso: dist/GeometricalOS.iso

run: dist/GeometricalOS.iso
	qemu-system-i386 -m 64 -vga std -cdrom dist/GeometricalOS.iso -boot d -serial stdio

clean:
	rm -rf $(BUILD) iso_root dist/GeometricalOS.iso
