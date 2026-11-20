"""Qualify disk-state loading with the cartridge runner and a private ROM."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def run(runner, rom, folder, script):
    route = folder / "route.input"
    route.write_text(script + "WAIT 1\nEXIT 0\n", encoding="ascii")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("GENESIS", "SDL_", "LNG_", "RNET_"))}
    for key in ("APPDATA", "LOCALAPPDATA", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
                "XDG_STATE_HOME", "XDG_CACHE_HOME"):
        env[key] = str(folder)
    env.update(GENESIS_USER_DIR=str(folder), GENESIS_REPLAY_RECORD="0",
               SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software", SDL_AUDIODRIVER="dummy",
               SDL_JOYSTICK_RAWINPUT="0", SDL_JOYSTICK_HIDAPI="0", SDL_JOYSTICK_WGI="0",
               SDL_DIRECTINPUT_ENABLED="0", SDL_XINPUT_ENABLED="0")
    result = subprocess.run(
        [runner, rom, "--no-launcher", "--widescreen", "fit", "--input-script", route,
         "--measure-runtime", "6000", "--runtime-uncapped"],
        cwd=folder, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    if result.returncode or "[input_script] EXIT 0" not in result.stdout:
        raise RuntimeError(result.stdout)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runner", type=Path)
    parser.add_argument("rom", type=Path)
    args = parser.parse_args()
    runner, rom = args.runner.resolve(strict=True), args.rom.resolve(strict=True)
    startup = Path(__file__).with_name("stage1-walk.input").read_text().split("HOLD RIGHT", 1)[0]
    movement = "WAIT 16\nHOLD RIGHT\nWAIT 60\nPRESS A 3\nWAIT 104\nRELEASE RIGHT\nWAIT 1\n"

    with tempfile.TemporaryDirectory(prefix="rka-state-test-") as scratch:
        folder = Path(scratch)
        run(runner, rom, folder, startup + "WAIT 1\nSAVE_STATE stage.state\n" + movement +
            "SCREENSHOT fresh.png\nDUMP_RAM fresh.ram\nDUMP_VRAM fresh.vram\n")
        log = run(runner, rom, folder, "WAIT 30\nLOAD_STATE stage.state\n" + movement +
                  "SCREENSHOT loaded.png\nDUMP_RAM loaded.ram\nDUMP_VRAM loaded.vram\n")
        if "[LOAD] loaded " not in log:
            raise RuntimeError(log)
        for extension in ("png", "ram", "vram"):
            fresh = (folder / f"fresh.{extension}").read_bytes()
            loaded = (folder / f"loaded.{extension}").read_bytes()
            if fresh != loaded:
                raise RuntimeError(f"Stage 1 continuation differs after a cold load ({extension})")

        # An early snapshot must still run the remainder of reset with its IRQ mask set.
        run(runner, rom, folder, "WAIT 2\nSAVE_STATE boot.state\n")
        log = run(runner, rom, folder, "WAIT 2\nLOAD_STATE boot.state\n"
                  "WAIT_RAM16 FFB002 0001\nWAIT_RAM16 FFB004 0001\n")
        if "[LOAD] loaded " not in log:
            raise RuntimeError(log)

    print("Stage 1 cold-state continuation and early startup state passed.")


if __name__ == "__main__":
    main()
