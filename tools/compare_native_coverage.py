#!/usr/bin/env python3
"""Fold gate: every instruction emitted natively before a regeneration must
still be emitted after it.

The recompiler ends a function at the next function start, so a new root (a
folded floor target, or a JSR target promoted by a larger runtime oracle) can
truncate an existing function and orphan its jump-table labels. Their code
then runs on the interpreter floor: guest state stays identical, but native
coverage silently regresses. Pass two generated directories (before, after).
"""
import argparse
import glob
import re
import sys
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('before', type=Path)
p.add_argument('after', type=Path)
a = p.parse_args()


def emitted(d):
    out = set()
    for f in glob.glob(str(d / '*_part*.c')):
        out.update(int(m, 16) for m in re.findall(r'/\* \$([0-9A-F]{6}) \*/', open(f).read()))
    return out


before, after = emitted(a.before), emitted(a.after)
lost = sorted(before - after)
print(f'before {len(before)} after {len(after)} gained {len(after - before)} lost {len(lost)}')
for pc in lost[:200]:
    print(f'lost ${pc:06X}')
sys.exit(1 if lost else 0)
