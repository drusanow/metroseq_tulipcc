#!/usr/bin/env bash
# Build the launcher and Tulip Studio (every app in modules/), and merge everything into one
# image for a first flash:   esptool.py --chip esp32s3 write_flash 0x0 tulip-full.bin
#
#   0x0        bootloader (app rollback enabled)
#   0x8000     partition table (components/tulip_hal/partitions_tulip.csv)
#   0x49000    otadata (blank: boot the launcher)
#   0x50000    launcher (factory)
#   0x250000   studio.bin, slot ota_0: every app, run on its own or together
#              in the Studio (the launcher says which). The launcher copies
#              it to /files/apps/studio.bin on first boot.
#   0x3D0000   slots ota_1..ota_5: free for other uploaded .bin apps
#   0xB50000   Drumster9's Gamma 9001 samples (tools/make_d9_samples.py)
#   0xF50000   /files: not in the image - flashing never touches it
#
# Needs ESP-IDF 5.3 in the environment (. $IDF_PATH/export.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/tulip-full.bin}"
export IDF_COMPONENT_MANAGER=0

(cd "$ROOT/apps/studio" && idf.py build)
(cd "$ROOT/launcher" && idf.py build)
python3 "$ROOT/tools/make_d9_samples.py" "$ROOT/build/gamma9001.bin"

# it must fit its 1.5 MB slot
sz=$(stat -c %s "$ROOT/apps/studio/build/studio.bin")
if [ "$sz" -gt $((0x180000)) ]; then
    echo "studio.bin is $sz bytes: more than an app slot (1.5 MB)" >&2
    exit 1
fi

cd "$ROOT/launcher/build"
python -m esptool --chip esp32s3 merge_bin -o "$OUT" @flash_args \
    0x250000 "$ROOT/apps/studio/build/studio.bin" \
    0xB50000 "$ROOT/build/gamma9001.bin"
echo "wrote $OUT"
