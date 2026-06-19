# RocketKnightAdventuresRecomp — Development Log

Native static recompilation of **Rocket Knight Adventures** (Konami, 1993)
for the segagenesisrecomp engine. First non-Sonic bring-up and the real
exercise of the recompiler's heuristic discovery path (RKA ships with **no
community disassembly** and has no behavioral oracle).

---

## Repo topology

This is a thin game/release repo. It holds the per-game `CMakeLists.txt`
and this log; everything else lives in the engine:

- Engine: reached via a **gitignored directory junction** `./segagenesisrecomp`
  → `F:\Projects\segagenesisrecomp\segagenesisrecomp` (NOT a submodule in
  this workspace). Recreate after a fresh clone:
  `mklink /J segagenesisrecomp ..\segagenesisrecomp`
- Per-game data (engine-side, in the engine repo): `segagenesisrecomp/rka/`
  — `game.toml`, `rka_spec.c`, `rka_hybrid_table.c`, `generated/`, and the
  literal-oracle probe tools (`scan_dispatch.py`, `table_probe.py`,
  `miss_probe.py`, `producer_probe.py`, `xref_probe.py`, …).
- Native exe: **RKARecomp** (debug TCP 4390 / oracle 4391, oracle unbuilt).

The directory was renamed `RocketKnightAdventures` → `RocketKnightAdventuresRecomp`
to match the other release repos (`SonicTheHedgehogRecomp`, …). After the
rename the `build/` CMake cache holds the old source path — delete `build/`
(or reconfigure) before the next build.

## ROM identity (USA)

- File: `Rocket Knight Adventures (USA).md` (1 MB, MD5
  `CE0FDE21D2C418B24C3D904E57DF466E`), copied to `build/Release/rka.bin`.
- Header "ROCKET KNIGHT ADVENTURES", serial GM T-95046-00, region U (NTSC).
- Flat `$000000–$0FFFFF`, identity-mapped, no banks, no SRAM.
- Vectors: reset SSP `$FFFFFF00` (used directly), entry `$000208`,
  V-int `$0003D4`, H-int `$FFFFE010` (RAM trampoline, installed at runtime).
- EU ROM `Rocket Knight Adventures (E).bin` present but **deferred**
  (NTSC/PAL timing risk; bring up USA first).

## Build & run

```pwsh
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$gr    = "F:\Projects\segagenesisrecomp\segagenesisrecomp\recompiler\build\Release\GenesisRecomp.exe"

# regen RKA (engine-side) after a recompiler change
cd F:\Projects\segagenesisrecomp\segagenesisrecomp\rka
& $gr rka.bin --game game.toml --dump-functions rka_funcs.txt

# build native (delete build/ first if the dir was just renamed)
& $cmake --build F:\Projects\segagenesisrecomp\RocketKnightAdventuresRecomp\build --config Release --target RKARecomp

# run headless, single instance (debug.ini next to exe -> TCP 4390)
Get-Process RKARecomp -ErrorAction SilentlyContinue | Stop-Process -Force
cd F:\Projects\segagenesisrecomp\RocketKnightAdventuresRecomp\build\Release
Start-Process .\RKARecomp.exe -ArgumentList "rka.bin","--no-launcher","--port","4390"
python vdp_probe.py     # vdp_state / read_vram / dispatch_miss_info / screenshot
```

---

## Bring-up history

### Black screen → renders (2026-06-18)

**Symptom.** RKA booted, free-ran, played audio, but the framebuffer was
static black. The TCP probe showed VDP `display=1`, `vint=1`, a real CRAM
palette — but **VRAM completely empty**. Display-on + empty VRAM = black.

**Root cause: starved function discovery.** Pure-heuristic discovery found
only ~84 functions. RKA reaches most of its 1 MB through indexed dispatch
tables the finder never enumerated, so the graphics-load handlers were
never emitted and silently **dispatch-missed** at runtime (15 unique
misses; the floor declined them, so the routines never ran → empty VRAM).
Not a VDP-renderer gap, not display-off.

**Fix (engine `recompiler/src/function_finder.c`).** Two detectors for
indexed long-pointer dispatch tables, both keyed on the `… ; jmp/jsr (aN)`
indirect transfer and resolving the table base statically:

| Form | Pattern | Table base |
|---|---|---|
| 1 | `movea.l <tbl>(pc,Xn.W),aN ; jmp/jsr (aN)` | PC-relative, in the movea EA |
| 2 | `lea <tbl>,aN ; … ; movea.l (aN,Xn.W),aP ; jmp/jsr (aP)` | abs.l / (d,pc) lea, then An-indexed-indirect load |

