#!/usr/bin/env bash
# RKA adaptation of SuperMarioWorldRecomp/tools/build-linux.sh.
# Prerequisites: cmake, make, g++, pkg-config, libsdl2-dev, libgl1-mesa-dev,
# curl, python3, patchelf, ImageMagick. Place the USA ROM at game/rka.bin.
# Usage: bash tools/build-linux.sh --version 0.1.0 --jobs 8 [--no-package]
# Release builds always disable tracing and reverse-debug instrumentation.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
VERSION=0.1.0
JOBS=8
OUT="$REPO/release-linux"
BUILD="$REPO/build-linux-prod"
PACKAGE=1
while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift 2;;
        --jobs) JOBS="$2"; shift 2;;
        --out) OUT="$2"; shift 2;;
        --build) BUILD="$2"; shift 2;;
        --no-package) PACKAGE=0; shift;;
        -h|--help) sed -n '2,6p' "$0"; exit 0;;
        *) echo "Unknown argument: $1" >&2; exit 2;;
    esac
done
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.]+)?$ ]] || exit 2
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || exit 2
test "$(uname -m)" = x86_64
cmake -S "$REPO" -B "$BUILD" -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release \
    -DGENESIS_RECOMP_ROOT="$REPO/segagenesisrecomp" -DRKA_BUILD_VERSION="$VERSION" \
    -DSONIC_REVERSE_DEBUG=OFF -DGEN_ENABLE_TRACE=OFF -DGEN_DEV_TRACE=OFF
cmake --build "$BUILD" --target RKARecomp -j"$JOBS"
[ "$PACKAGE" = 1 ] || exit 0
BIN="$BUILD/RKARecomp"
file "$BIN" | grep -q 'ELF 64-bit.*x86-64'
mkdir -p "$OUT" "$BUILD/appimage-tools"
OUT=$(realpath "$OUT")
WORK=$(mktemp -d)
trap 'chmod -R u+w "$WORK"; rm -rf "$WORK"' EXIT
APPDIR="$WORK/AppDir"
mkdir -p "$APPDIR"

# Same versioned, SHA-verified tools as SMW; no floating download URLs.
fetch() {
    local url="$1" sha="$2" dest="$3"
    if [ ! -f "$dest" ] || [ "$(sha256sum "$dest" | cut -d' ' -f1)" != "$sha" ]; then
        curl -fL --retry 3 "$url" -o "$dest.tmp"
        printf '%s  %s\n' "$sha" "$dest.tmp" | sha256sum -c -
        mv "$dest.tmp" "$dest"
    fi
    chmod +x "$dest"
}
LINUXDEPLOY="$BUILD/appimage-tools/linuxdeploy-x86_64.AppImage"
APPIMAGETOOL="$BUILD/appimage-tools/appimagetool-x86_64.AppImage"
fetch https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage \
    c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d "$LINUXDEPLOY"
fetch https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
    ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 "$APPIMAGETOOL"
# Reuse the runtime embedded in the verified appimagetool image. Otherwise
# appimagetool downloads a floating "continuous" runtime during every build.
RUNTIME="$BUILD/appimage-tools/runtime-x86_64"
OFFSET=$("$APPIMAGETOOL" --appimage-offset)
[[ "$OFFSET" =~ ^[1-9][0-9]*$ ]] || exit 1
head -c "$OFFSET" "$APPIMAGETOOL" > "$RUNTIME"
convert "$REPO/boxart.tga" -resize 240x240 -background transparent \
    -gravity center -extent 256x256 "$WORK/rka.png"
cat > "$WORK/rka.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Rocket Knight Adventures Recomp
Exec=RKARecomp
Icon=rka
Categories=Game;
Terminal=false
EOF
"$LINUXDEPLOY" --appimage-extract-and-run --appdir "$APPDIR" --executable "$BIN" \
    --desktop-file "$WORK/rka.desktop" --icon-file "$WORK/rka.png"
python3 "$REPO/tools/release/stage_payload.py" --build "$BUILD" \
    --dest "$APPDIR/usr/bin" --version "$VERSION"
# Preserve distribution notices for every bundled Linux library, together with
# package versions/source names and instructions to obtain corresponding source.
python3 "$REPO/tools/release/linux_notices.py" "$APPDIR"
rm -f "$APPDIR/AppRun"
cp "$REPO/tools/release/AppRun" "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"
bash "$REPO/tools/test_appimage_layout.sh" "$APPDIR"
if find "$APPDIR" -type f | grep -Ei '\.(bin|gen|smd|srm|ram|vram|log|map)$'; then
    echo 'Forbidden file in AppImage' >&2; exit 1
fi
APP="$OUT/RocketKnightAdventuresRecomp-linux-x86_64-v$VERSION.AppImage"
ARCH=x86_64 "$APPIMAGETOOL" --appimage-extract-and-run --runtime-file "$RUNTIME" "$APPDIR" "$APP"
chmod +x "$APP"
(cd "$OUT"; sha256sum "$(basename "$APP")" > "$(basename "$APP").sha256")
echo "Built $APP"
cat "$APP.sha256"
