#!/usr/bin/env python3
"""Boot every RKA area directly and record runtime coverage evidence.

With --save-zip, each owner save in the ZIP(s) is a run instead: the save is
loaded after reset and the same input pattern drives it from there, so a
tester's floor-coverage targets get native executed-PC evidence.

Each run starts a new game, writes the area index to $FFFB12 during the story
screens (the "stage modifier" RAM code), keeps the player alive with the
invincibility/health/lives RAM codes ($FFC068=$30, $FFC040=$3F, $FFFB0C=9)
and drives a rotating input pattern. It collects the interpreter-floor and
dispatch-miss evidence files, the always-on spawn ring (rka_spawns) and,
optionally, periodic screenshots for a renderer gallery.
"""
import argparse
import concurrent.futures as cf
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time
import zipfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--areas', default='0-63', help='e.g. 0-63 or 25,34,41')
p.add_argument('--frames', type=int, default=3600)
p.add_argument('--mode', default='off', help='video mode, e.g. off or 16:9')
p.add_argument('--spawn-range', choices=('expanded', 'original'), default='expanded')
p.add_argument('--shots-every', type=int, default=0)
p.add_argument('--jobs', type=int, default=8)
p.add_argument('--force-interp', action='store_true',
               help='interpret the whole program and dump executed PCs (runtime oracle)')
p.add_argument('--save-zip', type=Path, action='append', default=[],
               help='run every native_save_N.bin in this ZIP instead of booting areas (repeatable)')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
areas = []
for part in a.areas.split(','):
    lo, _, hi = part.partition('-')
    areas += range(int(lo), int(hi or lo) + 1)
saves = {}
for i, z in enumerate(a.save_zip):
    with zipfile.ZipFile(z) as zf:
        for name in zf.namelist():
            if name.endswith('.bin'):
                saves[f'save{i:02d}-{Path(name).stem}'] = zf.read(name)

# Movement, B jumps, A/C attacks and rocket charge, climbing and idling.
PATTERN = [('RIGHT',), ('RIGHT', 'C'), ('RIGHT', 'B'), ('RIGHT', 'A'), ('LEFT',), ('UP', 'B'),
           ('DOWN',), (), ('RIGHT', 'C', 'B'), ('LEFT', 'C'), ('UP',), ('RIGHT',)]


def script_for(area, run):
    if isinstance(area, str):
        # Let reset reach RKA's idle loop before replacing guest state. Owner
        # saves carry a 101-byte header; B014 bit 7 is the game's pause flag.
        save = saves[area]
        s = f'WAIT 600\nLOAD_STATE {(run / "save.bin").as_posix()}\nWAIT 3\n'
        if len(save) == 157106 and save[101 + 0xB014] & 128:
            s += 'PRESS START 2\nWAIT 4\n'
    else:
        s = ('WAIT_RAM16 FFB002 0001\nWAIT_RAM16 FFB004 0001\nPRESS START 2\nWAIT 4\n'
             'WAIT_RAM16 FFB004 0004\nPRESS START 2\nWAIT 4\n'
             f'WRITE_RAM16 FFFB12 {area:04X}\nWAIT_RAM16 FFB002 0004\nWRITE_RAM16 FFFB12 {area:04X}\n')
    t = 0
    while t < a.frames:
        buttons = PATTERN[(t // 60) % len(PATTERN)]
        s += 'WRITE_RAM16 FFC068 0030\nWRITE_RAM16 FFC040 003F\nWRITE_RAM16 FFFB0C 0009\n'
        s += ''.join(f'PRESS {b} 30\n' for b in buttons)
        s += 'WAIT 30\n'
        t += 30
        if a.shots_every and t % a.shots_every == 0:
            s += f'SCREENSHOT {(run / f"shot-{t:05d}.png").as_posix()}\n'
    # A fixed-frame RAM image lets two builds be compared for identical play.
    s += f'DUMP_RAM {(run / "final.ram").as_posix()}\n'
    s += f'SCREENSHOT {(run / "done.png").as_posix()}\nWAIT 60000\n'
    return s


def run_area(area):
    run = out / (area if isinstance(area, str) else f'area-{area:02d}')
    if run.exists():
        shutil.rmtree(run)
    run.mkdir()
    if isinstance(area, str):
        (run / 'save.bin').write_bytes(saves[area])
    shutil.copy2(a.exe, run / a.exe.name)
    for dll in a.exe.parent.glob('*.dll'):
        shutil.copy2(dll, run / dll.name)
    (run / 'rka-video.ini').write_text(f'spawn_range={a.spawn_range}\n')
    (run / 'route.input').write_text(script_for(area, run))
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
    (run / 'debug.ini').write_text(f'port={port}\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('GENESIS_', 'SDL_', 'LNG_', 'RNET_'))}
    env.update(SDL_VIDEODRIVER='dummy', SDL_RENDER_DRIVER='software', SDL_AUDIODRIVER='dummy',
               GENESIS_RUN_DONE='1', GENESIS_NO_LAUNCHER='1')
    cmd = [str(run / a.exe.name), str(root / 'game/rka.bin'), '--no-launcher', '--benchmark', '1000000',
           '--input-script', str(run / 'route.input')]
    if a.mode != 'off':
        cmd += ['--widescreen', a.mode]
    # Native runs record only interpreter-floor PCs; force-interp records all.
    cmd += ['--exec-coverage-out', str(run / 'exec_pcs.txt')]
    if a.force_interp:
        env['GENESIS_FORCE_INTERP'] = '1'
    with (run / 'process.log').open('w') as log:
        proc = subprocess.Popen(cmd, cwd=run, env=env, stdout=log, stderr=subprocess.STDOUT,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    result = dict(area=area, ok=False)
    try:
        deadline = time.monotonic() + 60 + a.frames / (5 if a.force_interp else 30)
        while not (run / 'done.png').exists() and time.monotonic() < deadline and proc.poll() is None:
            time.sleep(0.05)
        if proc.poll() is not None or not (run / 'done.png').exists():
            result['error'] = 'exited' if proc.poll() is not None else 'timeout'
            return result

        def query(name, **kw):
            with socket.create_connection(('127.0.0.1', port), timeout=10) as s:
                s.sendall((json.dumps(dict(id=1, cmd=name, **kw)) + '\n').encode())
                return json.loads(s.makefile('rb').readline())
        # The game keeps running while we page; stop at the count seen first,
        # or a scene spawning every frame outruns the reader forever.
        spawns, since, end = [], 0, None
        while end is None or since < end:
            reply = query('rka_spawns', since=since)
            end = reply['count'] if end is None else end
            page = [e for e in reply['spawns'] if e['serial'] < end]
            if not page:
                break
            spawns += page
            since = page[-1]['serial'] + 1
        (run / 'spawns.json').write_text(json.dumps(spawns))
        misses = query('dispatch_miss_info')
        video = query('custom_video')
        query('quit')
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
        floor = (run / 'floor_coverage.toml').read_text() if (run / 'floor_coverage.toml').exists() else ''
        result.update(ok=True, spawns=len(spawns), misses=misses.get('unique', []),
                      floor=floor.count('0x'), scene=video.get('scene'))
        return result
    finally:
        if proc.poll() is None:
            proc.kill()


with cf.ThreadPoolExecutor(a.jobs) as ex:
    results = list(ex.map(run_area, list(saves) if saves else areas))
for r in results:
    print(json.dumps(r), flush=True)
(out / 'summary.json').write_text(json.dumps(results, indent=1))
