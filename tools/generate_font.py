#!/usr/bin/env python3
"""Rebuild the checked-in 16px AI Passport font from the pinned Noto source."""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

PINNED_REPOSITORY = "https://github.com/notofonts/noto-cjk"
PINNED_REVISION = "f8d157532fbfaeda587e826d4cd5b21a49186f7c"
PINNED_FONT_PATH = "Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf"
PINNED_LV_FONT_CONV = "1.5.3"
PINNED_FONT_SHA256 = "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b"
CANONICAL_FONT_PATH = Path("work/font-source/NotoSansCJKsc-Regular.otf")
REQUESTED_RANGES = (
    (0x0020, 0x007E),  # ASCII
    (0x00A0, 0x024F),  # Latin extended
    (0x0370, 0x03FF),  # Greek (JLPT notes use α)
    (0x2000, 0x206F),  # general punctuation (… — “ ”)
    (0x2460, 0x24FF),  # enclosed numbers used as pitch marks (① ⓪)
    (0x3000, 0x30FF),  # CJK punctuation, hiragana, katakana
    (0x31F0, 0x31FF),  # katakana phonetic extensions
    (0x4E00, 0x9FFF),  # CJK unified ideographs
    (0xFF00, 0xFFEF),  # halfwidth/fullwidth
)
ROOT = Path(__file__).resolve().parents[1]


def spans(codepoints: set[int]) -> list[list[int]]:
    result: list[list[int]] = []
    for cp in sorted(codepoints):
        if result and result[-1][1] + 1 == cp:
            result[-1][1] = cp
        else:
            result.append([cp, cp])
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, default=ROOT / CANONICAL_FONT_PATH, help="Pinned Noto Sans SC Regular OTF")
    parser.add_argument("--output", type=Path, default=ROOT / "assets/fonts/pa_font_16.c")
    parser.add_argument("--manifest", type=Path, default=ROOT / "assets/fonts/pa_font_16.manifest.json")
    args = parser.parse_args()
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        parser.error("font generation requires fonttools; install requirements-font.txt")
    if not args.font.is_file():
        parser.error(f"font source does not exist: {args.font}")

    font_bytes = args.font.read_bytes()
    font_hash = hashlib.sha256(font_bytes).hexdigest()
    if font_hash != PINNED_FONT_SHA256:
        parser.error(f"font source SHA-256 does not match pinned upstream file ({PINNED_FONT_SHA256})")
    canonical_font = ROOT / CANONICAL_FONT_PATH
    canonical_font.parent.mkdir(parents=True, exist_ok=True)
    if args.font.resolve() != canonical_font.resolve():
        canonical_font.write_bytes(font_bytes)
    ttfont = TTFont(canonical_font)
    cmap = set(ttfont.getBestCmap())
    ttfont.close()
    requested = {cp for start, end in REQUESTED_RANGES for cp in range(start, end + 1)}
    available = requested & cmap
    missing = requested - cmap
    if not available:
        parser.error("font source has none of the requested glyphs")

    manifest = {
        "font_symbol": "pa_font_16",
        "font_size_px": 16,
        "bpp": 2,
        "font_family": "Noto Sans SC",
        "source_repository": PINNED_REPOSITORY,
        "source_revision": PINNED_REVISION,
        "source_path": PINNED_FONT_PATH,
        "source_file_sha256": font_hash,
        "license": "SIL Open Font License 1.1",
        "license_file": "OFL.txt",
        "requested_ranges": [[start, end] for start, end in REQUESTED_RANGES],
        "supported_ranges": spans(available),
        "unsupported_ranges": spans(missing),
        "requested_glyph_count": len(requested),
        "supported_glyph_count": len(available),
        "unsupported_glyph_count": len(missing),
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    output_arg = args.output.relative_to(ROOT) if args.output.resolve().is_relative_to(ROOT) else args.output
    command = ["npx", "--yes", f"lv_font_conv@{PINNED_LV_FONT_CONV}", "--size", "16", "--bpp", "2",
               "--format", "lvgl", "--font", CANONICAL_FONT_PATH.as_posix(), "--no-kerning",
               "--lv-include", "lvgl.h", "--lv-font-name", "pa_font_16", "--output", str(output_arg)]
    for start, end in spans(available):
        command.extend(["--range", f"0x{start:X}-0x{end:X}"])
    try:
        subprocess.run(command, cwd=ROOT, check=True)
    except FileNotFoundError:
        parser.error("npx is required to run the pinned lv_font_conv generator")
    except subprocess.CalledProcessError as exc:
        return exc.returncode
    print(f"Generated {len(available)} glyphs; {len(missing)} requested codepoints are absent and will be rejected by deck.py.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
