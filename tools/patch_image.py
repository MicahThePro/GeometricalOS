#!/usr/bin/env python3
"""Store the boot-image size at offset 24 so the real-mode stub can relocate."""
import os
import struct
import sys

path = sys.argv[1]
size = os.path.getsize(path)
rounded = (size + 511) & ~511
with open(path, "r+b") as f:
    f.seek(24)
    current = f.read(4)
    f.seek(24)
    f.write(struct.pack("<I", rounded))
print("patched image_bytes=%d (file %d)" % (rounded, size))
