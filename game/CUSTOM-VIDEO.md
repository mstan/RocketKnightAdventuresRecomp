# RKA custom renderer

The consuming RKA repository owns this implementation. It uses the shared
`GameVideo` interface and the scene capture / live VDP compositing approach
used by the Sonic renderers. All layout formats, addresses and instruction
hooks below are specific to **Rocket Knight Adventures (USA)**, CRC32
`A6EFEC47`. Generated C and ROM contents are not source files to edit.

## Rendering

`rka_video.c` decodes foreground terrain from the full level layout, so the
extra view does not repeat the VDP's partially streamed 512-pixel name table.
Each layout byte selects a 32-pixel block containing 4×4 tile attributes.
Patterns and palettes come from the live VDP at each rendered scanline.
Plane B uses its native name table and scrolling. Horizontal whole-plane,
per-strip and per-line scrolling, raster palette/scroll writes and vertical
whole-plane / two-cell scrolling use the current VDP state. Extra two-cell
scroll columns extend the nearest native edge column.

Background parallax uses the wider camera's center, including at the stage
edges where it stops moving before the original camera. The per-strip scroll
hooks capture the native camera term separately from autonomous motion, then
re-evaluate only that term for presentation. Matching native scroll values
against a short history keeps the metadata aligned with the live VDP. Clouds
and other independently moving strips retain their native phase. No guest
camera, scroll table or game state is changed.

The stage-1 castle is a sprite object, not part of plane B. Object type `$07`
computes world X as `7/8 * (camera + 160)`; type `$3B` derives its flying detail
from that anchor plus a timed phase. At clamped edges, the renderer replaces
their embedded native-camera term with the wider camera's term, retaining
fractional coordinates until the final pixel rounding. The clamped camera
does not inherit the moving native camera's fractional part. Their autonomous
animation and all guest state remain untouched. Once the view scrolls freely,
the native sprite projection is retained.

