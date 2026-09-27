# RocketKnightAdventuresRecomp

Static recompilation port of Rocket Knight Adventures (Konami, 1993,
Sega Genesis) to native C, sharing the recompiler / runner engine with
[SonicTheHedgehogRecomp](https://github.com/mstan/SonicTheHedgehogRecomp)
and its siblings. The native build is a clean-room implementation (own
VDP / bus / Z80 scheduling, ymfm FM). The retired emulator oracle is no
longer in the source tree or shipped binaries.

This is the **first non-Sonic game** on the engine — and the first with
no community disassembly, so function discovery is heuristic instead of
disasm-seeded. That makes RKA the proving ground for the engine's
resilience layer (see below).

## Status (v0.1.0)

- Boots, full attract/title flow, opening cinematic.
- Stage 1 playable end to end: correct background parallax and
  per-strip scrolling, plane column streaming, full-charge rocket and
  ricochet moves, end-of-stage sequence.
- Gamepads work plug-and-play (tested with a DualSense / PS5 pad);
  keyboard and pad are active simultaneously.
- Later stages are lightly tested; a complete widescreen playthrough is pending.
- Experimental custom widescreen renderer with adaptive window fitting,
  fixed aspect ratios, and configurable enemy activation.

### The tier-3 interpreter fallback (always on for RKA)

Heuristic discovery cannot prove it found every function. Instead of a
missed dispatch silently no-op'ing (the classic "stuck in place /
softlock" failure), any call to code the recompiler didn't discover is
executed **correctly at runtime by a clean-room 68000 interpreter**, and
the executed addresses are written to `floor_coverage.toml` next to the
exe. Those leads can be validated and folded back into the static function set on the
next regeneration — so the port converges toward fully-native with
every play session, and an undiscovered code path is a log line, not a
crash.

## Known behavioral notes

- The attract-mode demo can desync from its recorded inputs (Sparkster
  may die in the demo). Timing-fidelity item, tracked engine-side.
- Raster-effect scenes (per-line vertical scroll) execute their
  self-modifying RAM handlers on the interpreter by design.

## Layout

- `segagenesisrecomp/` — the shared engine (submodule): recompiler,
  runner and clean-room video/audio backends. Generated C is ignored build output.
- `game/` — RKA's configuration, discovery input, adapter, custom renderer
  and mod settings. See [renderer notes](game/CUSTOM-VIDEO.md).
- `CMakeLists.txt` — native `RKARecomp`.
- `scripts/link-engine.*` — optional local-dev junction (`engine-local`)
  to a shared workspace engine checkout; CMake prefers it over the
  submodule when present.
- `DEVELOPMENT.md` — bring-up investigation log.

## Building (Windows)

```cmd
git clone --recursive https://github.com/mstan/RocketKnightAdventuresRecomp.git
cd RocketKnightAdventuresRecomp
copy path\to\rka.bin game\rka.bin
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target RKARecomp
```

CMake builds the current recompiler and regenerates native C whenever the ROM,
configuration/discovery inputs, generation options, or recompiler changes.

### Release packages (Windows and Linux x64)

The scripts adapted from Super Mario World build production binaries with
all three diagnostic options disabled, stage launcher assets and license
notices, and exclude ROMs and local settings. Windows packages verify the
complete DLL import closure. Linux packages use pinned, hash-checked AppImage
tools and test writable state paths. See [release instructions](RELEASING.md).

```powershell
./tools/build-windows.ps1 -Version 0.1.0
```

```sh
bash tools/build-linux.sh --version 0.1.0 --jobs 8
```

Windows output: `release-stage/*-windows-x64-v0.1.0.zip`.
Linux output: `release-linux/*-linux-x86_64-v0.1.0.AppImage`.
Both include SHA-256 checksum files. Linux v0.1.0 is built on Ubuntu 24.04;
older distributions and Steam Deck have not been validated.

## Running

This repository and its releases contain **no game data**. Bring your
own legally obtained ROM dump of Rocket Knight Adventures (USA), 1 MB:

```cmd
RKARecomp.exe rka.bin --no-launcher
```

### Adaptive widescreen

Open **Mods → Widescreen**, enable it, and leave **Aspect ratio** on
**Adaptive (fit window)**. The renderer follows the window as it resizes;
16:9, 21:9 and 32:9 presets are also available. Terrain is reconstructed from
RKA's level layout, with live tile art, palettes, scrolling and actor mappings.
The HUD counters stay centered, their gold frame extends across the top,
and pixels keep their proportions. Background parallax follows the wider
camera at the level edges while clouds retain their independent motion.

**Enemy activation** offers:

- **Expanded Spawn Range** (default): activates and retains enemies across the
  wider view. This changes encounter timing and can make more enemies active.
- **Original Spawn Range**: keeps the original camera's spawn and despawn
  behavior. Enemies can appear inside the wider view as they enter that range.

The view stops at level boundaries, and narrow windows retain at least the
original 320-pixel playfield. Adaptive has no 32:9 cap. Special presentation
modes that cannot use the level renderer retain the original centered image.
Disabling Widescreen restores the native renderer and activation rules.

Settings are saved when launching: aspect/enabled state in `settings.ini`,
enemy activation in `rka-video.ini` beside it. For scripted runs:

```cmd
RKARecomp.exe rka.bin --no-launcher --widescreen fit
RKARecomp.exe rka.bin --no-launcher --widescreen 21:9
```

Automated checks cover the castle demo, outdoor stage 1, scrolling, earlier
enemy activation and live resizing. A complete playthrough of later stages
with the custom renderer is still needed.

### Controls

| Genesis | Keyboard | Pad (Xbox / DualSense) |
| ------- | -------- | ---------------------- |
| D-pad   | Arrows   | D-pad or left stick    |
| B (jump / rocket) | X | A / Cross |
| A (attack) | Z | B / Circle |
| C (attack) | C | X / Square |
| Start   | Enter    | Start / Options        |

## License

Engine and port are PolyForm Noncommercial 1.0.0; bundled third-party
components retain their own licenses (see `THIRD-PARTY-LICENSES.md` and
`licenses/` inside each release).
