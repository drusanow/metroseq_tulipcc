# elf_loader (vendored)

Espressif's ELF loader, from
https://github.com/espressif/esp-iot-solution/tree/master/components/elf_loader
(commit 35b831cfa84d132d66cb88f7b6f131632ed4f726, version 2.0.0), Apache-2.0 (`license.txt`).

Vendored because the Tulip build runs with the component manager off
(`IDF_COMPONENT_MANAGER=0`). One change: `CMakeLists.txt` defines the version macros
itself instead of calling `cu_pkg_define_version()` from
`espressif/cmake_utilities`, which isn't here.
The tests, examples and docs images were left out.

Tulip Studio uses it to load apps built as `.elf` files from
`/files/studio/apps` - see docs/APPS.md.
