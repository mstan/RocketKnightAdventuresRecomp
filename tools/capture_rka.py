#!/usr/bin/env python3
"""Run one isolated RKA input route with hidden SDL output and saved evidence."""
import argparse
import os
from pathlib import Path
import subprocess
import shutil
import socket
import json
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--exe", type=Path, required=True)
p.add_argument("--rom", type=Path, required=True)
p.add_argument("--out", type=Path, required=True)
p.add_argument("--mode", default="off")
p.add_argument("--spawn-range", choices=("expanded", "original"), default="expanded")
p.add_argument("--attract", action="store_true")
p.add_argument("--cheats", default="", help="enable Playtest Cheats, e.g. start_stage=4,infinite_lives=1")
p.add_argument("--frames", type=int, default=3400)
p.add_argument("--inspect", action="store_true", help="Query the live renderer after the capture")
p.add_argument("--resize-check", action="store_true", help="Also verify live adaptive resizing (requires --inspect)")
p.add_argument("--route", type=Path, help="Input prelude instead of the built-in menu navigation")
a = p.parse_args()
if a.resize_check and not a.inspect:
    p.error("--resize-check requires --inspect")
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
exe = out / a.exe.name
shutil.copy2(a.exe, exe)
shutil.copy2(a.exe.parent / "SDL2.dll", out / "SDL2.dll")
cheats = "cheats=1\n" + "".join(f"{kv}\n" for kv in a.cheats.split(",")) if a.cheats else ""
(out / "rka-video.ini").write_text(f"spawn_range={a.spawn_range}\n{cheats}", encoding="utf-8")
route = "" if a.attract else (
    "WAIT_RAM16 FFB002 0001\nWAIT_RAM16 FFB004 0001\nPRESS START 2\nWAIT 4\n"
    "WAIT_RAM16 FFB004 0004\nPRESS START 2\nWAIT 4\n"
    "WAIT_RAM16 FFB002 0004\nWAIT_RAM16 FFB194 0000\n")
if a.route:
    route = a.route.read_text(encoding="utf-8") + "\n"
route += (f"WAIT {a.frames}\nSCREENSHOT {(out / 'scene.png').as_posix()}\n"
          f"DUMP_RAM {(out / 'scene.ram').as_posix()}\n"
          f"DUMP_VRAM {(out / 'scene.vram').as_posix()}\n" + ("WAIT 60000\n" if a.inspect else "EXIT 0\n"))
(out / "route.input").write_text(route, encoding="utf-8")
env = {k: v for k, v in os.environ.items()
       if not k.startswith(("GENESIS_", "SDL_", "LNG_", "RNET_"))}
env.update(SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software", SDL_AUDIODRIVER="dummy",
           GENESIS_RUN_DONE="1", GENESIS_NO_LAUNCHER="1")
cmd = [str(exe), str(a.rom.resolve()), "--no-launcher", "--benchmark", "16000",
       "--input-script", str(out / "route.input")]
if a.mode != "off":
    cmd += ["--widescreen", a.mode]
if a.inspect:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1",0));port=sock.getsockname()[1]
    (out / "debug.ini").write_text(f"port={port}\n",encoding="utf-8")
with (out / "process.log").open("w", encoding="utf-8") as log:
    process = subprocess.Popen(cmd, cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT,
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    try:
        if a.inspect:
            deadline=time.monotonic()+150
            while not (out / "scene.png").exists() and process.poll() is None and time.monotonic()<deadline:
                time.sleep(0.01)
            def query(command, **values):
                with socket.create_connection(("127.0.0.1",port),timeout=5) as sock:
                    sock.sendall((json.dumps(dict(id=1,cmd=command,**values))+"\n").encode())
                    return json.loads(sock.makefile("rb").readline())
            if process.poll() is None:
                data={cmd:query(cmd) for cmd in ("custom_video","vdp_state","dispatch_miss_info",
                                                  "custom_video_sprites")}
                # Savestate loading resets the current scene's frame counter,
                # while the diagnostic history keeps its process-wide count.
                # Query that count before requesting the most recent frames.
                history=query("custom_video_history")
                data["custom_video_history"]=query(
                    "custom_video_history", since=max(0,history["count"]-64))
                (out / "diagnostics.json").write_text(json.dumps(data,indent=2),encoding="utf-8")
                if a.resize_check:
                    sizes=[]
                    for w,h in [(1600,900),(1234,567),(1000,1000),(3840,720),(640,448)]:
                        result=query("video_configure",mode="fit",window_width=w,window_height=h)
                        assert result.get("ok"),result
                        expected=max(320,int(224*w/h+0.5))
                        until=time.monotonic()+5
                        while True:
                            state=query("custom_video")
                            if state["width"]==expected:break
                            assert time.monotonic()<until,(w,h,expected,state)
                            time.sleep(0.02)
                        assert state["terrain_errors"]==0,state
                        sizes.append(dict(window=[w,h],logical_width=expected,renderer=state))
                    assert query("video_configure",mode="off").get("ok")
                    assert not query("custom_video")["enabled"]
                    (out / "resize.json").write_text(json.dumps(sizes,indent=2),encoding="utf-8")
                query("quit")
        code=process.wait(timeout=180)
    finally:
        if process.poll() is None:
            process.kill();process.wait()
print(f"exit={code}; evidence={out}")
print((out / "process.log").read_text(encoding="utf-8", errors="replace")[-2600:])
if not all((out / f"scene.{ext}").is_file() for ext in ("png", "ram", "vram")):
    raise SystemExit("Input route did not reach its capture point")
raise SystemExit(code)
