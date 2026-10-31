#!/usr/bin/env python3
"""Collect Unicode codepoints needed for pa_font_20 (study card front/back only)."""
from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "assets/fonts/pa_font_20.subset.json"
MODULE_PATH = ROOT / "tools/deck.py"


def load_deck_module():
    spec = importlib.util.spec_from_file_location("deck", MODULE_PATH)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load {MODULE_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


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
    parser.add_argument(
        "decks",
        nargs="*",
        type=Path,
        help="TSV/APKG paths (default: decks/demo.tsv)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help=f"subset manifest path (default {DEFAULT_OUTPUT.relative_to(ROOT)})",
    )
    args = parser.parse_args()
    paths = args.decks or [ROOT / "decks/demo.tsv"]
    deck = load_deck_module()
    codepoints: set[int] = set()
    for path in paths:
        if not path.is_file():
            raise SystemExit(f"deck path does not exist: {path}")
        codepoints |= deck.collect_study_body_codepoints(path)
    payload = {
        "description": (
            "Unicode codepoints for pa_font_20 study card front/back text. "
            "Regenerate with tools/collect_body_font_subset.py over decks/demo.tsv "
            "and the pinned eggrolls-JLPT10k-v3.5 APKG."
        ),
        "sources": [str(path.resolve()) for path in paths],
        "codepoint_count": len(codepoints),
        "codepoints": sorted(codepoints),
        "ranges": spans(codepoints),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Collected {len(codepoints)} codepoints from {len(paths)} deck source(s) -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
