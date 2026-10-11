# AI Passport font asset

`pa_font_16.c` defines the LVGL font symbol `pa_font_16`: Noto Sans SC Regular rendered at 16 px, 2 bpp, with kerning removed for the constrained target. Its source is pinned to `notofonts/noto-cjk` revision `f8d157532fbfaeda587e826d4cd5b21a49186f7c`; the exact OTF SHA-256 is recorded in `pa_font_16.manifest.json`. `OFL.txt` carries the SIL Open Font License 1.1. The original 15.6 MiB OTF is intentionally not checked in.

To reproduce, install Python dependencies with `pip install -r requirements-font.txt`, download `Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf` from the pinned revision to `work/font-source/NotoSansCJKsc-Regular.otf`, and run `python tools/generate_font.py`. The script verifies the source hash, uses `lv_font_conv` 1.5.3 through `npx`, writes the font and manifest, and records the actual font cmap coverage.

The design requested 21,823 codepoints across ASCII, Latin Extended, CJK punctuation, CJK Unified Ideographs, and fullwidth forms. The pinned Noto Sans SC file contains 21,509 of them. It omits 314 requested characters: parts of Latin Extended-A/B (`U+0104–U+024F` with gaps), `U+9FF0–U+9FFF`, and the absent fullwidth codepoints listed as ranges in the manifest. `tools/deck.py` validates each imported character against those generated coverage ranges and reports the missing codepoint before upload.

The generated C source is about 9.6 MiB as text. Its generated glyph bitmap contains 1,162,067 bytes; the exact linked flash size depends on compiler, LVGL types, and link-time placement and must be read from the firmware build report.
