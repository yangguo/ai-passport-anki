# Desktop learning simulator

`passport_sim` is a native C11 command-line host for exercising the same bounded scheduler, snapshot codec, and import transaction used by the firmware. It does not run LVGL, draw the device screen, model buttons or USB timing, or replace firmware build and physical-device acceptance.

## Build and run

```sh
cmake -S . -B build/host -DANKI_HOST=ON
cmake --build build/host --target passport_sim
./build/host/passport_sim --state ./study.pak
```

Omit `--state ./study.pak` to keep all changes in memory for that process only. With `--state`, a PAK2 image of the cards and schedule partitions is loaded on start. A legacy PAK1 file is migrated once and written back as PAK2. Updates go to a same-directory temporary file and are renamed into place only after writing, flushing, syncing, and closing succeed. A missing file starts an empty collection. A malformed or unreadable snapshot is rejected without replacing it. A failed rename restores the in-memory flash images so the process matches the file.

Use a state path from one simulator process at a time; interprocess locking is not implemented. File `fsync` followed by rename gives atomic replacement behavior, but the simulator does not sync the parent directory. Sudden power loss can therefore lose the renamed directory entry on filesystems that require a directory sync for that guarantee.

## Load the bundled demo

```sh
python3 tools/deck.py commands decks/demo.tsv --output build/demo.commands
./build/host/passport_sim --state build/study.pak < build/demo.commands
./build/host/passport_sim --state build/study.pak
```

In the interactive session, type these commands one at a time:

```text
DECK 中文常用词
NEXT
FLIP
RATE GOOD
NEXT
QUIT
```

The first process imports all eight demo cards and exits at end of input; the second loads the saved collection. Running the import again replaces the collection and resets its review history.

## Study commands

- `HELP` lists the commands.
- `DECK <name>` selects an existing UTF-8 deck name. Names may contain spaces.
- `NEXT` displays the front of the next due card in the selected deck. If a card is already active, it displays that same front again.
- `FLIP` displays the back. A card must be flipped before it can be rated.
- `RATE AGAIN|HARD|GOOD|EASY` applies the production scheduler and saves before changing the active collection.
- `ADVANCE <minutes>` advances the offline study clock by a nonnegative number of minutes.
- `QUIT` exits.

The simulator accepts core-valid UTF-8 text. It does not check whether the device's bundled font can display every character; the desktop `tools/deck.py` validation and the firmware's font gate serve separate import paths.

## Import protocol

The line protocol is version 2: `INFO`, `BEGIN <count> <crc> <length>`, `CHUNK <offset> <hex>`, `RESUME`, `COMMIT`, `ABORT`, `DAY <day>`, `EXPORT`, `XCHUNK <offset>`, and `XRESUME`. `CHUNK` carries the canonical card records produced by `tools/deck.py`. Import replaces the collection only after the payload CRC matches and the state file is saved. Study mutations are rejected while an import transaction is open. `EXPORT` starts a read-only snapshot of the schedule records and the on-device review log. `XCHUNK` returns up to 480 bytes of that image. A grade during an export invalidates it; `XRESUME` reports the CRC, length, epoch, and how many review records the snapshot contains. Import clears the review log.

`DAY` follows firmware semantics: it rejects a day earlier than the current study day and advances to the requested day boundary when that boundary is later than the current minute. It never changes the host system clock. `ADVANCE` may move within the current study day or across days. A due card is still due after advancing; advancing time does not mark it reviewed.

## Tests

The Python integration tests build and drive the real `passport_sim` binary. The host C suite and simulator tests can be run with:

```sh
cmake -S . -B build/host -DANKI_HOST=ON -DANKI_SANITIZERS=ON
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
python3 -m pip install -r requirements-dev.txt
python3 -m pytest tests/test_deck.py tests/test_anki_sync.py tests/test_simulator.py -q
```
