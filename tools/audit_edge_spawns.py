#!/usr/bin/env python3
"""Find RKA object creations placed at a fixed native screen edge.

Scans every instruction the recompiler emitted (the `/* $XXXXXX */` markers in
the generated C) for X stores into a freshly allocated slot, `$6(a2)`, whose
value is a constant (map-1 screen space) or the camera plus a constant (tiled
world space) outside the native 0..319 view. Constants reach the store
directly, through a data register loaded earlier in the same straight-line
run, or through a following ADDI/SUBI on the stored word.

A creator helper that stores an input register is followed to its callers,
where the same constant/camera classification applies (hooked at the call's
return address).

Prints one row per site and, with --toml, the matching `[[widescreen_site]]`
blocks (hooked at the instruction after the final X write) plus a C table.
"""
import argparse
import re
from pathlib import Path

import capstone

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('rom', type=Path)
p.add_argument('generated', type=Path, help='directory holding rka_part*.c')
p.add_argument('--toml', action='store_true')
a = p.parse_args()
rom = a.rom.read_bytes()
md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000)
addrs = set()
for f in a.generated.glob('rka_part*.c'):
    addrs.update(int(m, 16) for m in re.findall(r'/\* \$([0-9A-F]{6}) \*/', f.read_text()))
# Unreached JMP-table states (move.w tbl(pc,d0.w),d0 ; jmp tbl(pc,d0.w)) are
# decoded too: the tier-3 floor runs game hooks, so their sites still count.
for m in re.finditer(re.escape(bytes.fromhex('303b00064efb0002')), rom):
    base = m.start() + 8
    for n in range(32):
        off = int.from_bytes(rom[base + 2 * n:base + 2 * n + 2], 'big', signed=True)
        pc = base + off
        if off <= 0 or pc >= len(rom) or pc & 1:
            break
        for _ in range(256):
            i = next(md.disasm(rom[pc:pc + 10], pc, 1), None)
            if i is None or i.mnemonic.startswith('.'):
                break
            if pc in addrs:
                break
            addrs.add(pc)
            if i.mnemonic.split('.')[0] == 'bra' and i.op_str.startswith('$'):
                pc = int(i.op_str[1:], 16)   # a state's tail branch into shared code
                continue
            if i.mnemonic.split('.')[0] in ('rts', 'rte', 'jmp', 'bra'):
                break
            pc += i.size
addrs = sorted(addrs)
insn = {}
for x in addrs:
    for i in md.disasm(rom[x:x + 10], x, 1):
        insn[x] = i
order = {x: k for k, x in enumerate(addrs)}

def imm(text):
    text = text.strip()
    if not text.startswith('#'):
        return None
    v = int(text[2:], 16) if text.startswith('#$') else int(text[1:], 0)
    return v

def signed16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v

# Backtracking may cross conditional branches and loop heads (the runtime
# hook still decides from the stored value), but not calls or exits.
FLOW = ('rts', 'rte', 'jmp', 'bra', 'jsr', 'bsr')

def is_flow(i):
    return i.mnemonic.split('.')[0] in FLOW

CAMERA = ('$b1d2.w', '$ffb1d2.l')

def register_value(k, reg, size):
    """(value, space) of data register `reg` before addrs[k]: an immediate or
    camera load plus any ADDI/SUBI immediates applied to it since. None when
    any other instruction writes the register first."""
    delta = 0
    for j in range(k - 1, max(-1, k - 24), -1):
        i = insn.get(addrs[j])
        if not i or is_flow(i):
            return None
        ops = [o.strip() for o in i.op_str.split(',')]
        if not ops or ops[-1] != reg:
            if reg in ops[-1:]:
                return None
            continue
        if i.mnemonic in ('addi.w', 'addi.l', 'subi.w', 'subi.l', 'addq.w', 'addq.l',
                          'subq.w', 'subq.l') and imm(ops[0]) is not None:
            d = imm(ops[0])
            delta += -d if i.mnemonic.startswith('sub') else d
            continue
        if i.mnemonic == 'moveq':
            v = imm(ops[0])
            v = v - 0x100 if v & 0x80 else v
            return ((v << 16) if size == 'l' else v) + delta, 'screen'
        if i.mnemonic.startswith('move') and len(ops) == 2:
            if imm(ops[0]) is not None:
                return imm(ops[0]) + delta, 'screen'
            if ops[0] in CAMERA:
                return delta, 'camera'
        return None
    return None

