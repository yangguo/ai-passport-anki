"""Synthetic ESP image padding for the Cover Arts slot gap."""
import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from esp_image_pad import (  # noqa: E402
    HASH_LEN,
    SLOT_MODULUS,
    SLOT_RESIDUE,
    ImageError,
    image_length,
    pad_app_image,
    parse_image,
)


def _image(segment: bytes, *, segments: int = 1, hash_appended: bool = True) -> bytes:
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = segments
    header[23] = 1 if hash_appended else 0
    body = bytearray(header)
    for _ in range(segments):
        body += struct.pack("<II", 0x3C000000, len(segment))
        body += segment
    unpadded = len(body)
    pad_len = ((unpadded + 1 + 15) & ~15) - unpadded
    region = bytearray(pad_len)
    checksum = 0xEF
    for byte in segment * segments:
        checksum ^= byte
    region[-1] = checksum & 0xFF
    body += region
    if hash_appended:
        body += hashlib.sha256(body).digest()
    return bytes(body)


def test_pad_lands_on_slot_residue_and_stays_valid():
    original = _image(b"\x11\x22\x33\x44" * 8)
    assert len(original) % SLOT_MODULUS != SLOT_RESIDUE
    padded = pad_app_image(original)
    parsed = parse_image(padded)
    assert len(padded) % SLOT_MODULUS == SLOT_RESIDUE
    assert image_length(padded) == len(padded)
    assert parsed["header"][1] == 2
    assert parsed["segments"][-1][0] == 0
    assert len(parsed["segments"][-1][1]) % 4 == 0
    assert parsed["segments"][0][1] == b"\x11\x22\x33\x44" * 8
    assert pad_app_image(padded) == padded


def test_already_aligned_image_is_unchanged():
    seed = _image(b"\x01\x02\x03\x04")
    once = pad_app_image(seed)
    assert once != seed
    assert pad_app_image(once) == once


def test_checksum_rejects_a_flipped_byte():
    data = bytearray(_image(b"\x10\x20\x30\x40", hash_appended=False))
    data[24 + 8] ^= 0xFF
    try:
        parse_image(bytes(data))
    except ImageError as exc:
        assert "checksum" in str(exc)
    else:
        raise AssertionError("corrupt image was accepted")


def test_hash_covers_the_pad_segment():
    padded = pad_app_image(_image(b"\xAB\xCD\xEF\x01" * 3))
    assert hashlib.sha256(padded[:-HASH_LEN]).digest() == padded[-HASH_LEN:]
