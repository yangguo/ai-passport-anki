#!/usr/bin/env bash
set -euo pipefail

. "${IDF_PATH:-/opt/esp/idf}/export.sh"
idf.py build
idf.py size
python3 tools/esp_image_pad.py build/ai-passport-anki.bin
python3 tools/check_firmware_layout.py build/ai-passport-anki.bin
idf.py merge-bin -o full.bin
mkdir -p build/artifacts
cp build/ai-passport-anki.bin build/artifacts/app.bin
cp build/bootloader/bootloader.bin build/artifacts/bootloader.bin
cp build/partition_table/partition-table.bin build/artifacts/partition-table.bin
cp build/full.bin build/artifacts/full.bin
idf.py size > build/artifacts/size.txt
git -c safe.directory="$PWD" rev-parse HEAD > build/artifacts/source-revision.txt
