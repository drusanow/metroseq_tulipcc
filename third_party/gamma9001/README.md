# Gamma 9001 drum samples

The 155 one-shot samples Drumster9 plays, copied unchanged from AMY:
<https://github.com/shorepine/amy/tree/main/sounds/gamma9001> (commit
`17ac9f9e607641a496e9ae16d2376ad1695df881`). AMY is MIT licensed
(`LICENSE-AMY`); the samples are distributed with it as open source and free to use.

`manifest.json` is AMY's own index: bank, name, file, root note, length. The banks keep
AMY's preset numbers (TR-808 0-18, TR-909 256-272, Linn 9000 273-282, MR-12 283-286,
Tokyo Synthetics 287-310, 80s Power 311-330, Percussion 331-374, Acoustic 375-377,
Extras 378-391), so patterns and projects from the Python Drumster9 load as they are.

`tools/make_d9_samples.py` packs them into `build/gamma9001.bin` (16-bit, 22,050 Hz,
mono, as they are here), which `tools/make_full_image.sh` writes to the `samples`
partition.
