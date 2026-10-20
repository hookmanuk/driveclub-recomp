#!/usr/bin/env bash
# VR fork: builds the runtime and the launcher and lays out a playable folder, release/vr-dev/ (package.sh without
# the DLSS libraries and the zip), with the OpenXR loader beside the game program.
# usage: packaging/stage-vr.sh [path to openxr_loader.dll]
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="/c/msys64/clang64/bin:$PATH"
LOADER="${1:-${OPENXR_LOADER:-/f/rpsc3/source/rpcs3/bin/openxr_loader.dll}}"
OUT="${STAGE_OUT:-release/vr-dev}"
DLSS_SDK="${DLSS_SDK:-/f/rpsc3/source/DLSS}"

bash build.sh
mkdir -p out/launcher
windres -I launcher launcher/launcher.rc -O coff -o out/launcher/launcher.res.o
clang++ -O2 -std=c++20 -include gpu/libcxx_compat.h -municode -mwindows -static -o out/launcher/DriveClub.exe   launcher/driveclub_launcher.cpp launcher/prepare.cpp launcher/gpu_check.cpp launcher/datx.cpp launcher/unlock.cpp out/launcher/launcher.res.o   -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -ldwmapi
# The steering wheel tools (as package.sh): console programs, SDL linked in.
sdl_static="$(pkg-config --static --libs sdl3)"
clang -O2 -o out/WheelSetup.exe tools/wheel_setup.c -static $sdl_static -mconsole
clang -O2 -o out/FfbTest.exe tools/ffb_test.c -static $sdl_static -mconsole

# The player's data (launcher.ini, data/, logs/, mods/) stays; the program files are replaced.
rm -rf "$OUT/runtime"
mkdir -p "$OUT/runtime/patches"
cp out/launcher/DriveClub.exe out/WheelSetup.exe out/FfbTest.exe "$OUT/"
cp out/DriveClubGame.exe "$OUT/runtime/"
cp patches/DriveClub.xml "$OUT/runtime/patches/"
cp "$LOADER" "$OUT/runtime/openxr_loader.dll"
# DLSS (VR DLSS row): the bridge (gpu/dlss_bridge, MIT, built with MSVC into out/dlss-bridge) and NVIDIA's library.
if [ -f out/dlss-bridge/Release/bbport_dlss.dll ] && [ -f "$DLSS_SDK/lib/Windows_x86_64/rel/nvngx_dlss.dll" ]; then
  cp out/dlss-bridge/Release/bbport_dlss.dll "$DLSS_SDK/lib/Windows_x86_64/rel/nvngx_dlss.dll" "$OUT/runtime/"
fi
echo "vr-dev" > "$OUT/version.txt"
ldd out/DriveClubGame.exe | awk '{print $3}' | { grep -iE '/msys64/|^/clang64/' || true; } | sort -u | while read -r dll; do
  cp "$dll" "$OUT/runtime/"
done
for round in 1 2; do
  for staged in "$OUT"/runtime/*.dll; do
    ldd "$staged" 2>/dev/null | awk '{print $3}' | { grep -iE '/msys64/|^/clang64/' || true; } | sort -u | while read -r dll; do
      [ -f "$OUT/runtime/$(basename "$dll")" ] || cp "$dll" "$OUT/runtime/"
    done
  done
done
for extra in SDL3.dll; do
  [ -f "out/$extra" ] && cp "out/$extra" "$OUT/runtime/" || true
done
(cd "$OUT/runtime" && find . -type f ! -name files.txt | sed 's|^\./||' | sort > files.txt)
echo "Staged $PWD/$OUT/DriveClub.exe"
