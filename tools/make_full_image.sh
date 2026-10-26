#!/usr/bin/env bash
# Build the launcher and the bundled apps, and merge everything into one
# image for a first flash:   esptool.py --chip esp32s3 write_flash 0x0 tulip-full.bin
#
#   0x0        bootloader (app rollback enabled)
#   0x8000     partition table (components/tulip_hal/partitions_tulip.csv)
#   0x49000    otadata (blank: boot the launcher)
#   0x50000    launcher (factory)
#   0x350000   studio (metroseq + GinTonix + Sloe together), app slot ota_0
#   0x650000   metroseq on its own, app slot ota_1
#   0x950000   gintonix on its own, app slot ota_2
#              The launcher copies each one to /files/apps/<name>.bin on
#              first boot, so they appear at once.
#
# Needs ESP-IDF 5.3 in the environment (. $IDF_PATH/export.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/tulip-full.bin}"
export IDF_COMPONENT_MANAGER=0

for app in studio metroseq gintonix; do
    (cd "$ROOT/apps/$app" && idf.py build)
done
(cd "$ROOT/launcher" && idf.py build)

cd "$ROOT/launcher/build"
python -m esptool --chip esp32s3 merge_bin -o "$OUT" @flash_args \
    0x350000 "$ROOT/apps/studio/build/studio.bin" \
    0x650000 "$ROOT/apps/metroseq/build/metroseq.bin" \
    0x950000 "$ROOT/apps/gintonix/build/gintonix.bin"
echo "wrote $OUT"