Scenes fall into three classes. Tiled scenes (maps 2-8, and map 9 boss arenas
whose layout masks at `$B1E4` are real) draw terrain from the layout. The
stage-1 boss room (area 11) retains the previous area's layout metadata after
switching to map 9. It uses the live plane as a planar scene, keeps the exact
native boss chamber centered, and shows only sky in its side margins so the
chamber cannot repeat at the name-table wrap. Other planar scenes (map 1
vehicles, shooters, set pieces and cutscenes, and the ending's map 9) never
stream: their planes are loaded whole, so the live name tables
wrap correctly at the plane width and the view is always centered on the
native one. Scenes with no block dictionary at `$B1F6` (the Continue screen,
stage cards) keep their centered native presentation.

The tiled view clamps to the current room, the range `$3EAE` clamps the
following camera to (`$B39C`..`$B39E` plus 320), widened to include the live
camera during scripted pans. A room narrower than the view is centered on its
midpoint, even at a stage end, so locked boss rooms keep the exact native view
in the middle. Beyond a room the layout holds unrelated areas (the previous
corridor, a castle exterior beside an interior); there the renderer repeats
the room's outermost whole 32-pixel block column of terrain. Plane B and all
actors keep their real positions. A plane whose unseen name-table columns are
all blank (cutscenes author only 320 pixels) repeats its native 320 pixels.
The stage-1 tank bridge is a 320-pixel locked room with a curved fence.
Repeating only a sloped edge block makes the rail sawtooth, so its margins
reflect the complete authored bridge span at each room edge. The centered
native view stays byte-identical.

Actor mappings are captured
before the 80-piece SAT limit and 9-bit coordinate wrapping. A short history
matches the captured scene to the SAT actually published in VRAM; this avoids
drawing next-frame actors over the current frame's terrain. Sprite ordering,
flips, palette selection, foreground groups and the separate player SAT prefix
are retained. The two native HUD masking sentinels remain invisible.
Expanded-range actors can remain active outside the original viewport. Their
nine-bit hardware SAT X can wrap by 512 pixels into that viewport, leaving
partial carts, enemies and other sprite fragments in the native play area.
After capturing each unwrapped mapping, the renderer skips hardware pieces
whose world-space X is outside the SAT range. The wide sprites stay visible at
their true positions.

The native window/HUD counters stay centered. Its undecorated 32-pixel border
repeats across the added margins for all 32 HUD rows, covering water and other
world art that used to show through below the cornice. Counters and hearts do
not repeat. When the native and wide cameras align, the original 320-pixel image is copied
back over the reconstruction to preserve raster and boss effects.
The flying city (stage index 23) is an endless set piece with a 256-pixel
background map. Beyond 512 pixels of view width, its repeated city art fades
into a vertically smoothed average of its own colors outside the native view.
The 320-pixel play area is copied back exactly, and moving actors remain sharp.
The Continue screen has no block dictionary; it is a presentation screen
filled with the VDP backdrop around its centered native image.
Planes and sprites are composited per pixel exactly as `genesis_vdp.c` does:
first opaque sprite wins, S-hi > A-hi > B-hi > S-lo > A-lo > B-lo > backdrop,
and in shadow/highlight mode (the stage 7 corridors) palette-3 colours 14/15
are highlight/shadow operators. Interlace and H32 keep a centered native
fallback. The native 320-pixel playfield is the minimum width. Fixed ratios
and Adaptive may exceed 32:9. No per-stage strip captures remain; the
boss-room, bridge and stage-23 background treatments are described above.

Scene snapshots are presentation caches. Simulation state stays in the
original RAM and CPU; no host-owned enemy pool is introduced. Reset clears
the caches, and an unmatched SAT invalidates a cached scene after transitions.

## Spawn ranges

Original Spawn Range leaves the game's simulation hooks inert. Expanded
Spawn Range widens placement loading for tiled scenes and horizontal object
culling for both tiled and planar scenes, but only across the room the view
shows: margins beyond a locked room never wake the neighbouring area.
Scripted generators (vehicle rides, shooter waves, set pieces) create objects
at fixed native screen edges. `tools/audit_edge_spawns.py` finds every
constant or camera-relative X store into a new slot that falls in the native
spawn band (screen X -96..-1 or 320..415; X=0 only at audited left entries)
and emits the hook table `s_edge_spawns`. After each X store the hook moves an
off-view object to the corresponding wider edge. The scripts retain their
cadence, allocator and despawn logic.

The stage-1 fire corridor places its launcher and fire source at native X=304,
just inside that audit band. Four additional ROM-verified X-store hooks shift
the linked launcher parts together to the wider right edge; their child fire
objects inherit the shifted source position.

The original cell loaders, allocator, fixed slots, remembered placement bits
and respawn routines still own the objects. The native object pool remains
finite, so very wide views can exhaust it; there is no CPU speed multiplier.

At width 320 the original placement loader runs unchanged. Turning the mod
off immediately restores the native loader/culling rules; existing actors
leave through the game's normal despawn processing. Selecting a smaller
range does not rewind encounters that have already occurred.

The default is Expanded Spawn Range. `rka_options.c` composes the engine's
Widescreen mod provider and persists `spawn_range=expanded|original` in
`rka-video.ini` beside `settings.ini`. Because activation affects simulation,
the game adds the choice to the shared session configuration image. Host
adoption does not overwrite local preferences. Network sessions were not part
of this renderer validation.

## Playtest cheats

A second built-in Mods package, Playtest Cheats (`rka_cheats.c`), reproduces
the published Game Genie codes as instruction hooks, since recompiled code
cannot be byte-patched. Start stage (1-7) replaces only the new-game opening
area stored at `$0A8E`; the game then advances through its own next-area
table at `$8322`, so later stages, Continue and Game Over are unchanged. The
entry areas are 44 (opening), 4, 40, 45, 48, 49 and 47. The published "Start
on Level 3" code writes area 58, whose successor is area 1, so it restarts
stage 1; stage 3 uses 40, the successor of stage 2's last area. Infinite lives
pre-increments the BCD count before the `sbcd` at `$1189E`, infinite health
pre-adds the damage before `sub.w d1,$40(a1)` at `$F54A`, and invincibility
clears Z before the damage `bne.w` at `$F45E`/`$F46A`. The settings persist
in `rka-video.ini` and are part of the netplay session image.

## Coverage and instrumentation

`tools/coverage_sweep.py` boots every area index through `$FFFB12` with the
invincibility/health/lives RAM codes and a rotating input pattern, collecting
interpreter-floor targets, executed PCs and the spawn ring. With
`--force-interp` it records whole-program executed PCs. `tools/fold_floor_coverage.py`
accepts a floor target only when the oracle proves its straight-line run and
it decodes cleanly, and writes `rka_floor_sweep.discovery.toml` (late_extra
roots) plus the merged oracle `rka_executed_pcs_sweep.txt`. Three iterations
cut floor-executed targets across all 54 areas from 1777 to 16, with the RAM
after 3600 scripted frames byte-identical in every area. The engine
interpreter also runs game hooks, so hooks no longer depend on coverage.

Owner stage-1 saves added thirteen ROM-validated floor entries around the fire
corridor and boss handlers. Saved native floor capsules supplied their
executed-PC oracle, including three later targets reached during a scripted
boss encounter.

The always-on spawn ring (`rka_spawns` debug command) records every object
birth: `$116B0` callers directly, other slots by an object-table diff at each
snapshot, attributed through the engine write hook to the creating object.

## Audited USA addresses

All addresses were checked against the original ROM bytes with Capstone.
`tools/disasm_rka.py <owner-rom> <hex-start> <hex-end>` reproduces those reads.

| Address | Meaning |
| --- | --- |
| `$FFB002` | Main mode: 4 game, 7 attract |
| `$FFB39C`, `$FFB39E` | Room camera X bounds, applied by the follow camera at `$3EAE` |
| `$FFFB12` | Area index; `$8322` maps each area to its successor |
| `$000A8E` | New game: `move.w #$2C,$FB12` (opening area) |
| `$0116B0` | Object slot clear used by most creations (spawn ring) |
| `$F45E`, `$F46A`, `$F54A`, `$1189E` | Damage branches, health subtraction, lives decrement (cheats) |
| `$FFB1D0` | Map dimension mode, 2–8 use the tiled level layout |
| `$FFB1D2`, `$FFB1D6` | Camera X/Y, integer words of 16.16 values |
| `$FFB1C8`, `$FFB1CC` | Actor camera, including shake and the SAT bias |
| `$FFB1E4`, `$FFB1E6` | Layout coordinate masks (OR 31 for pixel coordinates) |
| `$FFB1E8` | Maximum camera X; add 320 for level width |
| `$FFB1EC` | Byte-layout row stride |
| `$FFB1F6`, `$FFB1FA` | Block dictionary and byte-layout pointers, ROM or RAM |
| `$FFFB12` | Stage index |
| `$002D7E`, `$0032B6` | Native terrain streamers proving the 4×4 block format |
| `$001FB0` | Begin actor mapping capture |
| `$0021AE` | Mapping header, before hardware piece limit and coordinate mask |
| `$0021B4` | Post-compare branch that skips wrapped hardware pieces for distant world actors |
| `$002358` | Capture completed RAM scene/SAT before SAT DMA |
| `$012C62` | Type `$07` castle: camera-plus-160 minus one eighth, in 16.16 coordinates |
| `$012CD8` | Type `$3B` flying detail: castle anchor plus independent timed phase |
| `$0069BE`, `$006AD2`, `$006BDA`, `$007964`, `$00803A` | Per-strip background total before NEG/SWAP: D4 = autonomous phase + (D5 camera >> D3 shift) |
| `$FFEC00`, `$FFEC40`, `$FFB3B8` | Per-strip scroll values, autonomous phases and background effect selector |
| `$0020AE`, `$0020BE` | Horizontal culling branches after original comparisons |
| `$002128`, `$002138` | Corresponding comparisons after respawn retry |
| `$0113D8` | Native placement loader, at most three 256-pixel columns |
| `$01150C`, `$011548` | Top/bottom placement-cell helpers reused over wider range |
| `$011584`, `$0115EA` | Native spawn allocator and successful-allocation observation |
| `$0116D2` | Native remembered-placement / respawn handling |
| `$01378C`, `$013814`, `$013898`, `$0138C0` | Stage-1 fire launcher/source X stores at camera+$130, before Y placement |
| `$02F8FE` | Water-ride type `$12` controller, immediately after spawning type `$11` at native screen X `$160` |
| `$016CA4` | Rail-ride type `$97` state-3 JMP-table target, generated as a callable stream to expose its creation hook |
| `$016E04` | Rail-ride type `$96` creation, after the native left/right X table store |
| `$FFB880` | Per-cell remembered placement bits |
| `$FFC000` | Player object, rendered through reserved SAT entries |
| `$FFC100`–`$FFE000` | General objects, 128 bytes each |
| `$FFB054` | Boundary of the foreground object group |
| `$0BAFC6` | Relative general-object sprite mapping table |
| `$FFF780` | 640-byte RAM SAT, ten reserved entries before general actors |

`game.toml` emits the instruction hooks. Hook callbacks return without
mutating the guest when the mod is disabled; the capture hooks are also
read-only in Original Spawn Range mode. Expanded loading preserves CPU
registers around calls to the game's original helpers.

## Reproducible validation

Build with `GEN_ENABLE_TRACE=ON`, `SONIC_REVERSE_DEBUG=OFF`, then run:

```text
python tools/verify_video.py --exe build-adaptive/Release/RKARecomp.exe --rom game/rka.bin --out build-adaptive/verification
```

The output directory must be new. Python needs Pillow. The script creates
isolated executable/settings directories, uses hidden SDL output, supplies
normal controller inputs, captures screenshots and RAM/VRAM, and queries the
free-running debug server. It asserts:

- Original Spawn Range keeps all 64 KiB of RAM and VRAM identical to native
  rendering in the castle demo at frame 1801 and outdoor walk at frame 4804.
- The original world view and centered HUD are pixel identical in those
  scenes, at 16:9 and 32:9 respectively.
- Expanded mode activates more stage-1 actors (15 versus 10 in the recorded
  run), using ten successful native allocations, without dispatch misses.
- Adaptive resizes through 1600×900, 1234×567, 1000×1000, 3840×720 and 640×448,
  producing widths 398, 488, 320, 1195 and 320, then disables successfully.
- The HUD's full border fills the wider margins while its centered counters
  remain pixel identical.
- Idle versus walking 80 frames from stage start keeps the wider view pinned
  at zero while the native camera moves from 2 to 235. Background strips and
  castle, flying detail, and clear cloud/mountain image regions agree at the
  same capture frame. The castle also stays anchored at 48:9 (1195 pixels),
  and RAM/VRAM are identical to the narrower Original Spawn Range captures.
- Native terrain samples match the decoded level map and the capture buffer
  does not overflow.

The release build also passes the stage-1 route with tracing disabled. Hidden
launcher checks verified the default, both choices, enabling the mod, and
saving/reloading Original Spawn Range through the actual Play action.

A longer attract run at frame 11001 also matched RAM, VRAM and native-view
pixels in the tree-climbing scene at 21:9 (map mode 5, stage index 2). Its
1,407 interpreter-floor dispatch events were identical with the native and
custom renderers; neither run reported a new unique true-miss address.

The supplied stage-1 save 4 and stage-2 save 6 were also replayed in native,
Original Spawn Range and Expanded Spawn Range. At frame 120, the stage-1 water
enemy's screen X was 39 in Original and 117 in Expanded; the stage-2 rail
hazard's X was 202 and 241. Original Spawn Range RAM and VRAM matched native
in both scenes. All 15 supplied saves were captured in native and 16:9; the
recovered stage-4 save 4 reached CLEAR BONUS in both modes after 600 frames.
These captures are not a full interactive playthrough. No ROM, savestate or
screenshot is committed; evidence stays in ignored build directories.
