# README screenshots

These are unedited game-frame captures from the published Windows x64
**v0.1.13** build (source commit `0606d763147d2a12756e0b88d03739b307c48cdf`).
They use **Adaptive (fit window)** in a 796 × 448 window and **Expanded Spawn
Range**; the renderer's captured frame is 398 × 224 pixels.

- `adaptive-forest.png`: stage 1, captured with `tools/capture_rka.py` and
  `tests/stage1-walk.input`, 100 frames after that route.
- `adaptive-cavern.png`: crystal caves, resumed from a tester-provided save.
- `adaptive-bridge.png`: tank boss on the bridge, resumed from a tester save.
- `adaptive-minecart.png`: minecart section, resumed from a tester save.

The later-stage captures use `tools/capture_rka.py`, load the private save,
resume play if paused, and wait 12 frames before capturing. ROMs and save
files are not stored with these documentation assets. The screenshots retain
the game's original artwork, which belongs to Konami.
