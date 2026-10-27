#!/usr/bin/env bash
# Build the launcher and the bundled apps, and merge everything into one
# image for a first flash:   esptool.py --chip esp32s3 write_flash 0x0 tulip-full.bin
#
#   0x0        bootloader (app rollback enabled)
#   0x8000     partition table (components/tulip_hal/partitions_tulip.csv)
#   0x49000    otadata (blank: boot the launcher)
#   0x50000    launcher (factory)
#   0x250000   studio (metroseq + GinTonix + Sloe + Wavetron together), slot ota_0
#   0x450000   metroseq on its own, slot ota_1
#   0x650000   gintonix on its own, slot ota_2
#   0x850000   sloe on its own, slot ota_3
#   0xA50000   wavetron on its own, slot ota_4
#              (ota_5 is free: the first app you push starts from there)
#              The launcher copies each one to /files/apps/<name>.bin on
#              first boot, so they appear at once.
#   0xF50000   /files: not in the image - flashing never touches it
#
# Needs ESP-IDF 5.3 in the environment (. $IDF_PATH/export.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/tulip-full.bin}"
export IDF_COMPONENT_MANAGER=0

for app in studio metroseq gintonix sloe wavetron; do
    (cd "$ROOT/apps/$app" && idf.py build)
done
(cd "$ROOT/launcher" && idf.py build)

cd "$ROOT/launcher/build"
python -m esptool --chip esp32s3 merge_bin -o "$OUT" @flash_args \
    0x250000 "$ROOT/apps/studio/build/studio.bin" \
    0x450000 "$ROOT/apps/metroseq/build/metroseq.bin" \
    0x650000 "$ROOT/apps/gintonix/build/gintonix.bin" \
    0x850000 "$ROOT/apps/sloe/build/sloe.bin" \
    0xA50000 "$ROOT/apps/wavetron/build/wavetron.bin"
echo "wrote $OUT"
