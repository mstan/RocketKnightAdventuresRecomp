"""Advance the supplied, already-corrupted stage-4 save 4 to its clear state.

The corrected recompiler prevents new boss type corruption, but a save made
after the bug has already lost the boss limbs and cannot finish its old state.
This one-off repair keeps the player's other saved state intact.
"""
import argparse
from pathlib import Path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('destination', type=Path)
args = parser.parse_args()
data = bytearray(args.source.read_bytes())
ram = 101
slot = ram + 0xCE00
if len(data) != 157106 or data[slot:slot+4] != bytes.fromhex('02d00004'):
    parser.error('expected the supplied stage-4 save 4 with poisoned CE00 boss state')
data[slot:slot+4] = bytes.fromhex('00d0000c')
args.destination.write_bytes(data)
print(args.destination)
