"""Load one owner save in a hidden 16:9 run and query the live debug server.

Prints custom_video (scene class, room, wide/native line counts) and selected
VDP registers after --frames frames, without stopping the game.
"""
import argparse
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
p.add_argument('--zip', type=Path, required=True)
p.add_argument('--slot', type=int, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--mode', default='16:9')
p.add_argument('--frames', type=int, default=30)
p.add_argument('--hold', default='', help='button(s) held after loading, e.g. RIGHT or RIGHT+B')
p.add_argument('--query', action='append', default=[], help='extra debug command names')
p.add_argument('--force-interp', action='store_true', help='interpret everything; write exec_pcs.txt')
p.add_argument('--spawns', action='store_true', help='page the always-on spawn ring into spawns.json')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
shutil.copy2(a.exe, out / a.exe.name)
for dll in a.exe.parent.glob('*.dll'):
    shutil.copy2(dll, out / dll.name)
save = zipfile.ZipFile(a.zip).read(f'native_save_{a.slot}.bin')
(out / 'save.bin').write_bytes(save)
(out / 'rka-video.ini').write_text('spawn_range=expanded\n')
paused = len(save) == 157106 and bool(save[101 + 0xB014] & 128)
hold = ''.join(f'PRESS {b} {a.frames + 600}\n' for b in a.hold.split('+') if b)
script = ('WAIT 600\nLOAD_STATE ' + (out / 'save.bin').as_posix() + '\nWAIT 3\n' +
          ('PRESS START 2\nWAIT 4\n' if paused else '') + hold +
          f'WAIT {a.frames}\nSCREENSHOT {(out / "probe.png").as_posix()}\nWAIT 60000\n')
(out / 'route.input').write_text(script)
with socket.socket() as s:
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
(out / 'debug.ini').write_text(f'port={port}\n')
env = {k: v for k, v in os.environ.items() if not k.startswith(('GENESIS_', 'SDL_', 'LNG_', 'RNET_'))}
# No host controller may reach a scripted run.
env.update(SDL_JOYSTICK_RAWINPUT='0', SDL_JOYSTICK_HIDAPI='0', SDL_JOYSTICK_WGI='0',
           SDL_DIRECTINPUT_ENABLED='0', SDL_XINPUT_ENABLED='0',
           SDL_VIDEODRIVER='dummy', SDL_RENDER_DRIVER='software', SDL_AUDIODRIVER='dummy',
           GENESIS_RUN_DONE='1', GENESIS_NO_LAUNCHER='1')
cmd = [str(out / a.exe.name), str(root / 'game/rka.bin'), '--no-launcher', '--benchmark', '100000',
       '--input-script', str(out / 'route.input')]
cmd += ['--widescreen', a.mode]  # explicit: games may default to widescreen
cmd += ['--exec-coverage-out', str(out / 'exec_pcs.txt')]
if a.force_interp:
    env['GENESIS_FORCE_INTERP'] = '1'
proc = subprocess.Popen(cmd, cwd=out, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
try:
    deadline = time.monotonic() + 120
    while not (out / 'probe.png').exists() and time.monotonic() < deadline and proc.poll() is None:
        time.sleep(0.02)

    def query(name, **kw):
        with socket.create_connection(('127.0.0.1', port), timeout=5) as s:
            s.sendall((json.dumps(dict(id=1, cmd=name, **kw)) + '\n').encode())
            return json.loads(s.makefile('rb').readline())
    result = {name: query(name) for name in ['custom_video', 'vdp_state'] + a.query}
    if a.spawns:
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
        (out / 'spawns.json').write_text(json.dumps(spawns, indent=0))
        result['spawn_count'] = len(spawns)
    print(json.dumps(result, indent=1))
    (out / 'probe.json').write_text(json.dumps(result, indent=1))
    query('quit')
finally:
    try:
        proc.wait(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