# $16E34: rail-ride type $9A starts at X=0 with +1px/frame (enters from the
# left). Other X=0/camera+0 stores are anchors or placeholders positioned later.
ZERO_EDGE = {0x016E34}
rows = []
for k, x in enumerate(addrs):
    i = insn.get(x)
    if not i:
        continue
    ops = [o.strip() for o in i.op_str.split(',')]
    dest = ops[-1]
    if dest != '$6(a2)':
        continue
    m = i.mnemonic
    size = m.split('.')[-1] if '.' in m else 'w'
    value, space = None, 'screen'
    if m.startswith('clr'):
        value = 0
    elif m.startswith('move') and len(ops) >= 2 and '(pc' in i.op_str:
        # move.w table(pc,dN.w),$6(a2): position chosen from a constant
        # table; classify from the table's distinct leading entries.
        base = int(re.match(r'\$([0-9a-f]+)\(pc', i.op_str).group(1), 16)
        vals = {signed16(int.from_bytes(rom[base + 2 * n:base + 2 * n + 2], 'big')) for n in range(16)}
        if all(v <= 0 or v >= 320 for v in vals):
            rows.append((x, x + i.size, 'screen', sorted(vals), 'table', f'{i.mnemonic} {i.op_str}'))
        continue
    elif m.startswith('move') and len(ops) == 2:
        v = imm(ops[0])
        if v is None and re.fullmatch(r'd[0-7]', ops[0]):
            got = register_value(k, ops[0], size)
            if got:
                v, space = got
        if v is None and ops[0] in CAMERA:
            v, space = 0, 'camera'
        value = v
    if value is None:
        continue
    whole = signed16(value >> 16) if size == 'l' else signed16(value)
    # Fold following ADDI/SUBI to the stored word (camera + offset idiom).
    last = i
    for j in range(k + 1, min(len(addrs), k + 4)):
        n = insn.get(addrs[j])
        if not n:
            break
        nops = [o.strip() for o in n.op_str.split(',')]
        if nops[-1] != '$6(a2)':
            break
        d = imm(nops[0])
        if d is None or n.mnemonic not in ('addi.w', 'subi.w'):
            break
        whole += signed16(d) if n.mnemonic == 'addi.w' else -signed16(d)
        last = n
    # Native culling keeps objects only at screen X -96..415, so a fixed
    # spawn outside that band is a world coordinate, not a screen edge. X=0
    # is an edge entry only at audited sites (see ZERO_EDGE).
    side = None
    if -96 < whole < 0 or (whole == 0 and x in ZERO_EDGE):
        side = 'left'
    elif 320 <= whole < 416:
        side = 'right'
    if not side:
        continue
    hook = last.address + last.size
    if hook not in order:
        continue
    rows.append((x, hook, space, whole, side, f'{i.mnemonic} {i.op_str}'))

# Register-passed X: a shared creator stores an input data register into
# $6(a2) (the debris/particle builder $368B6 behind $3689E). The constant
# lives at each call site, so trace the register back to the routine entry,
# then to every caller (through pass-through wrappers) and classify the value
# there. The hook is the call's return address: A2 still holds the new slot.
callers = {}
for x in addrs:
    i = insn.get(x)
    if i and i.mnemonic.split('.')[0] in ('jsr', 'bsr') and re.fullmatch(r'\$[0-9a-f]+(\.l)?', i.op_str):
        callers.setdefault(int(i.op_str[1:].split('.')[0], 16), []).append(x)

def writes(i, reg):
    ops = [o.strip() for o in i.op_str.split(',')]
    if not ops:
        return False
    dest = ops[-1]
    if dest == reg:
        return True
    if i.mnemonic.startswith('movem') and not dest.startswith(('-(', '(', '$')):
        for part in dest.split('/'):
            lo, _, hi = part.partition('-')
            if lo[0] == reg[0] and int(lo[1]) <= int(reg[1]) <= int((hi or lo)[1]):
                return True
    return False

def input_entries(k, reg):
    """Entries of the straight-line code ending before addrs[k] that receive
    `reg` unmodified from their caller, or None when it is written first."""
    found = []
    for j in range(k - 1, -1, -1):
        cur, nxt = addrs[j], addrs[j + 1]
        if nxt in callers:
            found.append(nxt)
        i = insn.get(cur)
        if not i or cur + i.size != nxt:
            return found or None
        if i.mnemonic.split('.')[0] in ('rts', 'rte', 'jmp', 'bra'):
            return found or None
        if i.mnemonic.split('.')[0] in ('jsr', 'bsr') or writes(i, reg):
            return None
    return found or None

def classify(whole):
    if -96 < whole < 0:
        return 'left'
    if 320 <= whole < 416:
        return 'right'
    return None

for k, x in enumerate(addrs):
    i = insn.get(x)
    if not i or not i.mnemonic.startswith('move'):
        continue
    ops = [o.strip() for o in i.op_str.split(',')]
    if len(ops) != 2 or ops[1] != '$6(a2)' or not re.fullmatch(r'd[0-7]', ops[0]):
        continue
    size = i.mnemonic.split('.')[-1]
    if register_value(k, ops[0], size):
        continue   # resolved locally above
    todo = [(e, ops[0]) for e in (input_entries(k, ops[0]) or [])]
    seen = set()
    while todo:
        entry, reg = todo.pop()
        if (entry, reg) in seen:
            continue
        seen.add((entry, reg))
        for site in callers.get(entry, []):
            ks = order[site]
            got = register_value(ks, reg, size)
            if got is None:
                todo += [(e, reg) for e in (input_entries(ks, reg) or [])]
                continue
            v, space = got
            whole = signed16(v >> 16) if size == 'l' else signed16(v)
            side = classify(whole)
            hook = site + insn[site].size
            if side and hook in order:
                rows.append((site, hook, space, whole, side,
                             f'{insn[site].mnemonic} {insn[site].op_str} -> {i.mnemonic} {i.op_str} @${x:06X}'))

rows.sort()
for x, hook, space, whole, side, text in rows:
    print(f'{x:06X} hook {hook:06X} {space:6} x={str(whole):>5} {side:5} | {text}')
if a.toml:
    print()
    for x, hook, space, whole, side, text in rows:
        print(f'[[widescreen_site]]\naddr = 0x{hook:06X}  # {text} ({space} {whole})\nkind = "game_hook"')
    print()
    for x, hook, space, whole, side, text in rows:
        print(f'    {{ 0x{hook:06X}u, {1 if space == "camera" else 0} }}, /* ${x:06X} {text} */')
