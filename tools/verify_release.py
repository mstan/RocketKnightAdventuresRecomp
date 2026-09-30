#!/usr/bin/env python3
"""Exercise a Windows ZIP or Linux AppImage using an owner-supplied USA ROM.

Requires Pillow. Optional --reference compares every captured pixel and all
RAM/VRAM against a previous renderer verification or the other platform.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile
from PIL import Image, ImageChops

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--package", type=Path, required=True)
p.add_argument("--rom", type=Path, required=True)
p.add_argument("--out", type=Path, required=True)
p.add_argument("--reference", type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
prelude = (root / "tests/stage1-walk.input").read_text().split("HOLD RIGHT")[0]
report = {"package_sha256": hashlib.sha256(a.package.read_bytes()).hexdigest(), "cases": {}}


def capture(name, mode, spawn="original", castle=False, edge=None):
    dest = out / name
    dest.mkdir()
    if a.package.suffix == ".zip":
        with zipfile.ZipFile(a.package) as z:
            for entry in z.namelist():
                assert not entry.startswith("/") and ".." not in Path(entry).parts and "\\" not in entry
                assert Path(entry).suffix.lower() not in (".bin", ".gen", ".smd", ".srm", ".log", ".map")
            z.extractall(dest)
        command = [str(dest / "RKARecomp.exe")]
    else:
        exe = dest / a.package.name
        shutil.copy2(a.package, exe)
        exe.chmod(0o755)
        command = [str(exe), "--appimage-extract-and-run"]
    (dest / "rka-video.ini").write_text(f"spawn_range={spawn}\n")
    if castle:
        route = "WAIT 1800\n"
    elif edge == "idle":
        route = prelude + "WAIT 100\n"
    elif edge == "walk":
        route = prelude + "HOLD RIGHT\nWAIT 80\nRELEASE RIGHT\nWAIT 20\n"
    else:
        route = prelude + "HOLD RIGHT\nHOLD A\nWAIT 300\n"
    route += (f"SCREENSHOT {(dest / 'scene.png').as_posix()}\n"
              f"DUMP_RAM {(dest / 'scene.ram').as_posix()}\n"
              f"DUMP_VRAM {(dest / 'scene.vram').as_posix()}\nEXIT 0\n")
    (dest / "route.input").write_text(route)
    command += [str(a.rom.resolve()), "--no-launcher", "--benchmark", "16000",
                "--input-script", str(dest / "route.input")]
    if mode != "off":
        command += ["--widescreen", mode]
    env = {k: v for k, v in os.environ.items() if not k.startswith(("SDL_", "GENESIS_", "RNET_", "LNG_"))}
    env.update(SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software", SDL_AUDIODRIVER="dummy", GENESIS_RUN_DONE="1")
    if os.name == "nt":
        system_root = os.environ["SystemRoot"]
        env["PATH"] = system_root + "\\System32;" + system_root
    with (dest / "process.log").open("w") as log:
        result = subprocess.run(command, cwd=dest, env=env, stdout=log, stderr=subprocess.STDOUT,
                                timeout=240, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    assert result.returncode == 0, f"{name}: exit {result.returncode}, see process.log"
    for suffix in ("png", "ram", "vram"):
        assert (dest / f"scene.{suffix}").is_file(), f"{name}: missing {suffix}"
        if a.reference:
            actual, expected = dest / f"scene.{suffix}", a.reference / name / f"scene.{suffix}"
            if suffix == "png":
                assert Image.open(actual).size == Image.open(expected).size
                assert ImageChops.difference(Image.open(actual).convert("RGB"), Image.open(expected).convert("RGB")).getbbox() is None, f"{name}: pixel mismatch"
            else:
                assert actual.read_bytes() == expected.read_bytes(), f"{name}: {suffix} mismatch"
    report["cases"][name] = {"size": Image.open(dest / "scene.png").size,
                              "reference_equal": bool(a.reference)}
    print(f"PASS {name}", flush=True)
    return dest


def parity(native, wide):
    for ext in ("ram", "vram"):
        assert (native / f"scene.{ext}").read_bytes() == (wide / f"scene.{ext}").read_bytes(), ext
    ram, vram = (native / "scene.ram").read_bytes(), (native / "scene.vram").read_bytes()
    word = lambda data, offset: int.from_bytes(data[offset:offset+2], "big")
    camera = word(ram, 0xB1D2)
    camera += (-word(vram, 0xAC00) - camera + 512) % 1024 - 512
    ni, wi = Image.open(native / "scene.png").convert("RGB"), Image.open(wide / "scene.png").convert("RGB")
    center = (wi.width - ni.width) // 2
    left = max(0, min(camera-center, max(0, word(ram, 0xB1E8)+320-wi.width)))
    origin = camera-left
    assert ImageChops.difference(ni.crop((0,32,320,224)), wi.crop((origin,32,origin+320,224))).getbbox() is None
    assert ImageChops.difference(ni.crop((0,0,320,32)), wi.crop((center,0,center+320,32))).getbbox() is None


parity(capture("castle-native", "off", castle=True), capture("castle-original", "16:9", castle=True))
native = capture("stage-native", "off")
original = capture("stage-original", "32:9")
parity(native, original)
expanded = capture("stage-expanded", "32:9", "expanded")
def actors(folder):
    ram = (folder / "scene.ram").read_bytes()
    return sum(ram[o:o+2] != b"\0\0" for o in range(0xC100,0xE000,0x80))
assert actors(expanded) > actors(original)
report["actors"] = {"original": actors(original), "expanded": actors(expanded)}
im = Image.open(expanded / "scene.png").convert("RGB")
center = (im.width-320)//2
assert im.crop((16,0,center,8)).getbbox()
# The live border art contains black pixels by design. Verify that both
# margins use the undecorated 32-pixel HUD border across every HUD row.
for y in range(32):
    for x in list(range(center)) + list(range(center+320, im.width)):
        assert im.getpixel((x,y)) == im.getpixel((center+((x-center)&31),y)), (x,y)
idle = Image.open(capture("edge-idle", "32:9", edge="idle") / "scene.png").convert("RGB")
walk = Image.open(capture("edge-walk", "32:9", edge="walk") / "scene.png").convert("RGB")
for box in ((315,75,390,125),(500,32,760,64),(600,128,700,144)):
    assert ImageChops.difference(idle.crop(box),walk.crop(box)).getbbox() is None
report.update(native_parity=True, slim_hud=True, castle_and_plane_parallax=True)
(out / "verification.json").write_text(json.dumps(report, indent=2))
print(f"PASS packaged release: {out / 'verification.json'}")
