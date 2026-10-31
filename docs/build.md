# Build and CI

## Toolchain

Firmware is pinned to ESP-IDF 5.5.3, LVGL 9.5.0, and esp_lvgl_port 2.9.0.
The target is ESP32-C3 with 8 MiB flash. Use the repository's dependencies.lock
and do not update component versions implicitly during a release build.

With ESP-IDF 5.5.3 installed and exported:

    idf.py set-target esp32c3
    idf.py build
    bash tools/validate_firmware.sh

The validation script pads the app image so its length modulo 64 KiB is 4 KiB,
checks the stock single-factory partition map, verifies that the padded image
fits the factory partition and leaves a tail large enough for the 12,000-card
windows, merges bootloader, partition table, and app into build/full.bin, and
collects size and revision records in build/artifacts/. It does not flash a
device.

The container build used by GitHub Actions can be reproduced with Docker:

    docker run --rm -v "$PWD:/project" -w /project espressif/idf:v5.5.3 \
      bash tools/validate_firmware.sh

## Host tests

The portable core, snapshot codec, UTF-8 page planner, import transaction,
USB line parser, Cover Arts return gate, and image padder run without ESP-IDF:

    cmake -S . -B build/host -DANKI_HOST=ON -DANKI_SANITIZERS=ON
    cmake --build build/host --parallel
    ctest --test-dir build/host --output-on-failure
    python3 -m pip install -r requirements-dev.txt
    python3 -m pytest -q tests/test_deck.py tests/test_anki_sync.py tests/test_simulator.py tests/test_image_pad.py

GitHub Actions runs these host tests and a separate ESP-IDF container build.
The firmware job publishes app.bin, bootloader.bin, partition-table.bin,
full.bin, an IDF size report, and the source revision as workflow artifacts.

## Install

One app image. The partition table is the stock FoloToy single-factory map
(`nvs` 24 KiB, `phy_init`, `factory` from `0x10000` through the end of the
8 MiB flash). Card text and review state are not partitions. They live after
the verified app image, inside the running app partition.

Cover Arts (https://calm.yishan.app/) accepts this table. Install `app.bin`,
or let the installer extract the app from a merged image. Do not flash
`full.bin` at `0x0` on a device that already runs the Launcher: that rewrites
the bootloader and partition table and erases launcher slots.

`full.bin` at `0x0` is the standalone path, only for a device you intend to
overwrite. The same binary then uses the factory tail. The build rejects an
image that would leave less than `0x460000 + 0xD0000` bytes after itself, so
a standalone device still has the historical 12,000-card windows.

The image is padded with an unloaded address-0 segment until
`image_length % 65536 == 4096`. Cover Arts sizes a slot as
`align64k(image_length) + 64 KiB`. That residue leaves a 60 KiB erased gap
before the sidecar. Reinstalling the play rewrites the slot, including the
gap. NVS is the 24 KiB stock partition and is shared with the Launcher; decks
are not stored there. A legacy PAK1 blob of about 37 KiB may no longer fit.
Migration stays best-effort and never calls `nvs_flash_erase`.

Inside a slot the structural cap is 256 cards, with 4096 bytes of card text
per slot and one review-log sector (508 grades). A JLPT-sized card is on the
order of 180 bytes, so a slot holds a couple dozen of those, not the
10,641-card sample. A 511-byte field leaves room for only a handful. The host reads `cap` and `room` from `INFO` and refuses to `BEGIN` when the
deck has more cards than `cap` or the payload has more bytes than `room`.
`BEGIN` still rejects a payload that does not fit the slot. Standalone factory
tail keeps `cap=12000`. The image pad runs as a CMake `POST_BUILD` on `app`,
and `tools/validate_firmware.sh` checks the padded bytes again after
`merge-bin` copies them into `build/artifacts`.

On the deck list, holding UP for the BSP long-press time (500 ms, not 1.5 s)
calls `launcher_contract_return_to_factory`. There is no separate cover page.
Other screens ignore UP long. If factory is missing, already running, or fails
verification, the play stays open. Reset or power-cycle is the universal
return when the slot was installed as a one-shot OTA. The firmware does not
call `esp_ota_mark_app_valid_cancel_rollback`.

## Flash map

| Partition | Offset | Size |
| --- | ---: | ---: |
| NVS | 0x9000 | 0x6000 |
| PHY init | 0xf000 | 0x1000 |
| Factory app | 0x10000 | 0x7F0000 |

The map ends at 0x800000. Do not reuse an app-only offset from another
project, and do not flash this `full.bin` over a Cover Arts Launcher.