`jt_enumerate_long()` walks the table with a **strong per-entry gate**: a
real ROM code pointer has a clear top byte and is `< rom_size`, so a random
longword almost never passes (top byte 0 ≈ 1/256, `< ≤4 MB rom_size` rarer
still). Over-walk therefore self-terminates at the table end — which is why
the detector runs **unconditionally for every game** (no opt-in), like the
existing bra-ladder detector. It tolerates up to 4 leading invalid slots
(unused low command indices, e.g. RKA's `$1976` cmd-0 garbage `$65D04E75`).

**Discovery progression**

| Stage | Functions found | Dispatch misses |
|---|---|---|
| Pure heuristic (baseline) | ~84 | 15 unique |
| `jump_table_autodiscovery` (pcrel16, prior session) | 314 | 15 unique → 1 |
| + Form-1 (PC-indexed movea) | 380 | 1 (`$23D6`) |
| + Form-2 (lea + indexed) | **831** (934 post-split) | **0** |

The last miss `$23D6` is a shared **null handler** (`rts`) that many table
slots point to, dispatched by `$23B6: lea $23D8,a0; movea.l (a0,d0.w),a0;
jmp (a0)` — the Form-2 idiom. Root-caused with `producer_probe.py`.

**Result.** VRAM fills with real tile graphics across the whole 64 KB;
CRAM palette present; `dispatch_miss_info` = 0; the game renders through
the boot/intro into gameplay. **User-confirmed** correct through the intro
(2026-06-18).

Engine commits (branch `feat/tier3-interp-floor`):
- `72a5fd5` recompiler: two-step long-pointer dispatch detection
- `59c6262` rka: bring-up + literal-oracle tooling
- `ac9cc2f` sandk,sonic3k: regenerate after the detector

### Sonic regression check

The detectors run for all games, so all five were regenerated and measured
(generated-function count vs prior):

| Game | Δ functions | Notes |
|---|---|---|
| Sonic 1 | 0 | byte-identical output |
| Sonic 2 | 0 | byte-identical output |
| Sonic 3 | 0 | byte-identical output |
| S&K | 0 | emission reorder only (delta 0) |
| S3K (combined) | **+56** | real, code-addr-oracle-admitted two-step targets the combined build previously missed |

No new `interior_unresolved` / `external_unresolved` sites in any game.
The code-addr oracle gates S3/S&K/S3K; the strong long-pointer gate
protects oracle-less S1/S2.

---

## Open items

1. **"Wrong stage" on the starting level (unconfirmed).** Headless
   `--no-launcher` falls through the title into RKA's **attract demo**,
   which auto-plays gameplay of various stages — likely why the captured
   frame "looked like the wrong stage." Confirm interactively (launcher
   up, press start, drive stage 1) before treating as a bug.
2. **Interior-label runtime misses (next discovery/codegen class).** Even
   with `dispatch_miss_info = 0`, the runtime silently no-ops a few
   computed jumps that land *inside* a function (Duff's-device shape):
   `$005F2E`, `$005F42`, `$00C7A6`, `$02EC1C`. These should compile to an
   in-function `switch`/`goto`, not `call_by_address` — the same class as
   the Sonic 2 CPZ fix. Static audit reports `INTERIOR_UNRESOLVED = 0`, so
   the producing path is a runtime-computed `jmp (aN)` whose index resolves
   to an interior label; investigate next.
3. **S&K / S3K native rebuild + boot_smoke** for the regenerated output
   (commit `ac9cc2f`) — additive-discovery + stale-field catch-up, pending
   E2E verification.
4. **EU ROM** support (`(E).bin`) — deferred (PAL timing).
5. **Tier-3 interpreter floor** default OFF for RKA; ON vs OFF is identical
   here (its charter declines interior-label misses), 0 floor activity.

## Discipline notes

- Disasm is ground truth; RKA's "literal oracle" is raw-ROM capstone
  disassembly via the `segagenesisrecomp/rka/*.py` probes. No behavioral
  oracle exists (`_oracle` unbuilt; `rka_hybrid_table.c` is a stub).
- Never edit generated C — fix `recompiler/src/` and regenerate.
- A dispatch miss is a discovery/codegen failure to fix at the generator,
  never papered over with stubs.
- One runtime instance at a time; `Stop-Process RKARecomp` before launch.
- ROM is the canonical stock dump (verify MD5 before recompiling); never
  patch in place. Releases never ship the ROM, `_oracle`, or `GenesisRecomp.exe`.
