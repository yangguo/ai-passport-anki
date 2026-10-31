# AI Passport font assets

`pa_font_16.c` defines the LVGL font symbol `pa_font_16`: Noto Sans SC Regular at 16 px for deck titles, footers, and grade chips. `pa_font_20.c` defines `pa_font_20` at 20 px for study card front/back text on the 240×320 panel. Both use 2 bpp output with kerning removed. Source is pinned to `notofonts/noto-cjk` revision `f8d157532fbfaeda587e826d4cd5b21a49186f7c`; exact OTF SHA-256 is in each manifest. `OFL.txt` carries SIL OFL 1.1. The 15.6 MiB OTF is not checked in.

To reproduce, install `pip install -r requirements-font.txt`, download `Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf` from the pinned revision to `work/font-source/NotoSansCJKsc-Regular.otf`, then run:

```bash
python tools/generate_font.py              # pa_font_16 (default)
python tools/collect_body_font_subset.py decks/demo.tsv /path/to/eggrolls-JLPT10k-v3.5.apkg
python tools/generate_font.py --size 20    # pa_font_20 (script blocks + JLPT/demo subset)
```

The script verifies the source hash, uses `lv_font_conv` 1.5.3 through `npx`, writes fonts and manifests, and records cmap coverage.

`pa_font_16` requests 22,447 codepoints: ASCII, Latin Extended, Greek, general punctuation, enclosed numbers, CJK punctuation plus hiragana and katakana, katakana phonetic extensions, CJK Unified Ideographs, and fullwidth forms. The pinned Noto Sans SC file contains 21,957 of them and omits 490. `tools/deck.py` checks deck names against the `pa_font_16` manifest.

`pa_font_20` keeps the same non-Hanzi script blocks as `pa_font_16`, but CJK unified ideographs come only from `assets/fonts/pa_font_20.subset.json`. That file lists front/back codepoints collected from `decks/demo.tsv` and the pinned `eggrolls-JLPT10k-v3.5` APKG via `tools/collect_body_font_subset.py`. `tools/deck.py` checks study card front/back against the `pa_font_20` manifest. The device COMMIT path validates layout with `pa_font_20` on front/back only.

Each generated C source is several MiB as text. Linked flash size depends on the compiler and must be read from the firmware build report. Glyphs live in flash, not DRAM.
