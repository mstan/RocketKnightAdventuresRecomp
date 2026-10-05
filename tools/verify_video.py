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
p.add_argument("--continue-save", type=Path,
               help="Optional local save captured on the Continue screen")
p.add_argument("--apple-save", type=Path,
               help="Optional local stage-1 save with apples beyond the native view")
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)


def capture(name, mode, spawn="original", castle=False, resize=False, route=None, frames=None,
            level=True):
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
    if level:
        assert diag["dispatch_miss_info"]["count"] == 0, diag
    else:
        # The longer attract run legitimately exercises interpreter-floor
        # dispatches. None may be a new, unresolved address.
        assert diag["dispatch_miss_info"]["unique_count"] == 0, diag
    if mode != "off" and level and diag["custom_video"]["scene"] == "tiled":
        assert diag["custom_video"]["terrain_checks"] > 0, diag
        assert diag["custom_video"]["terrain_errors"] == 0, diag
        assert diag["custom_video"]["sprite_overflow"] == 0, diag
    return dest


def view_left(ram, camera, width):
    """Mirror of rka_video.c view_left() for tiled scenes."""
    word = lambda offset: int.from_bytes(ram[offset:offset + 2], "big")
    signed = lambda v: v - 65536 if v & 0x8000 else v
    stage = word(0xB1E8) + 320
    lo, hi = signed(word(0xB39C)), signed(word(0xB39E)) + 320
    if lo < 0 or hi < lo + 320 or hi > stage:
        lo, hi = 0, stage
    lo, hi = min(lo, camera), max(hi, camera + 320)
    if hi - lo < width:
        return (lo + hi - width) // 2
    return max(lo, min(camera - (width - 320) // 2, hi - width))


def parity(native, wide):
    for ext in ("ram", "vram"):
        assert (native / f"scene.{ext}").read_bytes() == (wide / f"scene.{ext}").read_bytes(), ext
    ram = (native / "scene.ram").read_bytes()
    word = lambda data, offset: int.from_bytes(data[offset:offset + 2], "big")
    # Tiled views anchor on the game camera; raster shake/ripple stays per line.
    camera = word(ram, 0xB1D2)
    ni = Image.open(native / "scene.png").convert("RGB")
    wi = Image.open(wide / "scene.png").convert("RGB")
    center = (wi.width - ni.width) // 2
    origin = camera - view_left(ram, camera, wi.width)
    # A centered view copies the hardware image exactly. At a clamped room
    # edge the wider camera keeps its own background parallax, so only the
    # guest state and the centered HUD are exact there.
    if origin == center:
        assert ImageChops.difference(ni.crop((0, 32, 320, 224)),
                                    wi.crop((origin, 32, origin + 320, 224))).getbbox() is None
    assert ImageChops.difference(ni.crop((0, 0, 320, 32)),
                                wi.crop((center, 0, center + 320, 32))).getbbox() is None
    return {"width": wi.width, "native_origin": origin, "ram_equal": True,
            "vram_equal": True, "native_pixels_equal": origin == center}


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
# Keep the centered counters and cover both HUD margins through all 32 rows.
im = Image.open(stage_expanded / "scene.png").convert("RGB")
center = (im.width - 320) // 2
assert im.crop((16,0,center,8)).getbbox() is not None
for y in range(32):
    for x in list(range(center)) + list(range(center+320,im.width)):
        assert im.getpixel((x,y)) == im.getpixel((center+((x-center)&31),y)), (x,y)
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
# The castle is a sprite with its own camera term. At 32:9 a thrown enemy
# helmet crosses (341,66)-(349,71) in the walking run; compare the clear body
# here and include the flying detail in the unobstructed 48:9 crop below.
for box in [(315,75,390,125),(500,32,760,64),(600,128,700,144)]:
    assert ImageChops.difference(ii.crop(box),iw.crop(box)).getbbox() is None, box
report["edge_camera"] = {"idle": di, "walk": dw, "castle_clouds_and_mountains_match": True}

# Cover the owner's wider-than-32:9 display, including fractional native camera X.
ultra_idle = capture("edge-idle-ultrawide", "48:9", route=idle_route, frames=100)
ultra_walk = capture("edge-walk-ultrawide", "48:9", route=walk_route, frames=20)
ui=Image.open(ultra_idle / "scene.png").convert("RGB")
uw=Image.open(ultra_walk / "scene.png").convert("RGB")
assert ui.width == uw.width == 1195
# Walking activates foreground pigs whose helmets reach y=108 in front of
# the castle. Its upper silhouette and flying detail remain unobstructed.
box=(490,60,560,104)
assert ImageChops.difference(ui.crop(box),uw.crop(box)).getbbox() is None
for ordinary,ultra in [(idle,ultra_idle),(walk,ultra_walk)]:
    for ext in ("ram","vram"):
        assert (ordinary/f"scene.{ext}").read_bytes()==(ultra/f"scene.{ext}").read_bytes()
report["ultrawide_castle"]={"width":ui.width,"castle_matches":True,"ram_vram_equal":True}

# The attract loop reaches the flying city at frame 4501. Its finite tilemap
# used to repeat across a 1392px view (the owner's 2785x448 window at 2x scale).
city_native = capture("city-native", "off", castle=True, frames=4500)
city_wide = capture("city-ultrawide", "1392:224", castle=True, frames=4500)
for ext in ("ram", "vram"):
    assert (city_native / f"scene.{ext}").read_bytes() == (city_wide / f"scene.{ext}").read_bytes(), ext
cn = Image.open(city_native / "scene.png").convert("RGB")
cw = Image.open(city_wide / "scene.png").convert("RGB")
assert cw.width == 1392
# Map-1 set pieces are centered: the native image sits exactly in the middle,
# and beyond 128px of fog both sides are one flat color per row.
cc = (cw.width - 320) // 2
assert ImageChops.difference(cn.crop((0,32,320,224)), cw.crop((cc,32,cc+320,224))).getbbox() is None
for y in range(40, 224):
    for x1, x2 in ((0, cc - 128), (cc + 320 + 128, cw.width)):
        assert len(cw.crop((x1,y,x2,y+1)).getcolors(maxcolors=2) or []) == 1, (y, x1)

report["ultrawide_presentation"] = {"city_native_pixels_equal": True,
    "city_extra_background_diffused": True}
if a.continue_save:
    menu_route = out / "continue.input"
    menu_route.write_text(f"WAIT 600\nLOAD_STATE {a.continue_save.resolve().as_posix()}\nWAIT 3\n")
    menu_native = capture("continue-native", "off", route=menu_route, frames=0,
                          level=False)
    menu_wide = capture("continue-ultrawide", "1392:224", route=menu_route,
                        frames=0, level=False)
    for ext in ("ram", "vram"):
        assert (menu_native / f"scene.{ext}").read_bytes() == (menu_wide / f"scene.{ext}").read_bytes(), ext
    mn = Image.open(menu_native / "scene.png").convert("RGB")
    mw = Image.open(menu_wide / "scene.png").convert("RGB")
    center = (mw.width - 320) // 2
    assert ImageChops.difference(mn, mw.crop((center,0,center+320,224))).getbbox() is None
    for y in range(224):
        left = mw.crop((0,y,center,y+1)).getcolors(maxcolors=2) or []
        right = mw.crop((center+320,y,mw.width,y+1)).getcolors(maxcolors=2) or []
        assert len(left) == len(right) == 1 and left[0][1] == right[0][1], y
    report["ultrawide_presentation"]["continue_centered"] = True

if a.apple_save:
    apple_route = out / "apples.input"
    apple_route.write_text(f"WAIT 600\nLOAD_STATE {a.apple_save.resolve().as_posix()}\nWAIT 3\n")
    apple_native = capture("apples-native", "off", route=apple_route, frames=0)
    apple_original = capture("apples-original", "1392:224", route=apple_route,
                             frames=0)
    apple_expanded = capture("apples-expanded", "1392:224", spawn="expanded",
                             route=apple_route, frames=0)
    origin = parity(apple_native, apple_original)["native_origin"]
    native_im = Image.open(apple_native / "scene.png").convert("RGB")
    wide_im = Image.open(apple_expanded / "scene.png").convert("RGB")
    center = (wide_im.width - 320) // 2
    real_apple = (wide_im.getpixel((x+origin-center,y)) for y in range(85,96)
                  for x in range(91,105))
    assert sum(r > 200 and g < 100 and b < 80 for r,g,b in real_apple) > 20
    # Native SAT X is nine bits. Previously apples at x=91 and x=252 were
    # also copied into the centered viewport at x+512.
    # The room clamp places the native view at `origin`, where the wider
    # camera re-renders background parallax, so count apple-red pixels instead
    # of comparing them: a wrapped hardware copy would add them, the native
    # image has none there.
    red = lambda im, box: sum(r > 200 and g < 100 and b < 80
                              for r, g, b in im.crop(box).get_flattened_data())
    for x1,y1,x2,y2 in ((602,84,618,97),(763,84,779,97)):
        nx1, nx2 = x1 - center, x2 - center
        assert red(native_im, (nx1,y1,nx2,y2)) == 0, (nx1, "native")
        assert red(wide_im, (nx1+origin,y1,nx2+origin,y2)) == 0, (nx1, origin)
    ram = (apple_expanded / "scene.ram").read_bytes()
    apples = sum(ram[o:o+2] == b"\x00\x64" for o in range(0xC100,0xE000,0x80))
    assert apples >= 6, apples
    report["apple_wrap"] = {"real_apples_active": apples,
                            "wrapped_copies_absent": True}

(out / "verification.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print(f"PASS: native parity, activation, resize, HUD, castle parallax, ultrawide city"
      f"{' and Continue screen' if a.continue_save else ''}"
      f"{' and apple wrap' if a.apple_save else ''}; {out / 'verification.json'}")
