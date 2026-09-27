#!/usr/bin/env python3
"""ROM-backed renderer regression: native parity, activation, adaptive resize.

Requires Pillow and an RKARecomp build with GEN_ENABLE_TRACE=ON.
The owner supplies the ROM; no game data or golden images are distributed.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from PIL import Image, ImageChops

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--exe", type=Path, required=True)
p.add_argument("--rom", type=Path, required=True)
p.add_argument("--out", type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)


def capture(name, mode, spawn="original", castle=False, resize=False, route=None, frames=None):
    dest = out / name
    cmd = [sys.executable, str(root / "tools/capture_rka.py"),
           "--exe", str(a.exe.resolve()), "--rom", str(a.rom.resolve()),
           "--out", str(dest), "--mode", mode, "--spawn-range", spawn,
           "--inspect", "--frames", str(frames if frames is not None else 1800 if castle else 300)]
    cmd += ["--attract"] if castle else ["--route", str(route or root / "tests/stage1-walk.input")]
    if resize:
        cmd.append("--resize-check")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=240)
    (out / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    assert result.returncode == 0, f"{name} failed; see {out / (name + '.log')}"
    diag = json.loads((dest / "diagnostics.json").read_text())
    assert diag["dispatch_miss_info"]["count"] == 0, diag
    if mode != "off":
        assert diag["custom_video"]["terrain_checks"] > 0, diag
        assert diag["custom_video"]["terrain_errors"] == 0, diag
        assert diag["custom_video"]["sprite_overflow"] == 0, diag
    return dest


def parity(native, wide):
    for ext in ("ram", "vram"):
        assert (native / f"scene.{ext}").read_bytes() == (wide / f"scene.{ext}").read_bytes(), ext
    ram = (native / "scene.ram").read_bytes()
    vram = (native / "scene.vram").read_bytes()
    word = lambda data, offset: int.from_bytes(data[offset:offset + 2], "big")
    camera = word(ram, 0xB1D2)
    camera += (-word(vram, 0xAC00) - camera + 512) % 1024 - 512
    ni = Image.open(native / "scene.png").convert("RGB")
    wi = Image.open(wide / "scene.png").convert("RGB")
    center = (wi.width - ni.width) // 2
    left = max(0, min(camera - center, max(0, word(ram, 0xB1E8) + 320 - wi.width)))
    origin = camera - left
    # The world follows the clamped camera; the HUD remains centered.
    assert ImageChops.difference(ni.crop((0, 32, 320, 224)),
                                wi.crop((origin, 32, origin + 320, 224))).getbbox() is None
    assert ImageChops.difference(ni.crop((0, 0, 320, 32)),
                                wi.crop((center, 0, center + 320, 32))).getbbox() is None
    return {"width": wi.width, "native_origin": origin, "ram_equal": True,
            "vram_equal": True, "native_pixels_equal": True}


castle_native = capture("castle-native", "off", castle=True)
castle_wide = capture("castle-original", "16:9", castle=True)
stage_native = capture("stage-native", "off")
stage_wide = capture("stage-original", "32:9")
stage_expanded = capture("stage-expanded", "32:9", spawn="expanded", resize=True)
report = {"castle": parity(castle_native, castle_wide),
          "stage1": parity(stage_native, stage_wide)}


def actor_count(directory):
    ram = (directory / "scene.ram").read_bytes()
    return sum(ram[o:o + 2] != b"\0\0" for o in range(0xC100, 0xE000, 0x80))


original, expanded = actor_count(stage_wide), actor_count(stage_expanded)
assert expanded > original, (original, expanded)
diag = json.loads((stage_expanded / "diagnostics.json").read_text())["custom_video"]
assert diag["spawned"] > 0 and diag["spawn_mode"] == "expanded", diag
# Extended HUD uses the original end pillars and ornament, with no repeated counters.
im = Image.open(stage_expanded / "scene.png").convert("RGB")
center = (im.width - 320) // 2
assert ImageChops.difference(im.crop((0,0,16,32)),im.crop((center,0,center+16,32))).getbbox() is None
assert ImageChops.difference(im.crop((im.width-16,0,im.width,32)),im.crop((center+304,0,center+320,32))).getbbox() is None
assert im.crop((16,0,center,8)).getbbox() is not None
assert im.crop((16,24,center,32)).getbbox() is not None
report["activation"] = {"original_actors": original, "expanded_actors": expanded,
                        "successful_spawns": diag["spawned"]}
report["resize"] = json.loads((stage_expanded / "resize.json").read_text())

# Reproduce walking at the wide camera's left limit. The native camera moves
# from 2 to 235, while the wide camera remains at zero. Clouds keep their
# independent motion, and camera-driven mountain strips must stay stationary.
prelude = (root / "tests/stage1-walk.input").read_text().split("HOLD RIGHT")[0]
idle_route, walk_route = out / "edge-idle.input", out / "edge-walk.input"
idle_route.write_text(prelude, encoding="utf-8")
walk_route.write_text(prelude + "HOLD RIGHT\nWAIT 80\nRELEASE RIGHT\n", encoding="utf-8")
idle = capture("edge-idle", "32:9", route=idle_route, frames=100)
walk = capture("edge-walk", "32:9", route=walk_route, frames=20)
di = json.loads((idle / "diagnostics.json").read_text())["custom_video"]
dw = json.loads((walk / "diagnostics.json").read_text())["custom_video"]
assert di["view_left"] == dw["view_left"] == 0, (di, dw)
assert dw["camera"] > di["camera"] + 100, (di, dw)
assert di["bg_x_128"] == dw["bg_x_128"], (di, dw)
ii = Image.open(idle / "scene.png").convert("RGB")
iw = Image.open(walk / "scene.png").convert("RGB")
for box in [(500,32,760,64),(600,128,700,144)]:
    assert ImageChops.difference(ii.crop(box),iw.crop(box)).getbbox() is None, box
report["edge_camera"] = {"idle": di, "walk": dw, "clouds_and_mountains_match": True}
(out / "verification.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print(f"PASS: native RAM/VRAM/pixels, expanded activation, resize, HUD, edge parallax; {out / 'verification.json'}")
