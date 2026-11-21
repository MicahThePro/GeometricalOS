#!/usr/bin/env python3
"""HP firmware rejects an EFI image that is marked as a DLL."""
import struct
import sys

path = sys.argv[1]
with open(path, "r+b") as f:
    data = bytearray(f.read(512))
    if data[:2] != b"MZ":
        raise SystemExit("not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("missing PE signature")
    chars = struct.unpack_from("<H", data, pe + 22)[0]
    chars = (chars & ~0x2000) | 0x0002
    struct.pack_into("<H", data, pe + 22, chars)
    f.seek(0)
    f.write(data)
print("cleared EFI DLL flag, characteristics %#x" % chars)
