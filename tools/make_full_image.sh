#!/usr/bin/env bash
# Build the launcher and the bundled apps, and merge everything into one
# image for a first flash:   esptool.py --chip esp32s3 write_flash 0x0 tulip-full.bin
#
#   0x0        bootloader (app rollback enabled)
#   0x8000     partition table (components/tulip_hal/partitions_tulip.csv)
#   0x49000    otadata (blank: boot the launcher)
#   0x50000    launcher (factory)
#   0x250000   studio (metroseq, GinTonix, Sloe, Wavetron, Drumster9 together), slot ota_0
#   0x3D0000   metroseq on its own, slot ota_1
#   0x550000   gintonix on its own, slot ota_2
#   0x6D0000   sloe on its own, slot ota_3
#   0x850000   wavetron on its own, slot ota_4
#   0x9D0000   drumster9 on its own, slot ota_5
#              The launcher copies each one to /files/apps/<name>.bin on
#              first boot, so they appear at once.
#   0xB50000   Drumster9's Gamma 9001 samples (tools/make_d9_samples.py)
#   0xF50000   /files: not in the image - flashing never touches it
#
# Needs ESP-IDF 5.3 in the environment (. $IDF_PATH/export.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/tulip-full.bin}"
export IDF_COMPONENT_MANAGER=0

APPS="studio metroseq gintonix sloe wavetron drumster9"
for app in $APPS; do
    (cd "$ROOT/apps/$app" && idf.py build)
done
(cd "$ROOT/launcher" && idf.py build)
python3 "$ROOT/tools/make_d9_samples.py" "$ROOT/build/gamma9001.bin"

# every app must fit its 1.5 MB slot
for app in $APPS; do
    sz=$(stat -c %s "$ROOT/apps/$app/build/$app.bin")
    if [ "$sz" -gt $((0x180000)) ]; then
        echo "$app.bin is $sz bytes: more than an app slot (1.5 MB)" >&2
        exit 1
    fi
done

cd "$ROOT/launcher/build"
python -m esptool --chip esp32s3 merge_bin -o "$OUT" @flash_args \
    0x250000 "$ROOT/apps/studio/build/studio.bin" \
    0x3D0000 "$ROOT/apps/metroseq/build/metroseq.bin" \
    0x550000 "$ROOT/apps/gintonix/build/gintonix.bin" \
    0x6D0000 "$ROOT/apps/sloe/build/sloe.bin" \
    0x850000 "$ROOT/apps/wavetron/build/wavetron.bin" \
    0x9D0000 "$ROOT/apps/drumster9/build/drumster9.bin" \
    0xB50000 "$ROOT/build/gamma9001.bin"
echo "wrote $OUT"
