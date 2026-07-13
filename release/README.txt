RocketKnightAdventuresRecomp — native static recompilation of
Rocket Knight Adventures (Konami, 1993)
==============================================================================

A native Windows port produced by statically recompiling the Sega Genesis
68000 code to C. No emulator core for the shipped game: recompiled CPU code,
clean-room VDP/bus/scheduler, ymfm FM synthesis, clean-room SN76489 PSG. A
clean-room 68000 interpreter acts as an always-on safety net for any code
path the static recompiler has not discovered yet (logged to
floor_coverage.txt).

BRING YOUR OWN ROM
------------------
This package contains NO game data. Place your own legally obtained ROM dump
of Rocket Knight Adventures (USA) — a 1,048,576-byte .bin/.md file — next to
RKARecomp.exe and run:

    RKARecomp.exe rka.bin --no-launcher

CONTROLS
--------
    Genesis        Keyboard    Gamepad (Xbox / DualSense)
    D-pad          Arrows      D-pad or left stick
    B (jump)       X           A / Cross
    A (attack)     Z           B / Circle
    C (attack)     C           X / Square
    Start          Enter       Start / Options

Gamepads are plug-and-play (SDL2 game controller support; DualSense
tested). Keyboard and pad are both active at all times.

STATUS
------
v0.0.1 — Stage 1 playable end to end (attract, cinematic, parallax
scrolling, full-charge/ricochet, end-of-stage). Later stages lightly
tested; undiscovered code paths run on the interpreter fallback rather
than crashing.

LICENSES
--------
PolyForm Noncommercial 1.0.0 (this port + engine). Bundled components:
see THIRD-PARTY-LICENSES.md (ymfm BSD-3, superzazu z80 MIT, clowncommon
ISC, SDL2 zlib).
