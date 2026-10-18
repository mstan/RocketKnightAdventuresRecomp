#!/usr/bin/env python3
"""Fold interpreter-floor evidence into a validated RKA discovery file.

Inputs are runtime evidence from tools/coverage_sweep.py (and owner-save
surveys): floor_coverage.toml files list call/jump targets the tier-3 floor
executed because no recompiled function existed; exec_pcs.txt files from
force-interp runs or native floor capsules list executed instruction starts.

A floor target is accepted only when all of these hold:
  * it is a ROM address not already a generated function entry;
  * the force-interp oracle saw it as an executed instruction start;
  * every instruction on its straight-line run up to the first control
    transfer decodes cleanly and was executed too (alignment proof).

Accepted targets become [functions] late_extra roots (non-recursive, so they
cannot reopen heuristic discovery). Targets inside an existing generated
function's address span are rejected: the floor records every jump in a
missed routine's subtree, including a parent's own jump-table labels, and the
recompiler ends a function at the next function start, so seeding one (even
in an undecoded gap) truncates the parent and orphans its in-function
dispatch (area 21's boss, $02A754).
--oracle-out writes the merged executed-PC oracle (game/rka_fold_oracle.txt). Keep it
out of runtime_exec_file: promoting newly executed JSR targets truncates
parents the same way (beads-3vb.19). Gate every regen with
tools/compare_native_coverage.py.
"""
import argparse
import glob
import re
from pathlib import Path

import capstone

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--rom', type=Path, required=True)
p.add_argument('--generated', type=Path, required=True)
p.add_argument('--floor', nargs='+', required=True, help='floor_coverage.toml files or globs')
p.add_argument('--oracle', nargs='+', required=True, help='exec_pcs.txt files or globs')
p.add_argument('--out', type=Path, required=True)
p.add_argument('--oracle-out', type=Path)
p.add_argument('--keep', type=Path, help='existing discovery file whose entries stay (fold iterations)')
a = p.parse_args()
rom = a.rom.read_bytes()
md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000)


def files(patterns):
    out = []
    for pat in patterns:
        out += sorted(glob.glob(pat)) or [pat]
    return out


gen, starts, spans = set(), set(), []
for f in a.generated.glob('*_part*.c'):
    text = f.read_text()
    gen.update(int(m, 16) for m in re.findall(r'/\* \$([0-9A-F]{6}) \*/', text))
    starts.update(int(m, 16) for m in re.findall(r'^void func_([0-9A-F]{6})\(void\)', text, re.M))
    # Address span of each function's decoded instructions. The recompiler
    # ends a function at the next function start, so a seed in a gap of a
    # parent's span (code it never decoded) still truncates that parent.
    for body in re.split(r'^void func_', text, flags=re.M)[1:]:
        pcs = [int(m, 16) for m in re.findall(r'/\* \$([0-9A-F]{6}) \*/', body)]
        if pcs:
            spans.append((int(body[:6], 16), max(pcs)))


if not gen or not starts or not spans:
    p.error('--generated must contain populated *_part*.c files; use the game output subdirectory')


def interior(addr):
    return addr in gen or any(lo < addr <= hi for lo, hi in spans)
oracle = set()
oracle_files = files(a.oracle)
for f in oracle_files:
    for line in open(f):
        line = line.strip()
        if line and not line.startswith('#'):
            oracle.add(int(line, 16))
floor = set()
floor_files = files(a.floor)
for f in floor_files:
    floor.update(int(m, 16) for m in re.findall(r'0x([0-9A-Fa-f]{6})', open(f).read()))

TRANSFER = ('rts', 'rte', 'rtr', 'jmp', 'jsr', 'bra', 'bsr', 'dbra', 'trap')


def is_transfer(mnemonic):
    m = mnemonic.split('.')[0]
    if m in ('sub', 'subq', 'subi', 'suba', 'subx', 'swap', 'bset', 'bclr', 'bchg', 'btst'):
        return False
    return m in TRANSFER or m.startswith(('b', 'db', 's'))


def validate(entry):
    """Straight-line run to the first transfer: every instruction decodes and
    all but the transfer executed. The floor capsule peeks (does not execute)
    its own depth-0 RTS, so the oracle never records that one."""
    pc = entry
    for _ in range(64):
        ins = next(md.disasm(rom[pc:pc + 10], pc, 1), None)
        if ins is None or ins.mnemonic.startswith('.'):
            return f'${pc:06X} does not decode'
        if is_transfer(ins.mnemonic):
            return None if pc != entry or ins.mnemonic.startswith(('rts', 'rtr')) or pc in oracle                 else f'${pc:06X} not executed'
        if pc not in oracle:
            return f'${pc:06X} not executed'
        pc += ins.size
    return None


accepted, rejected = [], []
kept = set()
if a.keep and a.keep.exists():
    kept = {int(m, 16) for m in re.findall(r'0x([0-9A-Fa-f]{6})', a.keep.read_text())}
    accepted += [(x, 'kept') for x in sorted(kept)]
for addr in sorted(floor - kept):
    if addr >= len(rom) or addr in starts:
        continue
    why = 'inside an existing function (interior label, never a seed)' if interior(addr) else validate(addr)
    (rejected if why else accepted).append((addr, why or 'new'))

accepted.sort()
lines = [
    '# Validated tier-3 floor targets (tools/fold_floor_coverage.py).',
    f'# Evidence: {len(floor_files)} floor_coverage.toml and {len(oracle_files)} executed-PC oracle inputs',
    '# Executed-PC evidence from coverage sweeps and owner-save floor capsules.',
    '# Each entry was executed by the floor as a call/jump target and its straight-line',
    '# run decodes cleanly and appears in the executed-PC oracle.',
    f'# accepted={len(accepted)} rejected={len(rejected)}',
    '',
    '[functions]',
    'late_extra = [',
]
for addr, kind in accepted:
    lines.append(f'  0x{addr:06X},')
lines.append(']')
a.out.write_text('\n'.join(lines) + '\n', newline='\n')
for addr, why in rejected:
    print(f'rejected ${addr:06X}: {why}')
print(f'accepted {len(accepted)}, rejected {len(rejected)} '
      f'({sum("interior" in w for _, w in rejected)} interior) -> {a.out}')
if a.oracle_out:
    a.oracle_out.write_text('# executed ROM instruction-start PCs: attract, all-area sweep, and owner-save floor capsules\n' +
                            ''.join(f'{x:06X}\n' for x in sorted(oracle) if x < len(rom)), newline='\n')
    print(f'oracle {len(oracle)} PCs -> {a.oracle_out}')
