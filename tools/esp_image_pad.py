#!/usr/bin/env python3
"""Pad an ESP app image so len(image) % 65536 == 4096.

Cover Arts sizes a slot as align_64k(image_length) + a 64 KiB sidecar.
A length congruent to 4096 leaves a 60 KiB erased gap between the image
and that sidecar. The extra bytes are one segment whose load address is 0.
The ROM does not map or copy that segment. Its data is included in the
image checksum and, when present, the appended SHA-256.

The pad is not an extension of a mapped DROM segment, so MMU alignment of
the real segments stays as the linker emitted it.
"""
from __future__ import annotations

import hashlib
import pathlib
import struct
import sys

MAGIC = 0xE9
HEADER_LEN = 24
SEGMENT_HEADER_LEN = 8
HASH_LEN = 32
MAX_SEGMENTS = 16
CHECKSUM_INIT = 0xEF
SLOT_MODULUS = 65536
SLOT_RESIDUE = 4096


class ImageError(ValueError):
    """The file is not a pad-able ESP app image."""


def _checksum(segment_data: bytes) -> int:
    value = CHECKSUM_INIT
    for byte in segment_data:
        value ^= byte
    return value & 0xFF


def _checksum_pad_length(unpadded: int) -> int:
    aligned = (unpadded + 1 + 15) & ~15
    return aligned - unpadded


def parse_image(data: bytes) -> dict:
    if len(data) < HEADER_LEN or data[0] != MAGIC:
        raise ImageError("missing ESP image magic")
    segment_count = data[1]
    hash_appended = data[23]
    if segment_count < 1 or segment_count > MAX_SEGMENTS:
        raise ImageError(f"unsupported segment count {segment_count}")
    if hash_appended not in (0, 1):
        raise ImageError(f"unsupported hash flag {hash_appended}")
    offset = HEADER_LEN
    segments: list[tuple[int, bytes]] = []
    raw_segments = bytearray()
    for index in range(segment_count):
        if offset + SEGMENT_HEADER_LEN > len(data):
            raise ImageError(f"truncated segment header {index}")
        load_addr, data_len = struct.unpack_from("<II", data, offset)
        offset += SEGMENT_HEADER_LEN
        if data_len % 4 != 0:
            raise ImageError(f"segment {index} length {data_len} is not 4-byte aligned")
        end = offset + data_len
        if end > len(data):
            raise ImageError(f"truncated segment {index}")
        segment = data[offset:end]
        raw_segments += segment
        segments.append((load_addr, segment))
        offset = end
    pad_len = _checksum_pad_length(offset)
    if offset + pad_len > len(data):
        raise ImageError("truncated checksum")
    checksum_region = data[offset:offset + pad_len]
    calculated = _checksum(bytes(raw_segments))
    if checksum_region[-1] != calculated:
        raise ImageError(
            f"checksum mismatch: image 0x{checksum_region[-1]:02x} calculated 0x{calculated:02x}"
        )
    image_len = offset + pad_len
    digest = b""
    if hash_appended:
        if image_len + HASH_LEN > len(data):
            raise ImageError("truncated SHA-256")
        digest = data[image_len:image_len + HASH_LEN]
        actual = hashlib.sha256(data[:image_len]).digest()
        if digest != actual:
            raise ImageError("appended SHA-256 does not match the image")
        image_len += HASH_LEN
    if image_len != len(data):
        raise ImageError(f"image length {image_len} does not match file size {len(data)}")
    return {
        "header": bytearray(data[:HEADER_LEN]),
        "segments": segments,
        "hash_appended": bool(hash_appended),
        "image_len": image_len,
    }


def image_length(data: bytes) -> int:
    return int(parse_image(data)["image_len"])


def _assemble(header: bytes, segments: list[tuple[int, bytes]], hash_appended: bool) -> bytes:
    body = bytearray(header)
    raw = bytearray()
    for load_addr, segment in segments:
        body += struct.pack("<II", load_addr, len(segment))
        body += segment
        raw += segment
    pad_len = _checksum_pad_length(len(body))
    region = bytearray(pad_len)
    region[-1] = _checksum(bytes(raw))
    body += region
    if hash_appended:
        body += hashlib.sha256(body).digest()
    return bytes(body)


def _pad_length(body_end: int, hash_appended: bool) -> int:
    for data_len in range(4, SLOT_MODULUS + 32, 4):
        total = body_end + SEGMENT_HEADER_LEN + data_len
        total += _checksum_pad_length(total)
        if hash_appended:
            total += HASH_LEN
        if total % SLOT_MODULUS == SLOT_RESIDUE:
            return data_len
    raise ImageError("no address-0 pad meets the slot gap")


def pad_app_image(data: bytes) -> bytes:
    parsed = parse_image(data)
    if parsed["image_len"] % SLOT_MODULUS == SLOT_RESIDUE:
        return data
    segments: list[tuple[int, bytes]] = list(parsed["segments"])
    if len(segments) >= MAX_SEGMENTS:
        raise ImageError("image already has 16 segments")
    header = bytearray(parsed["header"])
    body_end = HEADER_LEN
    for _load, segment in segments:
        body_end += SEGMENT_HEADER_LEN + len(segment)
    data_len = _pad_length(body_end, parsed["hash_appended"])
    segments.append((0, b"\x00" * data_len))
    header[1] = len(segments)
    padded = _assemble(bytes(header), segments, parsed["hash_appended"])
    if len(padded) % SLOT_MODULUS != SLOT_RESIDUE:
        raise ImageError("pad did not land on the slot residue")
    parse_image(padded)
    return padded


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        raise SystemExit(f"usage: {argv[0]} APP.bin")
    path = pathlib.Path(argv[1])
    original = path.read_bytes()
    padded = pad_app_image(original)
    if padded != original:
        path.write_bytes(padded)
    print(
        f"app image {len(original)} -> {len(padded)} bytes "
        f"(length % {SLOT_MODULUS} = {len(padded) % SLOT_MODULUS})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
