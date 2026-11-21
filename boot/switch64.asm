; Runs at physical 0x7100 after UEFI exits. RAX holds the new CR3.
; Firmware identity-maps this page, and the kernel page tables do too.
bits 64
org 0x7100

mov cr3, rax
lgdt [0x7C00]
push 8
push 0x7000
retfq
