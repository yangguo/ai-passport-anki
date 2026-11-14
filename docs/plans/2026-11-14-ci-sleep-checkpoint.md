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
- Full ESP-IDF build not run: no local ESP-IDF installation was found and Docker
  daemon is unavailable. GitHub Actions has not been dispatched for these edits.
- No flashing or hardware acceptance performed.
