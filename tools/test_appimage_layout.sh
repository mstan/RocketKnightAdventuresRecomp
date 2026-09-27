#!/usr/bin/env bash
# Adapted from SuperMarioWorldRecomp: state survives updates and relocation.
set -euo pipefail
APPDIR=$(realpath "${1:?usage: test_appimage_layout.sh AppDir}")
WORK=$(mktemp -d)
trap 'chmod -R u+w "$APPDIR" "$WORK"; rm -rf "$WORK"' EXIT
chmod -R a-w "$APPDIR"
before=$(find "$APPDIR" -type f -exec sha256sum {} + | sort)
run() {
    mkdir -p "$1"
    # No ROM: the process should report an error, not open an interactive UI.
    set +e
    (cd /tmp; APPIMAGE="$1/RKA.AppImage" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        GENESIS_RUN_DONE=1 timeout 15 "$APPDIR/AppRun" "$WORK/missing-rom.bin" --no-launcher --benchmark 1 > "$1/run.txt" 2>&1)
    rc=$?
    set -e
    [ "$rc" -ne 124 ] && [ "$rc" -ne 126 ] && [ "$rc" -ne 127 ] || {
        cat "$1/run.txt"; echo "Failed to start packaged application ($rc)" >&2; exit 1; }
}
state="$WORK/folder with spaces"
run "$state"
test -s "$state/settings.ini"
test -s "$state/assets/fonts/LatoLatin-Regular.ttf"
printf '\n# user marker\n' >> "$state/settings.ini"
printf 'spawn_range=original\n' > "$state/rka-video.ini"
settings=$(sha256sum "$state/settings.ini" "$state/rka-video.ini")
printf 'README is not a ROM\n' > "$state/README.md"
run "$state"
test "$settings" = "$(sha256sum "$state/settings.ini" "$state/rka-video.ini")"
test ! -e "$state/rom-RKA.cfg"
# A fake cart header tests discovery without redistributing game data.
truncate -s 1048576 "$state/pretend.bin"
printf SEGA | dd of="$state/pretend.bin" bs=1 seek=256 conv=notrunc status=none
run "$state"
test "$(cat "$state/rom-RKA.cfg")" = "$state/pretend.bin"
cp "$state/pretend.bin" "$state/other.bin"
printf '%s\n' "$state/other.bin" > "$state/rom-RKA.cfg"
run "$state"
test "$(cat "$state/rom-RKA.cfg")" = "$state/other.bin"
run "$WORK/moved"
test -s "$WORK/moved/settings.ini"
test "$before" = "$(find "$APPDIR" -type f -exec sha256sum {} + | sort)"
echo 'PASS: AppImage state, asset refresh, ROM discovery, relocation and read-only payload'
