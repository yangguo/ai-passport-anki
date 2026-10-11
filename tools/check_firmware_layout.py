#!/usr/bin/env python3
"""Check the fixed 8 MiB factory image layout and built app size."""
import csv
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
FLASH_SIZE = 0x800000
EXPECTED = [
    ("nvs", 0x9000, 0x40000),
    ("phy_init", 0x49000, 0x1000),
    ("factory", 0x50000, 0x7B0000),
]


def main() -> int:
    with (ROOT / "partitions.csv").open(newline="", encoding="utf-8") as f:
        rows = [row for row in csv.reader(f) if row and not row[0].lstrip().startswith("#")]
    actual = [(r[0].strip(), int(r[3].strip(), 0), int(r[4].strip(), 0)) for r in rows]
    if actual != EXPECTED:
        raise SystemExit(f"unexpected partition map: {actual!r}")
    if EXPECTED[-1][1] + EXPECTED[-1][2] != FLASH_SIZE:
        raise SystemExit("partition map does not end at 8 MiB")
    app = pathlib.Path(sys.argv[1])
    size = app.stat().st_size
    limit = EXPECTED[-1][2]
    if size > limit:
        raise SystemExit(f"app is {size} bytes; factory partition allows {limit}")
    print(f"partition map valid; app image {size}/{limit} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
