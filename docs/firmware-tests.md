# Firmware verification

## Automated checks

The host workflow builds and runs CTest with AddressSanitizer and
UndefinedBehaviorSanitizer, then runs the Python deck importer tests.
It covers core scheduling and snapshot validation, UTF-8 page boundaries,
the bounded USB command parser, and import transaction commit/abort behavior.

The firmware workflow builds with ESP-IDF 5.5.3, reports the IDF size summary,
checks the 8 MiB partition map and factory app limit, and creates a merged
factory image. A successful build establishes compile and partition-size
compatibility only.

## Device acceptance

The board-level checks in acceptance.md remain NOT RUN until a real device
is exercised. The current firmware has not been flashed as part of this
development task. In particular, CI cannot establish the display layout,
ADC key thresholds, USB serial behavior, power-loss recovery, or runtime
heap/stack margin on the ESP32-C3.

Record each device check with the firmware revision, board identity, exact
steps, and observed result. A compile or host test result must not be copied
into the device acceptance column.
