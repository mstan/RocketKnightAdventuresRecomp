"""Reproduce owner-supplied RKA saves in isolated, hidden native/wide runs."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile
from PIL import Image, ImageDraw

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--downloads', type=Path, default=Path('E:/Downloads'))
p.add_argument('--stage', type=int)
p.add_argument('--zip', action='append', default=[], metavar='LABEL=PATH',
               help='replay saves from this ZIP under LABEL (repeatable); default stage 1-4 ZIPs')
p.add_argument('--every', type=int, default=0, help='also capture a screenshot every N frames while moving')
p.add_argument('--ram-every', type=int, default=0, help='also dump guest RAM every N frames while moving')
p.add_argument('--modes', default='off,16:9', help='comma-separated video modes to replay')
p.add_argument('--hold', default='RIGHT', help='button(s) held during the moved capture, e.g. RIGHT or LEFT')
p.add_argument('--slot', type=int)
p.add_argument('--save-file', type=Path, help='replace the selected ZIP entry with this native save')
p.add_argument('--frames', type=int, default=120)
p.add_argument('--spawn-range', choices=('expanded', 'original'), default='expanded')
p.add_argument('--cheats', default='', help='Playtest Cheats ini keys, e.g. invincible=1,infinite_lives=1')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
env = {k: v for k, v in os.environ.items() if not k.startswith(('GENESIS_', 'SDL_', 'LNG_', 'RNET_'))}
env.update(SDL_VIDEODRIVER='dummy', SDL_RENDER_DRIVER='software', SDL_AUDIODRIVER='dummy',
           GENESIS_RUN_DONE='1', GENESIS_NO_LAUNCHER='1')
summary = []
sources = [tuple(z.split("=", 1)) for z in a.zip] or \
          [(str(s), a.downloads / f'stage {s} saves.zip') for s in ([a.stage] if a.stage else range(1, 5))]
for stage, zpath in sources:
    with zipfile.ZipFile(zpath) as z:
        for item in z.infolist():
            if not item.filename.endswith('.bin'):
                continue
            slot = int(Path(item.filename).stem.rsplit('_', 1)[1])
            if a.slot is not None and slot != a.slot:
                continue
            for mode in a.modes.split(','):
                run = out / f'stage{stage}-save{slot}-{mode.replace(":", "x")}'
                run.mkdir(exist_ok=True)
                exe = run / a.exe.name
                shutil.copy2(a.exe, exe)
                for dll in a.exe.parent.glob('*.dll'):
                    shutil.copy2(dll, run / dll.name)
                save = a.save_file.read_bytes() if a.save_file else z.read(item)
                (run / 'save.bin').write_bytes(save)
                cheats = ''.join(f'{kv}\n' for kv in a.cheats.split(',') if kv)
                (run / 'rka-video.ini').write_text(f'spawn_range={a.spawn_range}\n{cheats}')
                # Let reset reach RKA's idle loop before replacing guest state.
                script = 'WAIT 600\nLOAD_STATE ' + (run / 'save.bin').as_posix() + '\nWAIT 3\n'
                # Owner's own-backend saves have a 101-byte header before
                # guest RAM; B014 bit 7 is the game's pause state.
                paused = len(save) == 157106 and bool(save[101 + 0xB014] & 128)
                unpause = 'PRESS START 2\nWAIT 4\n' if paused else ''
                hold = ''.join(f'PRESS {b} {a.frames}\n' for b in a.hold.split('+') if b)
                for name, movement in (('loaded', ''), ('moved', unpause + hold)):
                    script += movement
                    if name == 'moved':
                        # Periodic frames expose scroll-time defects that a
                        # single end capture can miss.
                        shots = set(range(a.every, a.frames, a.every)) if a.every else set()
                        dumps = set(range(a.ram_every, a.frames, a.ram_every)) if a.ram_every else set()
                        last = 0
                        for t in sorted(shots | dumps):
                            script += f'WAIT {t - last}\n'
                            if t in shots:
                                script += f'SCREENSHOT {(run / f"moved-{t:04d}.png").as_posix()}\n'
                            if t in dumps:
                                script += f'DUMP_RAM {(run / f"ram-{t:04d}.bin").as_posix()}\n'
                            last = t
                        script += f'WAIT {a.frames - last}\n'
                    for op, ext in (('SCREENSHOT', 'png'), ('DUMP_RAM', 'ram'), ('DUMP_VRAM', 'vram')):
                        script += f'{op} {(run / (name + "." + ext)).as_posix()}\n'
                    script += 'WAIT 1\n'
                script += 'EXIT 0\n'
                (run / 'route.input').write_text(script)
                command = [str(exe), str(root / 'game/rka.bin'), '--no-launcher', '--benchmark', str(max(3000, a.frames + 1000)),
                           '--input-script', str(run / 'route.input')]
                if mode != 'off':
                    command += ['--widescreen', mode]
                with (run / 'process.log').open('w') as log:
                    result = subprocess.run(command, cwd=run, env=env, stdout=log, stderr=subprocess.STDOUT,
                                            timeout=120, creationflags=subprocess.CREATE_NO_WINDOW)
                record = dict(stage=stage, slot=slot, mode=mode, exit=result.returncode)
                for name in ('loaded', 'moved'):
                    path = run / f'{name}.ram'
                    if path.exists():
                        ram = path.read_bytes()
                        word = lambda x: int.from_bytes(ram[x:x+2], 'big')
                        record[name] = {f'{x:04X}': word(x) for x in
                                        (0xB002, 0xB004, 0xB1D0, 0xB1D2, 0xB1D6, 0xB1E8, 0xFB12, 0xC106)}
                summary.append(record)
                print(json.dumps(record), flush=True)
    rows = [r for r in summary if r['stage'] == stage]
    sheet = Image.new('RGB', (840, (len(rows) + 1) // 2 * 280), '#222222')
    draw = ImageDraw.Draw(sheet)
    moved_sheet = Image.new('RGB', sheet.size, '#222222')
    moved_draw = ImageDraw.Draw(moved_sheet)
    for i, r in enumerate(rows):
        run = out / f'stage{stage}-save{r["slot"]}-{r["mode"].replace(":", "x")}'
        x, y = (i % 2) * 420, (i // 2) * 280
        draw.text((x+4, y+4), f'Stage {stage} save {r["slot"]} {r["mode"]}', fill='white')
        if (run / 'loaded.png').exists():
            im = Image.open(run / 'loaded.png').convert('RGB')
            im.thumbnail((410, 250))
            sheet.paste(im, (x+4, y+24))
        moved_draw.text((x+4, y+4), f'Stage {stage} save {r["slot"]} {r["mode"]}', fill='white')
        if (run / 'moved.png').exists():
            im = Image.open(run / 'moved.png').convert('RGB')
            im.thumbnail((410, 250))
            moved_sheet.paste(im, (x+4, y+24))
    sheet.save(out / f'stage{stage}-contact.png')
    moved_sheet.save(out / f'stage{stage}-moved-contact.png')
(out / 'summary.json').write_text(json.dumps(summary, indent=2))
