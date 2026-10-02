Rocket Knight Adventures Recomp v0.1.9
=====================================

Bring your own Rocket Knight Adventures (USA) ROM: 1,048,576 bytes,
CRC32 A6EFEC47. No game ROM is included. Other regions are unsupported.
Source: https://github.com/mstan/RocketKnightAdventuresRecomp (private for now).

Windows x64: extract the entire ZIP into a writable folder, then launch
RKARecomp.exe. Select your ROM in the launcher.

Linux x86_64: put the AppImage in a writable folder, make it executable with
chmod +x RocketKnightAdventuresRecomp-linux-x86_64-v0.1.9.AppImage, then run it.
Built on Ubuntu 24.04 (glibc 2.39); requires a compatible Linux installation
and working OpenGL. If FUSE is unavailable, run with --appimage-extract-and-run.
The launcher uses zenity, kdialog or yad for its file picker. Alternatively,
place your USA ROM beside the AppImage as rka.bin, or pass its path on the
command line (this skips the launcher). The AppImage copies launcher assets
beside itself on startup. Keep the AppImage in its own writable folder.

Mods > Widescreen: enable the mod, then choose Adaptive (fit window), 16:9,
21:9 or 32:9. Enemy activation defaults to Expanded Spawn Range; Original
Spawn Range keeps the original screen's encounter timing. The HUD stays
centered, with slim gold trim and scenery in the side margins. Special
presentation screens remain centered.

Mods > Cheats: each cheat is its own mod. Start Stage begins a new game at
stage 1-7 and then continues normally; Infinite Lives, Infinite Health and
Invincible are independent toggles.

Controls: arrows to move, X for B (jump), Z for A (attack/rocket charge),
C for C (attack/rocket charge), and Enter for Start. Controllers and keyboard
bindings are configurable in the
launcher. Settings and ROM pointers live beside the EXE/AppImage:
settings.ini, rka-video.ini and rom-RKA.cfg. Preserve these when upgrading.

This is an early release. Every stage area, reported saves from all seven
stages, adaptive resizing and both spawn modes have automated coverage.
The attract demo may diverge from its recorded inputs.

License: PolyForm Noncommercial 1.0.0 (LICENSE). Third-party components retain
their own terms; see THIRD-PARTY-LICENSES.md and licenses/. RKA and its game
assets belong to their respective owners. This project is unofficial.
