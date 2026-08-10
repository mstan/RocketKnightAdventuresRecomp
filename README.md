# RocketKnightAdventuresRecomp

Static recompilation port of Rocket Knight Adventures (Konami, 1993,
Sega Genesis) to native C, sharing the recompiler / runner engine with
[SonicTheHedgehogRecomp](https://github.com/mstan/SonicTheHedgehogRecomp)
and its siblings. The native build is a clean-room implementation (own
VDP / bus / Z80 scheduling, ymfm FM, permissively-licensed throughout);
the AGPL clownmdemu core is a development-only conformance oracle and is
never part of the shipped binary.

This is the **first non-Sonic game** on the engine — and the first with
no community disassembly, so function discovery is heuristic instead of
disasm-seeded. That makes RKA the proving ground for the engine's
resilience layer (see below).

## Status (v0.0.1)

- Boots, full attract/title flow, opening cinematic.
- Stage 1 playable end to end: correct background parallax and
  per-strip scrolling, plane column streaming, full-charge rocket and
  ricochet moves, end-of-stage sequence.
- Gamepads work plug-and-play (tested with a DualSense / PS5 pad);
  keyboard and pad are active simultaneously.
- Later stages are lightly tested — expect rough edges, not softlocks
  (see the fallback below).

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
  runner, clean-room video/audio backends, and per-game configuration under
  `segagenesisrecomp/rka/`. Generated C is ignored build output.
- `CMakeLists.txt` — native `RKARecomp` + dev-only `RKARecomp_oracle`.
- `scripts/link-engine.*` — optional local-dev junction (`engine-local`)
  to a shared workspace engine checkout; CMake prefers it over the
  submodule when present.
- `DEVELOPMENT.md` — bring-up investigation log.

## Building (Windows)

```cmd
git clone --recursive https://github.com/mstan/RocketKnightAdventuresRecomp.git
cd RocketKnightAdventuresRecomp
copy path\to\rka.bin segagenesisrecomp\rka\rka.bin
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target RKARecomp
```

`Skipping submodule 'clownmdemu-core'` in the clone output is expected and
correct. That core is AGPL and dev-only; nothing you build needs it, and CMake
skips the dev-only `_oracle` targets automatically when it is absent.

CMake builds the current recompiler and regenerates native C whenever the ROM,
configuration/discovery inputs, generation options, or recompiler changes.

## Running

This repository and its releases contain **no game data**. Bring your
own legally obtained ROM dump of Rocket Knight Adventures (USA), 1 MB:

```cmd
RKARecomp.exe rka.bin --no-launcher
```

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
components are permissively licensed (see `THIRD-PARTY-LICENSES.md` in
the engine / release zip). Dev-only oracle builds link AGPL code and are
never distributed.
