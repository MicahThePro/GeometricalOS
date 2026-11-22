OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
    /* The 8 KiB real-mode stub is prepended at physical 0x10000. */
    . = 0x12000;

    .hash     : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .dynsym   : { *(.dynsym) }
    .dynstr   : { *(.dynstr) }

    .text : {
        *(.text.entry)
        *(.text*)
    }

    .rodata : ALIGN(4) {
        *(.rodata*)
    }

    .data : ALIGN(4) {
        *(.data)
    }

    kernel_file_end = .;

    .bss : ALIGN(4) {
        bss_start = .;
        *(.bss)
        *(COMMON)
        bss_end = .;
    }

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment)
        *(.note*)
        *(.got*)
        *(.gnu*)
        *(.note.GNU-stack)
    }

    /* Sound Blaster playback uses 0x78000. Stay below it. */
    ASSERT(bss_end < 0x78000, "Kernel image overlaps the stack")
}
