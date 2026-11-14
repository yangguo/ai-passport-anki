# CI and Sleep Checkpoint Implementation Plan

**Goal:** Install complete host test dependencies, build firmware on code changes,
and prevent automatic deep sleep after a failed checkpoint.

**Architecture:** Keep the existing host/firmware workflows and idle state machine.
Gate deep sleep on the existing checkpoint result. On failure, restore the screen
and reset the inactivity timer so failures do not retry every second.

**Tech Stack:** GitHub Actions, ESP-IDF 5.5.3, C11, CTest with ASan/UBSan, pytest.

## Tasks

1. Install both `requirements.txt` and `requirements-dev.txt` in
   `.github/workflows/host-tests.yml`. The preceding baseline run reproduced
   six missing-zstandard failures; installing runtime dependencies resolved them.
2. Add main push and pull-request triggers to `.github/workflows/firmware.yml`,
   matching the host workflow's documentation exclusions. Retain manual dispatch.
3. In `main/main.c`, check `pa_store_checkpoint()` before automatic deep sleep.
   On failure, log the error, restore configured brightness, reset idle time,
   render, and return without calling the power API.
4. Document failure/retry behavior and distinguish host checks from hardware
   acceptance. Run host sanitizer tests, Python tests, workflow lint, and firmware
   build if ESP-IDF is available. Do not flash hardware.

## Validation limits

Host tests do not execute `main.c`, LVGL, or device power transitions. The sleep
failure path needs hardware fault injection to establish device acceptance.
Review the checkpoint guard and recovery state explicitly in the source diff.

## Local results (2026-11-14)

- Implemented dependency installation, automatic firmware triggers, and the
  checkpoint failure guard. Updated README test setup and sleep documentation.
- CMake sanitizer build and CTest: 9/9 passed.
- pytest with both requirements files installed: 59 passed.
- actionlint 1.7.7: passed for both workflows; shellcheck disabled because it is
  not installed locally.
- One-off host fault injection compiled the actual `check_idle_sleep` function
  with hardware stubs under ASan/UBSan. The HEAD baseline failed the assertion
  that no sleep occurs after checkpoint failure. The working tree passed failure
  cancellation, brightness restoration, full-cycle retry delay, successful retry,
  and import inhibition. Harness: `build/host/verify_idle_sleep.py` (local build
  artifact; not part of the permanent test suite).
- At local validation time, no ESP-IDF installation was found and Docker daemon
  was unavailable. Cloud validation subsequently completed as recorded below.
- No flashing or hardware acceptance performed.

## GitHub Actions results (2026-11-14)

Source revision: `0564fc77a6c3226e9d3b3b6e042c802dbaef85b8`.

- [Firmware SUCCESS](https://github.com/yangguo/ai-passport-anki/actions/runs/43575913123):
  ESP-IDF 5.5.3 build, size/layout checks, merged image and artifact upload passed.
- [Host tests SUCCESS](https://github.com/yangguo/ai-passport-anki/actions/runs/43575934179):
  installation of runtime/test dependencies, sanitizer C tests and Python tests passed.
- Downloaded artifacts to `build/ci-43575913123/`; verified source revision,
  image checksums, padding, factory tail and merged app bytes with
  `tools/check_firmware_layout.py`.
- `app.bin`: 2,691,072 bytes; `full.bin`: 2,756,608 bytes. Factory tail:
  5,632,000 bytes, sufficient for the 12,000-card windows. Linked DRAM:
  121,188 bytes used, 200,108 bytes remaining (runtime heap unmeasured).
- SHA-256 `app.bin`:
  `ab132b40f3b4722d38315d275044a883a4b6cce88fade2e2f7f119cd1b6ea704`.
- SHA-256 `full.bin`:
  `735e3a211045431162227b5fa7d06f389dc2c7a34f57e3e27d3e94865e7c9253`.
- Device acceptance remains NOT RUN; no firmware was flashed.
