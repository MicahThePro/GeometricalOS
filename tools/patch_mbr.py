#!/usr/bin/env python3
"""Install the BIOS USB boot sector without touching the partition table."""
import sys

iso_path, mbr_path = sys.argv[1], sys.argv[2]
code = open(mbr_path, "rb").read()
if len(code) < 432:
    raise SystemExit("MBR code must be at least 432 bytes, got %d" % len(code))
with open(iso_path, "r+b") as f:
    sector = bytearray(f.read(512))
    if len(sector) < 512 or sector[510] != 0x55 or sector[511] != 0xAA:
        raise SystemExit("ISO is missing an MBR signature")
    sector[:432] = code[:432]
    f.seek(0)
    f.write(sector)
    f.seek(17 * 2048)
    bootrec = f.read(2048)
    if bootrec[1:6] != b"CD001" or b"EL TORITO SPECIFICATION" not in bootrec[:64]:
        raise SystemExit("ISO has no El Torito boot record")
    catalog = int.from_bytes(bootrec[0x47:0x4B], "little")
    f.seek(catalog * 2048 + 32)
    entry = bytearray(f.read(32))
    if entry[0] != 0x88:
        raise SystemExit("BIOS boot catalog entry is not bootable")
    entry[2] = 0xC0
    entry[3] = 0x07
    f.seek(catalog * 2048 + 32)
    f.write(entry)
    print("BIOS load segment 0x07C0, catalog LBA %d" % catalog)
print("patched BIOS USB boot sector")
