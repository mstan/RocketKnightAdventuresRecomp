# Releasing RKA

Keep the repository private until the owner requests otherwise. Build from a
clean, committed checkout with initialized submodules and an owner-supplied
USA ROM at `game/rka.bin` (CRC32 `A6EFEC47`). Never commit or distribute it.

## Windows x64

Requires Visual Studio 2022 C++ tools, CMake and Python 3. The CMake executable
and Python interpreter can be selected explicitly to avoid MSYS PATH shims.

```powershell
./tools/build-windows.ps1 -Version 0.1.2 -Python C:/path/to/python.exe
```

This configures Release with tracing and reverse-debug instrumentation off
and the MSVC runtime linked statically. The resulting ZIP has `RKARecomp.exe`,
its SDL2/runtime DLL closure, launcher assets, build metadata and notices.
Only allowlisted inputs enter the fresh stage; ROMs copied by the development
build are excluded. `tools/make_release.ps1` can package an existing build,
but rejects stale commit metadata, wrong versions and development flags.

## Linux x86_64

On Ubuntu 24.04 install `build-essential cmake pkg-config libsdl2-dev
libgl1-mesa-dev curl python3 patchelf imagemagick`. Build in Linux or WSL:

```sh
bash tools/build-linux.sh --version 0.1.2 --jobs 8
```

`--build DIR`, `--out DIR` and `--no-package` are available. The script verifies
pinned linuxdeploy/appimagetool downloads (including the runtime embedded in
that verified appimagetool AppImage), bundles the game's dependency closure,
copies distribution copyright notices, and runs the AppImage layout gate.
The build distribution determines the minimum glibc/libstdc++ requirements;
this release targets Ubuntu 24.04 or compatible installations.

The pinned Genesis host anchors assets and settings beside `$APPIMAGE`.
`tools/release/AppRun` refreshes release-owned `assets/` there, seeds missing
settings, discovers adjacent raw cartridge images, and preserves user state.
The built-in Widescreen mod is compiled into RKA; it requires no external mod
catalog. Run `--appimage-extract-and-run` when FUSE is unavailable.

## Validation and publication

1. Run packaged binaries from fresh folders with an owner-supplied ROM. Check
   launcher assets, native rendering, widescreen and both spawn modes.
2. `tools/verify_release.py` compares the actual packages across platforms and
   checks native RAM/VRAM parity in Original mode, HUD framing and the opening
   background behavior. It needs Pillow and writes local, ignored evidence.
3. Confirm the Git checkout and submodules are clean; merge the release branch
   into `main`, push, and tag that commit `v0.1.2`. Reconfigure/repackage so the
   embedded `build-info.json` identifies that exact commit.
4. Publish the ZIP, AppImage and `SHA256SUMS.txt` to the private GitHub release.
   Verify uploaded hashes, the tag target, `origin/main`, and private visibility.

Full renderer diagnostics and live resize coverage use the trace-enabled
`tools/verify_video.py` workflow documented in `game/CUSTOM-VIDEO.md`.

## Script provenance

Adapted from SuperMarioWorldRecomp commit `55628231b1fe683fe96062ce5a7f322ab05ed8b3`:
`tools/make_release.ps1`,
`tools/build-linux.sh`, `tools/test_appimage_layout.sh`, and its pinned
`snesrecomp/tools/release/RuntimeDllClosure.ps1`. RKA removes SMW-only co-op,
Lua, SDL3 and external-mod staging. The DLL helper additionally treats Visual
C++ redistributables as application dependencies even on a developer machine
where those DLLs happen to be installed in System32.

Font copyright notices are extracted from each shipped font. Full OFL 1.1
terms were copied from `google/fonts/ofl/lato/OFL.txt`; CC BY-SA 4.0 terms
were copied from `https://creativecommons.org/licenses/by-sa/4.0/legalcode.txt`.
