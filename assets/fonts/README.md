# AI Passport font asset

`pa_font_16.c` defines the LVGL font symbol `pa_font_16`: Noto Sans SC Regular rendered at 16 px, 2 bpp, with kerning removed for the constrained target. Its source is pinned to `notofonts/noto-cjk` revision `f8d157532fbfaeda587e826d4cd5b21a49186f7c`; the exact OTF SHA-256 is recorded in `pa_font_16.manifest.json`. `OFL.txt` carries the SIL Open Font License 1.1. The original 15.6 MiB OTF is intentionally not checked in.

To reproduce, install Python dependencies with `pip install -r requirements-font.txt`, download `Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf` from the pinned revision to `work/font-source/NotoSansCJKsc-Regular.otf`, and run `python tools/generate_font.py`. The script verifies the source hash, uses `lv_font_conv` 1.5.3 through `npx`, writes the font and manifest, and records the actual font cmap coverage.

The generator requests 22,447 codepoints: ASCII, Latin Extended, Greek, general punctuation, enclosed numbers, CJK punctuation plus hiragana and katakana, katakana phonetic extensions, CJK Unified Ideographs, and fullwidth forms. The pinned Noto Sans SC file contains 21,957 of them and omits 490. The added ranges cover the kana, pitch marks (`①`–`⑳`, `⓪`), ellipsis, and `α` used by the JLPT deck. Gaps that remain are parts of Latin Extended, Greek, and punctuation that are not in this OTF, plus `U+9FF0–U+9FFF` and some fullwidth codepoints. `tools/deck.py` checks each character against the manifest and reports the missing codepoint before upload.

The generated C source is about 10 MiB as text. The exact linked flash size, including the glyph bitmap, depends on the compiler and must be read from the firmware build report.
