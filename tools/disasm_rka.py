#!/usr/bin/env python3
"""Disassemble an explicit range of the owner's RKA USA ROM (hex offsets)."""
import argparse
from pathlib import Path
import capstone

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("rom", type=Path)
p.add_argument("start", type=lambda s: int(s, 16))
p.add_argument("end", type=lambda s: int(s, 16))
a = p.parse_args()
rom = a.rom.read_bytes()
md = capstone.Cs(capstone.CS_ARCH_M68K,
                 capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000)
md.skipdata = True
for insn in md.disasm(rom[a.start:a.end], a.start):
    print(f"{insn.address:06X}: {insn.bytes.hex():<20} {insn.mnemonic:<9} {insn.op_str}")
