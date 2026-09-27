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

The view clamps to the stage's horizontal bounds. Actor mappings are captured
before the 80-piece SAT limit and 9-bit coordinate wrapping. A short history
matches the captured scene to the SAT actually published in VRAM; this avoids
drawing next-frame actors over the current frame's terrain. Sprite ordering,
flips, palette selection, foreground groups and the separate player SAT prefix
are retained. The two native HUD masking sentinels remain invisible.

The native window/HUD stays centered. Menus and non-layout scenes retain their
native center; surrounding planes can fill the added width. Interlace and
shadow/highlight scenes use a centered native fallback. The native 320-pixel
playfield is the minimum width. Fixed ratios and Adaptive may exceed 32:9.

Scene snapshots are presentation caches. Simulation state stays in the
original RAM and CPU; no host-owned enemy pool is introduced. Reset clears
the caches, and an unmatched SAT invalidates a cached scene after transitions.

## Spawn ranges

Original Spawn Range leaves the game's simulation hooks inert. Expanded
Spawn Range widens both placement loading and horizontal object culling.
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

## Audited USA addresses

All addresses were checked against the original ROM bytes with Capstone.
`tools/disasm_rka.py <owner-rom> <hex-start> <hex-end>` reproduces those reads.

| Address | Meaning |
| --- | --- |
| `$FFB002` | Main mode: 4 game, 7 attract |
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
| `$002358` | Capture completed RAM scene/SAT before SAT DMA |
| `$0020AE`, `$0020BE` | Horizontal culling branches after original comparisons |
| `$002128`, `$002138` | Corresponding comparisons after respawn retry |
| `$0113D8` | Native placement loader, at most three 256-pixel columns |
| `$01150C`, `$011548` | Top/bottom placement-cell helpers reused over wider range |
| `$011584`, `$0115EA` | Native spawn allocator and successful-allocation observation |
| `$0116D2` | Native remembered-placement / respawn handling |
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
- Native terrain samples match the decoded level map and the capture buffer
  does not overflow.

The release build also passes the stage-1 route with tracing disabled. Hidden
launcher checks verified the default, both choices, enabling the mod, and
saving/reloading Original Spawn Range through the actual Play action.

A longer attract run at frame 11001 also matched RAM, VRAM and native-view
pixels in the tree-climbing scene at 21:9 (map mode 5, stage index 2). Its
1,407 interpreter-floor dispatch events were identical with the native and
custom renderers; neither run reported a new unique true-miss address.

These checks are not a full playthrough. Later boss, vehicle and unusual raster
scenes still need interactive coverage. No ROM, savestate or screenshot is
committed; evidence stays in ignored build directories.
