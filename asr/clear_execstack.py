#!/usr/bin/env python3
"""Clears the executable-stack (PF_X) flag on an ELF64 file's PT_GNU_STACK
program header segment, in place. Stands in for the `execstack -c` tool
(dropped from Debian trixie's repos) -- ctranslate2's prebuilt .so ships
without the correct PT_GNU_STACK marking, and this environment's kernel
refuses to map a shared object that demands an executable stack
("cannot enable executable stack as shared object requires: Invalid argument").
"""
import struct
import sys

PT_GNU_STACK = 0x6474E551
PF_X = 0x1


def clear_execstack(path: str) -> None:
    with open(path, "r+b") as f:
        data = bytearray(f.read())
        if data[:4] != b"\x7fELF":
            print(f"{path}: not an ELF file, skipping")
            return
        if data[4] != 2:
            print(f"{path}: not ELF64, skipping")
            return

        e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
        e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
        e_phnum = struct.unpack_from("<H", data, 0x38)[0]

        changed = False
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            p_type = struct.unpack_from("<I", data, off)[0]
            if p_type == PT_GNU_STACK:
                p_flags = struct.unpack_from("<I", data, off + 4)[0]
                struct.pack_into("<I", data, off + 4, p_flags & ~PF_X)
                changed = True

        if changed:
            f.seek(0)
            f.write(data)
            print(f"{path}: cleared PF_X on PT_GNU_STACK")
        else:
            print(f"{path}: no PT_GNU_STACK segment found")


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        clear_execstack(arg)
