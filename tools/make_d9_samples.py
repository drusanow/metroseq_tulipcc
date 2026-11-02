#!/usr/bin/env python3
"""Pack the Gamma 9001 samples (third_party/gamma9001) into one image for
the Tulip's `samples` partition, which Drumster9 reads.

    tools/make_d9_samples.py [out.bin]      (default: build/gamma9001.bin)

Layout (little-endian; see modules/mod_drumster9/core/d9_samples.h):

    header   32 bytes  "D9SM", version, banks, samples, rate, data offset, total size
    banks    32 bytes each: key[12] label[16] first_preset u16 count u16
    samples  40 bytes each: name[28] preset u16 bank u8 root u8 offset u32 frames u32
    data     int16 PCM, mono, 22,050 Hz, each sample 4-byte aligned

Preset numbers are AMY's, as the Python Drumster9 used them.
"""
import json
import os
import struct
import sys
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "third_party", "gamma9001")
PART_SIZE = 0x400000
RATE = 22050

# AMY's bank order and first preset numbers (docs/synth.md)
BANKS = [
    ("tr808", "TR-808", 0),
    ("tr909", "TR-909", 256),
    ("linn9000", "Linn 9000", 273),
    ("mr12", "MR-12", 283),
    ("synthetics", "Tokyo Syn", 287),
    ("power", "80s Power", 311),
    ("percussion", "Percussion", 331),
    ("acoustic", "Acoustic", 375),
    ("extras", "Extras", 378),
]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "gamma9001.bin")
    man = json.load(open(os.path.join(SRC, "manifest.json")))
    by_bank = {k: [] for k, _, _ in BANKS}
    for e in man:
        by_bank[e["bank"]].append(e)
    samples = []
    bank_rows = []
    for bi, (key, label, first) in enumerate(BANKS):
        rows = by_bank[key]
        bank_rows.append((key, label, first, len(rows)))
        for i, e in enumerate(rows):
            w = wave.open(os.path.join(SRC, e["file"]))
            assert (w.getframerate(), w.getsampwidth(), w.getnchannels()) == (RATE, 2, 1), e["file"]
            pcm = w.readframes(w.getnframes())
            samples.append((e["name"], first + i, bi, int(e.get("root", 60)), pcm))
    hdr = 32
    off = hdr + 32 * len(bank_rows) + 40 * len(samples)
    off = (off + 3) & ~3
    data_off = off
    table = b""
    blobs = []
    for name, preset, bi, root, pcm in samples:
        nm = name.encode("ascii", "replace")[:27]
        table += struct.pack("<28sHBBII", nm, preset, bi, root & 255, off, len(pcm) // 2)
        blobs.append(pcm + b"\0" * ((-len(pcm)) % 4))
        off += len(blobs[-1])
    total = off
    if total > PART_SIZE:
        sys.exit("samples are %d bytes, the partition holds %d" % (total, PART_SIZE))
    body = struct.pack("<4sHHHHIII8x", b"D9SM", 1, len(bank_rows), len(samples), RATE, 0,
                       data_off, total)
    for key, label, first, n in bank_rows:
        body += struct.pack("<12s16sHH", key.encode(), label.encode(), first, n)
    body += table
    body += b"\0" * (data_off - len(body))
    body += b"".join(blobs)
    assert len(body) == total
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, "wb").write(body)
    print("wrote %s: %d samples in %d banks, %d bytes (%d KB free in the partition)" %
          (out, len(samples), len(bank_rows), total, (PART_SIZE - total) // 1024))


if __name__ == "__main__":
    main()
