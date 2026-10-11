# Build and CI

## Toolchain

Firmware is pinned to ESP-IDF 5.5.3, LVGL 9.5.0, and esp_lvgl_port 2.9.0.
The target is ESP32-C3 with 8 MiB flash. Use the repository's dependencies.lock
and do not update component versions implicitly during a release build.

With ESP-IDF 5.5.3 installed and exported:

    idf.py set-target esp32c3
    idf.py build
    bash tools/validate_firmware.sh

The validation script checks the fixed partition map, verifies that the app
fits the factory partition, merges bootloader, partition table, and app into
build/full.bin, and collects size and revision records in build/artifacts/.
It does not flash a device.

The container build used by GitHub Actions can be reproduced with Docker:

    docker run --rm -v "$PWD:/project" -w /project espressif/idf:v5.5.3 \
      bash tools/validate_firmware.sh

## Host tests

The portable core, snapshot codec, UTF-8 page planner, import transaction, and
USB line parser run without ESP-IDF:

    cmake -S . -B build/host -DANKI_HOST=ON -DANKI_SANITIZERS=ON
    cmake --build build/host --parallel
    ctest --test-dir build/host --output-on-failure
    python3 -m pip install -r requirements-dev.txt
    python3 -m pytest -q tests/test_deck.py

GitHub Actions runs these host tests and a separate ESP-IDF container build.
The firmware job publishes app.bin, bootloader.bin, partition-table.bin,
full.bin, an IDF size report, and the source revision as workflow artifacts.
The full.bin image is for explicit factory installation after checking the
device's current partition map.

## Flash map

| Partition | Offset | Size |
| --- | ---: | ---: |
| NVS | 0x9000 | 0x40000 |
| PHY init | 0x49000 | 0x1000 |
| Factory app | 0x50000 | 0x7b0000 |

The map ends at 0x800000. These offsets are specific to this standalone
firmware. Do not reuse app-only offsets from another AI Passport project.
