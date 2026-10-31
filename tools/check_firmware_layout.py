#!/usr/bin/env python3
"""Check the stock 8 MiB single-factory map and the padded app image."""
import csv
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from esp_image_pad import SLOT_MODULUS, SLOT_RESIDUE, image_length  # noqa: E402


ROOT = pathlib.Path(__file__).resolve().parents[1]
FLASH_SIZE = 0x800000
FULL_TAIL = 0x460000 + 0xD0000
EXPECTED = [
    ("nvs", 0x9000, 0x6000),
    ("phy_init", 0xf000, 0x1000),
    ("factory", 0x10000, 0x7F0000),
]


FACTORY_OFFSET = next(offset for name, offset, _size in EXPECTED if name == "factory")


def check_app(app: pathlib.Path) -> bytes:
    factory = next(size for name, _offset, size in EXPECTED if name == "factory")
    data = app.read_bytes()
    length = image_length(data)
    if length != len(data):
        raise SystemExit(f"parsed image length {length} != file size {len(data)}")
    if length > factory:
        raise SystemExit(f"app is {length} bytes; factory partition allows {factory}")
    if length % SLOT_MODULUS != SLOT_RESIDUE:
        raise SystemExit(
            f"app length {length} % {SLOT_MODULUS} = {length % SLOT_MODULUS}, want {SLOT_RESIDUE}"
        )
    tail = factory - length
    if tail < FULL_TAIL:
        raise SystemExit(
            f"factory tail {tail} bytes is smaller than the 12000-card windows ({FULL_TAIL})"
        )
    print(
        f"partition map valid; app image {length}/{factory} bytes; "
        f"factory tail {tail} bytes"
    )
    return data


def check_merged(full: pathlib.Path, app: bytes) -> None:
    merged = full.read_bytes()
    end = FACTORY_OFFSET + len(app)
    if len(merged) < end:
        raise SystemExit(f"{full} is {len(merged)} bytes; padded app does not fit at {FACTORY_OFFSET:#x}")
    if merged[FACTORY_OFFSET:end] != app:
        raise SystemExit(f"{full} at {FACTORY_OFFSET:#x} is not the padded app image")
    length = image_length(merged[FACTORY_OFFSET:end])
    if length != len(app) or length % SLOT_MODULUS != SLOT_RESIDUE:
        raise SystemExit("padded app inside the merged image lost its slot residue")
    print(f"merged image matches padded app at {FACTORY_OFFSET:#x} ({len(app)} bytes)")


def main() -> int:
    with (ROOT / "partitions.csv").open(newline="", encoding="utf-8") as handle:
        rows = [row for row in csv.reader(handle) if row and not row[0].lstrip().startswith("#")]
    actual = [(row[0].strip(), int(row[3].strip(), 0), int(row[4].strip(), 0)) for row in rows]
    if actual != EXPECTED:
        raise SystemExit(f"unexpected partition map: {actual!r}")
    end = EXPECTED[-1][1] + EXPECTED[-1][2]
    if end != FLASH_SIZE:
        raise SystemExit(f"partition map ends at {end:#x}, not 8 MiB")
    if len(sys.argv) not in (2, 3):
        raise SystemExit(f"usage: {sys.argv[0]} APP.bin [FULL.bin]")
    app = check_app(pathlib.Path(sys.argv[1]))
    if len(sys.argv) == 3:
        check_merged(pathlib.Path(sys.argv[2]), app)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
