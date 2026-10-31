# AI Passport font assets

`pa_font_16.c` defines the LVGL font symbol `pa_font_16`: Noto Sans SC Regular at 16 px for deck titles, footers, and grade chips. `pa_font_20.c` defines `pa_font_20` at 20 px for study card front/back text on the 240×320 panel. Both use 2 bpp output with kerning removed. Source is pinned to `notofonts/noto-cjk` revision `f8d157532fbfaeda587e826d4cd5b21a49186f7c`; exact OTF SHA-256 is in each manifest. `OFL.txt` carries SIL OFL 1.1. The 15.6 MiB OTF is not checked in.

To reproduce, install `pip install -r requirements-font.txt`, download `Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf` from the pinned revision to `work/font-source/NotoSansCJKsc-Regular.otf`, then run:

```bash
python tools/generate_font.py              # pa_font_16 (default)
python tools/generate_font.py --size 20    # pa_font_20
```

The script verifies the source hash, uses `lv_font_conv` 1.5.3 through `npx`, writes fonts and manifests, and records cmap coverage.

The generator requests 22,447 codepoints: ASCII, Latin Extended, Greek, general punctuation, enclosed numbers, CJK punctuation plus hiragana and katakana, katakana phonetic extensions, CJK Unified Ideographs, and fullwidth forms. The pinned Noto Sans SC file contains 21,957 of them and omits 490. The added ranges cover the kana, pitch marks (`①`–`⑳`, `⓪`), ellipsis, and `α` used by the JLPT deck. Gaps that remain are parts of Latin Extended, Greek, and punctuation that are not in this OTF, plus `U+9FF0–U+9FFF` and some fullwidth codepoints. `tools/deck.py` checks each character against the `pa_font_16` manifest and reports the missing codepoint before upload (same coverage as `pa_font_20`).

Each generated C source is on the order of 10 MiB as text. Linked flash size depends on the compiler and must be read from the firmware build report. Glyphs live in flash, not DRAM.
