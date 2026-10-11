# Host Learning Simulator Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a native C11 terminal simulator that exercises the production scheduler, snapshot codec, and import transaction on a desktop.

**Architecture:** Add a `passport_sim` host executable wired to `passport_anki`, `pa_protocol`, and `pa_import`. The command loop keeps separate live/staging collections and a review session; snapshot writes use a same-directory temporary file and only swap live state after write, flush, file sync, close, and rename succeed. Python integration tests drive the actual executable through stdin/stdout and restart it against persisted files.

**Tech Stack:** C11, CMake, POSIX file APIs, Python 3, pytest.

### Task 1: Define simulator behavior with integration tests

**Files:**
- Create: `tests/test_simulator.py`

**Steps:**
1. Add subprocess tests for protocol import, UTF-8 deck selection, card review gating (`RATE` before `FLIP`), answer reveal, successful rating, quit, and restart recovery.
2. Add raw-byte input tests for embedded NUL, internal CR, bounded overlong-line recovery, and accepted CRLF endings.
3. Exercise all four ratings through persisted due times: Again 1 minute, Hard 6, Good 10, Easy 5760.
4. Add tests for malformed snapshot preservation, atomic persistence failure preserving the active card, `DAY` and `ADVANCE`, and no-due-card output.
5. Build the host target and run the tests to confirm they fail because the simulator target does not exist.

### Task 2: Add the host executable and command loop

**Files:**
- Create: `tools/passport_sim.c`
- Modify: `CMakeLists.txt`

**Steps:**
1. Link the executable to `passport_anki`, `main/pa_protocol.c`, and `main/pa_import.c`.
2. Implement HELP, DECK, NEXT, FLIP, RATE, ADVANCE, QUIT plus the existing INFO/BEGIN/CARD/COMMIT/ABORT/DAY import protocol.
3. Preserve transactional semantics: stage imports with `pa_import_*`; grade/day/time updates use candidate copies; persist before updating `live`.
4. Run the focused simulator tests and the existing CTest suite.

### Task 3: Add snapshot loading and atomic persistence

**Files:**
- Modify: `tools/passport_sim.c`

**Steps:**
1. Decode snapshots with `pa_decode`; reject malformed files without rewriting them.
2. Encode with `pa_encode`, write to a same-directory temporary path, check every write/flush/sync/close/rename operation, and remove temporary files on failure.
3. Support no-state in-memory operation and `--state PATH` restart recovery.
4. Re-run failure-path integration tests, including a blocked temporary path.

### Task 4: Document and run host validation

**Files:**
- Modify: `README.md`
- Create: `docs/simulator.md`
- Modify: `.github/workflows/host-tests.yml`
- Modify: `requirements-dev.txt` only if needed

**Steps:**
1. Document build/run commands, supported commands, snapshot behavior, and the boundary: this does not emulate LVGL or the physical device.
2. Include simulator integration tests in host CI.
3. Build with ASan and UBSan, run all C tests and all Python tests, and record command output and limitations for the parent review.
