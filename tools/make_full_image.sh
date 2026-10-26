#!/usr/bin/env bash
# Build the launcher and the bundled apps, and merge everything into one
# image for a first flash:   esptool.py --chip esp32s3 write_flash 0x0 tulip-full.bin
#
#   0x0        bootloader (app rollback enabled)
#   0x8000     partition table (components/tulip_hal/partitions_tulip.csv)
#   0x49000    otadata (blank: boot the launcher)
#   0x50000    launcher (factory)
#   0x350000   metroseq, preloaded in app slot ota_0. The launcher copies it to
#              /files/apps/metroseq.bin on first boot, so it appears at once.
#
# Needs ESP-IDF 5.3 in the environment (. $IDF_PATH/export.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/tulip-full.bin}"
export IDF_COMPONENT_MANAGER=0

(cd "$ROOT/apps/metroseq" && idf.py build)
(cd "$ROOT/launcher" && idf.py build)

cd "$ROOT/launcher/build"
python -m esptool --chip esp32s3 merge_bin -o "$OUT" @flash_args \
    0x350000 "$ROOT/apps/metroseq/build/metroseq.bin"
echo "wrote $OUT"
