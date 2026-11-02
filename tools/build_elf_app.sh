#!/usr/bin/env bash
# Build a Tulip Studio app as a loadable .elf (an EXPERIMENT, docs/APPS.md §7):
#
#   tools/build_elf_app.sh modules/mod_<name> [out-dir]
#     -> <out-dir>/<name>.elf and <name>.json   (out-dir defaults to build/elf_apps)
#
# Copy both to /files/studio/apps/ on the Tulip (the launcher's browser page
# or tools/tulip.py push <file> /studio/apps/) and restart Tulip Studio: the
# app appears on the start page and in the Studio, with no new studio.bin.
#
# The app's sources must end with TULIP_APP(mod_<name>). The build fails if
# it calls anything Tulip Studio doesn't export to apps (tools/tulip_abi.py).
# Needs ESP-IDF 5.3 in the environment (. $IDF_PATH/export.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$(cd "${1:?usage: build_elf_app.sh <app folder> [out-dir]}" && pwd)"
OUT="$(mkdir -p "${2:-$ROOT/build/elf_apps}" && cd "${2:-$ROOT/build/elf_apps}" && pwd)"
[ -f "$SRC/app.json" ] || { echo "$SRC has no app.json" >&2; exit 1; }
NAME=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["name"])' "$SRC/app.json")
grep -qs "TULIP_APP(mod_$NAME)" "$SRC"/*.c "$SRC"/*/*.c ||
    { echo "no TULIP_APP(mod_$NAME) in $SRC: add it after the tmod_t" >&2; exit 1; }

# a throw-away ESP-IDF project around the app's sources
P="$ROOT/build/elf_proj/$NAME"
mkdir -p "$P/main"
cat > "$P/CMakeLists.txt" <<CM
cmake_minimum_required(VERSION 3.16)
set(TULIP_ROOT "$ROOT")
set(EXTRA_COMPONENT_DIRS "\${TULIP_ROOT}/components/tulip_studio" "\${TULIP_ROOT}/components/tulip_hal"
    "\${TULIP_ROOT}/components/lvgl" "\${TULIP_ROOT}/components/elf_loader")
set(SDKCONFIG_DEFAULTS "\${TULIP_ROOT}/components/tulip_hal/sdkconfig.tulip;\${TULIP_ROOT}/apps/studio/sdkconfig.defaults")
include(\$ENV{IDF_PATH}/tools/cmake/project.cmake)
project($NAME)
include(elf_loader)
project_elf($NAME)
CM
cat > "$P/main/CMakeLists.txt" <<CM
file(GLOB_RECURSE srcs "$SRC/*.c")
idf_component_register(SRCS \${srcs} INCLUDE_DIRS "$SRC" REQUIRES tulip_studio lvgl json freertos)
target_compile_definitions(\${COMPONENT_LIB} PRIVATE TULIP_ELF_APP=1)
CM
cd "$P"
[ -f build/Makefile ] || idf.py -G "Unix Makefiles" set-target esp32s3 > build.log 2>&1 ||
    { tail -30 build.log; exit 1; }
make -C build elf > elf.log 2>&1 ||
    { grep -E "error:|warning:" elf.log | head -30 || tail -30 elf.log; echo "(full log: $P/elf.log)"; exit 1; }
READELF=$(dirname "$(command -v xtensa-esp32s3-elf-gcc)")/xtensa-esp32s3-elf-readelf
python3 "$ROOT/tools/tulip_abi.py" check "build/$NAME.app.elf" "$READELF"
cp "build/$NAME.app.elf" "$OUT/$NAME.elf"
cp "$SRC/app.json" "$OUT/$NAME.json"
echo "wrote $OUT/$NAME.elf ($(stat -c %s "$OUT/$NAME.elf") bytes) and $NAME.json"
