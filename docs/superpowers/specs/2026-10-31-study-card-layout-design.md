# Study Card Layout — Design

**Goal:** Match the official anki-jlpt-decks card style and AnkiDroid's card
rendering on the 240×320 study screen: left-aligned body text, and JLPT back
lines that do not waste a line on a single pitch mark or part-of-speech glyph.

**Status:** Approved (scheme A) by the user on 2026-10-31.

## Evidence

- Official deck (ankiweb 832276382) template: front is the headword; back leads
  with the reading and pitch, then part of speech, definition, notes, then one
  example sentence in furigana form with its gloss. Its CSS sets
  `.card { text-align: left }`.
- AnkiDroid renders the same HTML with the deck CSS and `text-align: left`, so
  long definitions wrap from the left margin and short segments sit at the
  margin, not in the middle of the panel.
- Current firmware ([main/main.c](../../../main/main.c) `sync_study_chrome`)
  forces `LV_TEXT_ALIGN_CENTER` on the body while a card is shown. The JLPT
  converter emits `VocabPitch` and `VocabPoS` as separate lines, so a back such
  as `①\n名\n茄子…` renders as a lone `①` centered on its own line.
- Converted data confirms it: card 1 back is
  `①\n名\n茄子\n茄子\n麻（マー） 婆（ボー） 茄子（なす）\n麻婆茄子` — the plus
  field duplicates the definition, costing another line.

## Design

### 1. Body alignment (firmware)

`sync_study_chrome()` sets the study body label to `LV_TEXT_ALIGN_LEFT` for
every screen. Menu and status screens are already left-aligned; only the card
branch differed, so this is a one-line change plus removing the now-unused
`body_align` temporary. Title, footer, and grade chips keep their existing
alignment. Pagination, font, line height, and page count are unchanged.

### 2. Compact JLPT back (converter)

`tools/deck.py` builds the JLPT back from one helper shared by the APKG reader
(`_jlpt_sides`) and the font-subset collector (`_study_body_text`):

- **Head line:** `VocabPitch` and `VocabPoS` and `VocabDefSC` joined with single
  spaces, e.g. `① 名 茄子`. Whitespace inside each field is collapsed so the
  head stays one logical line before width wrapping. Empty fields are skipped,
  so a card with no pitch starts with `名 …` or just the definition.
- **Notes:** `VocabPlus` only when it differs from `VocabDefSC` (the real deck
  repeats the definition in this field).
- **Example:** `SentFurigana1`, then `SentDefSC1`, each on its own line.

The 511-byte fitting rule is unchanged in effect: if the back is too long, drop
the notes line first, then the sentence gloss, then fail with `DeckError`. The
fitting now indexes the four resulting lines instead of the six raw fields.

This changes the imported card text, so the deck must be re-uploaded for the
new layout to appear. Card ids, order, and decks are unchanged.

## Non-goals

- No change to the page/footer chrome, grade chips, or pagination algorithm.
- No moving the pitch mark onto the front (the device front already shows the
  reading; that would give away the answer).
- No change to the `XCHUNK` export or the USB chunk size.
- No HTML rendering on the device; it remains plain text.

## Testing

- `tests/test_deck.py`: JLPT conversion asserts the merged head line, duplicate
  notes removal, and the existing drop-plus/drop-gloss byte fitting.
- `tests/test_anki_sync.py`: collection and AnkiConnect JLPT pulls keep passing
  with the new line layout.
- Host C tests unchanged (`tests/test_layout.c` covers wrapping, not alignment).
- Device: rebuild/flash, open a JLPT card, confirm left margin alignment and
  that the back no longer shows a one-glyph line.
