# Native `collection.anki21b` import support — design

- Date: 2026-10-31
- Status: approved by user (Section 1 scope/behavior, Section 2 errors/dependency/docs/tests)
- Motivating input: `n3.apkg` at repo root (eggrolls-JLPT10k-v3.5 N3 subset)

## 1. Background: what `n3.apkg` actually contains

Inspection (verified with throwaway scripts, repo untouched):

- Real data lives in the **zstd-compressed `collection.anki21b` member** (428 KB → 1.5 MB SQLite, schema 18: `notetypes`/`fields`/`decks` tables). Single notetype `eggrolls-JLPT10k-v3.5` with `VocabKanji` / `VocabFurigana` / `VocabDefSC` fields — the JLPT shape the tool already converts.
- The alongside `collection.anki2` (51 KB) is a **1-card stub** ("Please update to the latest Anki version…") — must never be picked when `anki21b` is present.
- `media` member is empty (9 bytes of zstd framing → 0 bytes). Media stays ignored, as today.
- **1820 notes / 1820 cards** in 3 nested decks: `NEW-JLPT ▸ eggrolls-JLPT10k-v3.5 ▸ 3-N3 ▸ {1-高频 (381) / 2-中频 (707) / 3-低频 (732)}`. Hierarchy separator inside the DB is `\x1F`.
- Trial conversion with leaf deck names: **1820/1820 pass**, max front 52 B / back 466 B (under the 511 B cap), font coverage OK. Only blockers in current code are the deliberate `anki21b` rejection and the `\x1F` control character in deck names.

## 2. Scope and behavior

- `tools/deck.py load_deck` (hence `inspect` / `commands` / `upload`) reads the zstd-compressed `collection.anki21b` member from `.apkg` files, decompresses it, and reuses the **existing schema-18 reader and unchanged Basic/JLPT conversion** (`card_from_note`, `_fit_jlpt_back`, size/font validation).
- **Member preference** when an `.apkg` holds several collections: `collection.anki21b` > `collection.anki21` > `collection.anki2`. This fixes the stub-member trap (`n3.apkg`'s 1-card `anki2` next to the real `anki21b`) and preserves the existing `anki21`-over-`anki2` preference.
- **Deck-name normalization:** split each deck name on Anki's `\x1F` separator (and legacy `::`), take the last segment, strip whitespace. `n3.apkg` yields `1-高频` / `2-中频` / `3-低频`. Documented tradeoff: two branches sharing a leaf name merge into one device deck (accepted; consistent with the flat 64-deck device model).
- `anki-pull --collection` accepts a raw `collection.anki21b` file the same way (decompress whole file, then read-only open).
- **Non-goals:** no firmware, protocol, scheduling, or device-format changes — the device receives the same flat payload. No FSRS, template engine, media extraction, Cloze/custom models, or AnkiWeb on device. Sync-back flow (`--map`, device→Anki id mapping) is untouched.

## 3. Dependency and implementation notes

- Add `zstandard` to `requirements.txt` (runtime tool dependency; verified already importable in this env, but undeclared).
- Keep the existing temp-file + read-only-open pattern: decompress member/file bytes with a **64 MiB output cap** (`max_output_size`), write to temp dir, `sqlite3.connect(...?mode=ro)`. Compressed input stays under the existing 16 MiB `APKG_DB_MAX_BYTES` cap. 64 MiB is generous for ≤12000 text-only cards (~12 MB worst case).
- One shared helper for "zstd bytes → temp SQLite path" used by both the `.apkg`-member path and the `--collection` file path.

## 4. Error behavior (strictness preserved)

- Missing `zstandard` import or corrupt/truncated zstd payload → `DeckError` naming the member/file, with an actionable hint (`pip install -r requirements.txt`); never a partial import.
- Decompressed bytes that are not SQLite → `DeckError` (same family as today's "unsupported or invalid legacy APKG collection").
- Cloze/custom models, over-limit fields, multi-cards-per-note → the **existing** errors, unchanged.
- `collection.anki21b` stays rejected in exactly one place: the AnkiWeb-sync download body (`docs/ankiweb-sync.md` — that is a sync envelope, not an apkg member).

## 5. Docs and tests

- `docs/user-guide.md`: `anki21b` moves from "not converted" to supported (apkg member + `--collection` file); add `pip install -r requirements.txt` before inspect/upload and the direct `upload n3.apkg --port …` shape.
- `docs/design.md` import section + `docs/status.md` scope line: drop the "anki21b rejected" statements.
- `tests/test_deck.py`: new tests — zstd member preferred over stub `anki2`; leaf naming of `\x1F`/`::` hierarchies; corrupt zstd rejected; oversize decompressed output rejected. The existing `test_apkg_rejects_compressed…` test is **replaced** (behavior intentionally changes).
- `tools/anki_sync.py` `.anki21b` guard + its test: `--collection collection.anki21b` reads instead of refusing.

## 6. Acceptance for this spec

- `python3 tools/deck.py inspect n3.apkg` reports 1820 cards / 3 decks; `upload n3.apkg --port …` produces the same chunked `BEGIN/CHUNK/COMMIT` flow as TSV input and ends only on `OK COMMIT`.
- `pytest tests/test_deck.py tests/test_anki_sync.py` green; host `ctest` suite unaffected (no firmware changes).
