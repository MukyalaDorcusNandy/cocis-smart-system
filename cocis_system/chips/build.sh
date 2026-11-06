#!/bin/sh
# Compiles the MFRC522 Wokwi custom chip to WebAssembly.
# Needs wasi-sdk (default ~/.wokwi/wasi-sdk, or set WASI_SDK).
# The .wasm is committed, so you only need this after changing mfrc522.chip.c.
set -e
cd "$(dirname "$0")"
WASI_SDK="${WASI_SDK:-$HOME/.wokwi/wasi-sdk}"
"$WASI_SDK/bin/clang" --target=wasm32-unknown-wasi \
    --sysroot "$WASI_SDK/share/wasi-sysroot" \
    -nostartfiles -Wl,--import-memory -Wl,--export-table -Wl,--no-entry \
    -O2 -Wall -Wextra -Wno-unused-function -Werror \
    -o mfrc522.chip.wasm mfrc522.chip.c
echo "built chips/mfrc522.chip.wasm"
