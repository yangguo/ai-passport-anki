#!/usr/bin/env python3
"""Validate AI Passport Anki decks and upload them over USB Serial/JTAG."""
from __future__ import annotations

import argparse
import csv
import html.parser
import json
import re
import sqlite3
import struct
import sys
import tempfile
import zipfile
import zlib
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Callable, Sequence

MAX_CARDS = 12000
MAX_DECKS = 64
MAX_DECK_BYTES = 47
MAX_TEXT_BYTES = 511
MAX_LINE_BYTES = 1200
MAX_CHUNK_BYTES = 480
UINT32_MAX = 0xFFFFFFFF
PROTOCOL_VERSION = 2
APKG_DB_MAX_BYTES = 16 * 1024 * 1024
APKG_DECOMPRESSED_MAX_BYTES = 64 * 1024 * 1024
FONT_MANIFEST = Path(__file__).resolve().parents[1] / "assets/fonts/pa_font_16.manifest.json"


class DeckError(Exception):
    """Invalid input deck or failed device exchange."""


@dataclass(frozen=True)
class Card:
    id: int
    deck: str
    front: str
    back: str
    anki_id: str = ""
    anki_note_id: str = ""


class _PlainText(html.parser.HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.parts: list[str] = []
        self.skip_depth = 0

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag in ("script", "style"):
            self.skip_depth += 1
        elif tag == "br" and not self.skip_depth:
            self.parts.append("\n")

    def handle_endtag(self, tag: str) -> None:
        if tag in ("script", "style") and self.skip_depth:
            self.skip_depth -= 1

    def handle_data(self, data: str) -> None:
        if not self.skip_depth:
            self.parts.append(data)


_SOUND = re.compile(r"\[sound:[^\]]*\]", re.IGNORECASE)
_IMG = re.compile(r"<img\b[^>]*>", re.IGNORECASE)
_FURIGANA = re.compile(r"([^\s\[\]]+)\[([^\[\]]+)\]")


def _strip_html(source: str) -> str:
    parser = _PlainText()
    try:
        parser.feed(source)
        parser.close()
    except Exception as exc:
        raise DeckError(f"invalid HTML field: {exc}") from exc
    return "".join(parser.parts).strip()


def _clean_field(source: str) -> str:
    """Drop media, keep line breaks, and expand Anki furigana kanji[reading] to 漢字（かな）."""
    without_media = _IMG.sub("", _SOUND.sub("", source))
    text = _strip_html(without_media)
    text = _FURIGANA.sub(lambda match: f"{match.group(1)}（{match.group(2)}）", text)
    text = re.sub(r"[ \t]+\n", "\n", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


@lru_cache(maxsize=1)
def _font_ranges() -> tuple[tuple[int, int], ...]:
    try:
        manifest = json.loads(FONT_MANIFEST.read_text(encoding="utf-8"))
        ranges = manifest["supported_ranges"]
        return tuple((int(start), int(end)) for start, end in ranges)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise DeckError(f"font coverage manifest is missing or invalid: {exc}") from exc

def _font_contains(codepoint: int) -> bool:
    return any(start <= codepoint <= end for start, end in _font_ranges())


def _check_chars(value: str, where: str, *, allow_newline: bool = False) -> None:
    if "\x00" in value:
        raise DeckError(f"{where}: embedded NUL is not allowed")
    for char in value:
        codepoint = ord(char)
        if char == "\n":
            if allow_newline:
                continue
            raise DeckError(f"{where}: newline is not allowed")
        if codepoint < 0x20 or 0x7F <= codepoint <= 0x9F:
            raise DeckError(f"{where}: control character U+{codepoint:04X} is not allowed")
        if not _font_contains(codepoint):
            raise DeckError(f"{where}: no pa_font_16 glyph for U+{codepoint:04X}")


def _validate_card(card: Card, row: int) -> Card:
    card = Card(card.id, card.deck.replace("\r\n", "\n").replace("\r", "\n"),
                card.front.replace("\r\n", "\n").replace("\r", "\n"),
                card.back.replace("\r\n", "\n").replace("\r", "\n"),
                card.anki_id, card.anki_note_id)
    if not isinstance(card.id, int) or isinstance(card.id, bool) or not 1 <= card.id <= UINT32_MAX:
        raise DeckError(f"row {row}: id must be in range 1..{UINT32_MAX}")
    if not card.deck:
        raise DeckError(f"row {row}: deck is empty")
    if not card.front:
        raise DeckError(f"row {row}: front is empty")
    if not card.back:
        raise DeckError(f"row {row}: back is empty")
    _check_chars(card.deck, f"row {row} deck")
    _check_chars(card.front, f"row {row} front", allow_newline=True)
    _check_chars(card.back, f"row {row} back", allow_newline=True)
    for name, value, limit in (("deck", card.deck, MAX_DECK_BYTES),
                               ("front", card.front, MAX_TEXT_BYTES),
                               ("back", card.back, MAX_TEXT_BYTES)):
        length = len(value.encode("utf-8"))
        if length > limit:
            raise DeckError(f"row {row}: {name} exceeds {limit} bytes (got {length})")
    return card


def _finish(cards: list[Card]) -> list[Card]:
    if not cards:
        raise DeckError("deck has no cards")
    if len(cards) > MAX_CARDS:
        raise DeckError(f"deck exceeds {MAX_CARDS} cards (got {len(cards)})")
    seen: set[int] = set()
    normalized: list[Card] = []
    for row, card in enumerate(cards, 1):
        card = _validate_card(card, row)
        if card.id in seen:
            raise DeckError(f"row {row}: duplicate id {card.id}")
        seen.add(card.id)
        normalized.append(card)
    if len({card.deck for card in normalized}) > MAX_DECKS:
        raise DeckError(f"deck exceeds {MAX_DECKS} distinct decks")
    return normalized


def _read_tsv(path: Path) -> list[Card]:
    try:
        with path.open("r", encoding="utf-8-sig", newline="") as stream:
            contents = stream.read()
    except (OSError, UnicodeError) as exc:
        raise DeckError(f"cannot read UTF-8 deck: {exc}") from exc
    if "\x00" in contents:
        raise DeckError("input contains embedded NUL")
    stream = __import__("io").StringIO(contents, newline="")
    reader = csv.reader(stream, delimiter="\t", quotechar='"', strict=True)
    try:
        rows = list(reader)
    except csv.Error as exc:
        raise DeckError(f"invalid TSV near line {reader.line_num}: {exc}") from exc
    html_export = False
    while rows and rows[0] and rows[0][0].startswith("#"):
        directive = rows.pop(0)[0].strip().lower()
        if directive not in ("#separator:tab", "#html:false", "#html:true"):
            raise DeckError(f"unsupported Anki export directive: {directive}")
        if directive == "#html:true":
            html_export = True
    if rows and [field.strip().lower() for field in rows[0]] in (["front", "back"], ["question", "answer"]):
        rows.pop(0)
    if not rows:
        raise DeckError("deck has no rows")
    header = [field.strip().lower() for field in rows[0]]
    cards: list[Card] = []
    if header == ["id", "deck", "front", "back"]:
        data_rows = rows[1:]
        for index, fields in enumerate(data_rows, 2):
            if len(fields) != 4:
                raise DeckError(f"row {index}: expected 4 tab-separated fields, got {len(fields)}")
            if not fields[0].isdigit():
                raise DeckError(f"row {index}: id must be a decimal integer")
            try:
                card_id = int(fields[0], 10)
            except ValueError as exc:
                raise DeckError(f"row {index}: id must be a decimal integer") from exc
            values = [_clean_field(value) for value in fields[2:4]] if html_export else fields[2:4]
            cards.append(Card(card_id, fields[1], values[0], values[1]))
    else:
        # Anki text exports commonly contain two fields and no column header.
        if len(rows[0]) != 2:
            raise DeckError("expected header id<TAB>deck<TAB>front<TAB>back or a two-field Anki text export")
        for index, fields in enumerate(rows, 1):
            if len(fields) != 2:
                raise DeckError(f"row {index}: expected 2 Anki text fields, got {len(fields)}")
            values = [_clean_field(value) for value in fields] if html_export else fields
            cards.append(Card(index, "Imported", values[0], values[1]))
    return _finish(cards)


def _decompress_anki21b(data: bytes, member: str) -> bytes:
    try:
        import zstandard
    except ImportError as exc:
        raise DeckError(
            f"{member} is zstd-compressed; install it with: pip install -r requirements.txt"
        ) from exc
    try:
        reader = zstandard.ZstdDecompressor().stream_reader(data)
        try:
            chunks: list[bytes] = []
            total = 0
            while True:
                piece = reader.read(min(1 << 20, APKG_DECOMPRESSED_MAX_BYTES + 1 - total))
                if not piece:
                    break
                chunks.append(piece)
                total += len(piece)
                if total > APKG_DECOMPRESSED_MAX_BYTES:
                    raise DeckError(
                        f"cannot decompress {member}: output exceeds "
                        f"{APKG_DECOMPRESSED_MAX_BYTES} byte conversion limit"
                    )
        finally:
            reader.close()
    except DeckError:
        raise
    except Exception as exc:
        raise DeckError(f"cannot decompress {member}: {exc}") from exc
    return b"".join(chunks)


def _apkg_database(path: Path) -> bytes:
    try:
        archive = zipfile.ZipFile(path, "r")
    except (OSError, zipfile.BadZipFile) as exc:
        raise DeckError(f"invalid APKG archive: {exc}") from exc
    with archive:
        names = set(archive.namelist())
        if "collection.anki21b" in names:
            member = "collection.anki21b"
        elif "collection.anki21" in names:
            member = "collection.anki21"
        elif "collection.anki2" in names:
            member = "collection.anki2"
        else:
            member = None
        if member is None:
            raise DeckError("APKG must contain collection.anki2, collection.anki21, or collection.anki21b")
        info = archive.getinfo(member)
        if info.file_size > APKG_DB_MAX_BYTES:
            raise DeckError(f"APKG collection exceeds {APKG_DB_MAX_BYTES} byte conversion limit")
        try:
            blob = archive.read(member)
        except (OSError, zipfile.BadZipFile, RuntimeError) as exc:
            raise DeckError(f"cannot read APKG collection: {exc}") from exc
        if member == "collection.anki21b":
            return _decompress_anki21b(blob, member)
        return blob


def _field_names(model: dict) -> list[str]:
    names: list[str] = []
    for field in model.get("flds", []):
        if isinstance(field, dict) and "name" in field:
            names.append(str(field["name"]))
    return names


def _field_map(model: dict) -> dict[str, int]:
    return {name: index for index, name in enumerate(_field_names(model))}


def _is_jlpt_model(model: dict) -> bool:
    names = set(_field_names(model))
    return {"VocabKanji", "VocabFurigana", "VocabDefSC"} <= names


def _take_field(fields: list[str], mapping: dict[str, int], name: str) -> str:
    index = mapping.get(name)
    if index is None or index >= len(fields):
        return ""
    return _clean_field(fields[index])


def _fit_jlpt_back(parts: list[str]) -> str:
    """parts: pitch, pos, definition, plus, sentence, sentence definition. Drop plus, then the sentence gloss."""
    selected = list(parts)
    for drop in (3, 5):
        text = "\n".join(part for part in selected if part)
        if len(text.encode("utf-8")) <= MAX_TEXT_BYTES:
            return text
        selected[drop] = ""
    text = "\n".join(part for part in selected if part)
    if len(text.encode("utf-8")) <= MAX_TEXT_BYTES:
        return text
    raise DeckError(f"back exceeds {MAX_TEXT_BYTES} bytes after dropping optional JLPT fields")


def _jlpt_sides(fields: list[str], mapping: dict[str, int], note_id: int) -> tuple[str, str]:
    kanji = _take_field(fields, mapping, "VocabKanji")
    furigana = _take_field(fields, mapping, "VocabFurigana")
    if not kanji:
        raise DeckError(f"JLPT note {note_id} has an empty VocabKanji field")
    front = f"{kanji}\n{furigana}" if furigana and furigana != kanji else kanji
    back = _fit_jlpt_back([
        _take_field(fields, mapping, "VocabPitch"),
        _take_field(fields, mapping, "VocabPoS"),
        _take_field(fields, mapping, "VocabDefSC"),
        _take_field(fields, mapping, "VocabPlus"),
        _take_field(fields, mapping, "SentFurigana1"),
        _take_field(fields, mapping, "SentDefSC1"),
    ])
    if not back:
        raise DeckError(f"JLPT note {note_id} has an empty back")
    return front, back


def card_from_note(model: dict, fields: list[str], deck_name: str, note_id: int, anki_card_id: int,
                   device_id: int) -> Card:
    """Map one Anki note onto a device card. Basic and JLPT fields only; Cloze is rejected."""
    name = str(model.get("name", ""))
    kind = int(model.get("type", -1))
    if kind == 1 or name.lower() == "cloze" or any("{{c" in field.lower() for field in fields):
        raise DeckError(f"Cloze note {note_id} is unsupported")
    if _is_jlpt_model(model):
        front, back = _jlpt_sides(fields, _field_map(model), note_id)
    elif kind == 0 and name == "Basic":
        if len(fields) < 2:
            raise DeckError(f"Basic note {note_id} must have at least two text fields")
        front, back = _clean_field(fields[0]), _clean_field(fields[1])
    else:
        raise DeckError(f"custom model {name!r} is unsupported; only Basic and JLPT fields are converted")
    return Card(device_id, deck_name, front, back, str(int(anki_card_id)), str(int(note_id)))


def _table_names(connection: sqlite3.Connection) -> set[str]:
    try:
        rows = connection.execute("SELECT name FROM sqlite_master WHERE type='table'")
    except sqlite3.Error as exc:
        raise DeckError(f"collection schema cannot be read: {exc}") from exc
    return {str(row[0]) for row in rows}


def _leaf_deck_name(name: str) -> str:
    """Schema-18 decks nest with \x1F separators (older exports use ::); the device is flat."""
    leaf = name.replace("\x1f", "::").split("::")[-1]
    return leaf.strip()


def _selected_cards(connection: sqlite3.Connection, only_decks: set[str] | None, *, schema18: bool) -> list[Card]:
    try:
        if schema18:
            decks = {int(row["id"]): str(row["name"]) for row in connection.execute("SELECT id, name FROM decks")}
            notetypes = {int(row["id"]): str(row["name"]) for row in connection.execute("SELECT id, name FROM notetypes")}
            field_names: dict[int, list[str]] = {}
            for row in connection.execute("SELECT ntid, ord, name FROM fields ORDER BY ntid, ord"):
                field_names.setdefault(int(row["ntid"]), []).append(str(row["name"]))
            models = {
                ntid: {"name": name, "type": 1 if "cloze" in name.lower() else 0,
                       "flds": [{"name": field} for field in field_names.get(ntid, [])]}
                for ntid, name in notetypes.items()
            }
        else:
            collection = connection.execute("SELECT models, decks FROM col LIMIT 1").fetchone()
            if collection is None:
                raise DeckError("collection has no metadata row")
            models = {int(key): model for key, model in json.loads(collection["models"]).items()}
            decks = {int(key): str(value.get("name", "")) for key, value in json.loads(collection["decks"]).items()}
        notes = {int(row["id"]): row for row in connection.execute("SELECT id, mid, flds FROM notes")}
        if only_decks is None:
            total = int(connection.execute("SELECT COUNT(*) FROM cards").fetchone()[0])
            if total > MAX_CARDS:
                raise DeckError(f"deck exceeds {MAX_CARDS} cards (got {total})")
        cards: list[Card] = []
        seen_notes: set[int] = set()
        query = "SELECT id AS cid, nid, did FROM cards ORDER BY id"
        for row in connection.execute(query):
            deck_name = decks.get(int(row["did"]))
            if deck_name is None:
                raise DeckError(f"card {row['cid']} references a missing deck")
            deck_name = _leaf_deck_name(deck_name)
            if only_decks is not None and deck_name not in only_decks:
                continue
            note_id = int(row["nid"])
            if note_id in seen_notes:
                raise DeckError("multiple cards from one note are unsupported")
            seen_notes.add(note_id)
            note = notes.get(note_id)
            if note is None:
                raise DeckError(f"card {row['cid']} references a missing note")
            model = models.get(int(note["mid"]))
            if not model:
                raise DeckError(f"note {note_id} references a missing model")
            fields = str(note["flds"]).split("\x1f")
            cards.append(card_from_note(model, fields, deck_name, note_id, int(row["cid"]), len(cards) + 1))
    except DeckError:
        raise
    except (sqlite3.Error, ValueError, TypeError, KeyError, AttributeError, json.JSONDecodeError) as exc:
        raise DeckError(f"unsupported or invalid collection schema: {exc}") from exc
    if not cards:
        raise DeckError("deck has no cards")
    return _finish(cards)


def read_sqlite_collection(connection: sqlite3.Connection, only_decks: set[str] | None = None) -> list[Card]:
    """Read Basic/JLPT cards from a schema 11 or schema 18 collection. SQL errors stop the read."""
    tables = _table_names(connection)
    if "notes" not in tables or "cards" not in tables:
        raise DeckError("collection schema has no notes/cards tables")
    schema18 = {"notetypes", "fields", "decks"} <= tables
    if not schema18 and "col" not in tables:
        raise DeckError("collection schema has neither notetypes nor col.models")
    return _selected_cards(connection, only_decks, schema18=schema18)


def _read_apkg(path: Path) -> list[Card]:
    database = _apkg_database(path)
    if len(database) > APKG_DECOMPRESSED_MAX_BYTES:
        raise DeckError(f"APKG collection exceeds {APKG_DECOMPRESSED_MAX_BYTES} byte conversion limit")
    try:
        with tempfile.TemporaryDirectory(prefix="passport-anki-") as directory:
            db_path = Path(directory) / "collection.sqlite"
            db_path.write_bytes(database)
            connection = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
            connection.row_factory = sqlite3.Row
            try:
                return read_sqlite_collection(connection)
            finally:
                connection.close()
    except DeckError:
        raise
    except (OSError, sqlite3.Error) as exc:
        raise DeckError(f"unsupported or invalid legacy APKG collection: {exc}") from exc


def load_deck(filename: str | Path) -> list[Card]:
    path = Path(filename)
    suffix = path.suffix.lower()
    if suffix == ".apkg":
        return _read_apkg(path)
    if suffix in (".tsv", ".txt", ".anki"):
        return _read_tsv(path)
    raise DeckError("input must be a UTF-8 TSV/Anki text export or legacy .apkg")


def pack_card(card: Card) -> bytes:
    deck = card.deck.encode("utf-8")
    front = card.front.encode("utf-8")
    back = card.back.encode("utf-8")
    if len(deck) > MAX_DECK_BYTES or len(front) > MAX_TEXT_BYTES or len(back) > MAX_TEXT_BYTES:
        raise DeckError("card does not fit the device record")
    return struct.pack("<IBHH", card.id, len(deck), len(front), len(back)) + deck + front + back


def payload_for(cards: Sequence[Card]) -> bytes:
    return b"".join(pack_card(card) for card in cards)


def write_tsv(cards: Sequence[Card], path: str | Path) -> None:
    with Path(path).open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n", quoting=csv.QUOTE_MINIMAL)
        writer.writerow(["id", "deck", "front", "back"])
        for card in cards:
            writer.writerow([card.id, card.deck, card.front, card.back])


def write_map(cards: Sequence[Card], path: str | Path, *, source: str = "file",
              imported_at_ms: int | None = None, decks: list[str] | None = None) -> None:
    document = {
        "version": 1,
        "source": source,
        "decks": decks if decks is not None else sorted({card.deck for card in cards}),
        "imported_at_ms": imported_at_ms,
        "note": "Local map from device u32 ids to Anki card ids. Do not commit.",
        "cards": {
            str(card.id): {
                "anki_card_id": card.anki_id or str(card.id),
                "anki_note_id": card.anki_note_id,
                "deck": card.deck,
            }
            for card in cards
        },
    }
    Path(path).write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _write_map(cards: Sequence[Card], path: str | Path) -> None:
    write_map(cards, path, source="file")


def commands_for(cards: Sequence[Card], day: int | None = None) -> list[str]:
    cards = _finish(list(cards))
    commands = []
    if day is not None:
        if not isinstance(day, int) or isinstance(day, bool) or not 0 <= day <= UINT32_MAX // 1440:
            raise DeckError("day must be in range 0..2982616")
        commands.append(f"DAY {day}")
    payload = payload_for(cards)
    crc = zlib.crc32(payload) & UINT32_MAX
    commands.append(f"BEGIN {len(cards)} {crc} {len(payload)}")
    offset = 0
    while offset < len(payload):
        chunk = payload[offset:offset + MAX_CHUNK_BYTES]
        commands.append(f"CHUNK {offset} {chunk.hex()}")
        offset += len(chunk)
    commands.append("COMMIT")
    for command in commands:
        if len(command.encode("ascii")) > MAX_LINE_BYTES:
            raise DeckError(f"generated command exceeds {MAX_LINE_BYTES} bytes")
    return commands


def export_commands(filename: str | Path, output: str | Path, day: int | None = None) -> list[str]:
    commands = commands_for(load_deck(filename), day)
    Path(output).write_text("\n".join(commands) + "\n", encoding="ascii")
    return commands


def _open_serial(port: str, baudrate: int, timeout: float):
    try:
        import serial
    except ImportError as exc:
        raise DeckError("upload requires pyserial; install requirements.txt") from exc
    return serial.Serial(port=port, baudrate=baudrate, timeout=min(timeout, 0.25), write_timeout=timeout)


def _read_reply(serial_port, command: str, expected: str, timeout: float) -> str:
    import time
    deadline = time.monotonic() + timeout
    shown = command if len(command) <= 48 else command[:48] + "..."
    while time.monotonic() < deadline:
        raw = serial_port.readline()
        if not raw:
            continue
        try:
            reply = raw.decode("ascii", errors="strict").strip()
        except UnicodeDecodeError:
            continue
        if reply.startswith("ERR "):
            raise DeckError(f"device error to {shown}: {reply[4:]}")
        if expected == "INFO":
            match = re.fullmatch(r"OK INFO version=(\d+) cards=(\d+) day=(\d+) cap=(\d+)", reply)
            if match:
                if int(match.group(1)) != PROTOCOL_VERSION:
                    raise DeckError(f"device protocol version {match.group(1)} is unsupported")
                return reply
        elif expected == "BEGIN":
            if re.fullmatch(r"OK BEGIN resume=\d+", reply):
                return reply
        elif expected == "CHUNK":
            if re.fullmatch(r"OK CHUNK \d+", reply):
                return reply
        elif expected == "RESUME":
            if re.fullmatch(r"OK RESUME offset=\d+ count=\d+ crc=\d+ length=\d+", reply):
                return reply
        elif expected in ("EXPORT", "XRESUME"):
            if re.fullmatch(
                r"OK (?:EXPORT|XRESUME) cards=\d+ revlog=\d+ crc=\d+ length=\d+ "
                r"epoch=\d+ dropped=\d+ now=\d+ offset=\d+",
                reply,
            ):
                return reply
        elif expected == "XCHUNK":
            if re.fullmatch(r"OK XCHUNK \d+ [0-9a-fA-F]*", reply):
                return reply
        elif reply == expected:
            return reply
    raise DeckError(f"timeout waiting for {expected} acknowledgment to {shown}")


def _resume_from(reply: str) -> tuple[int, int, int, int]:
    match = re.fullmatch(r"OK RESUME offset=(\d+) count=(\d+) crc=(\d+) length=(\d+)", reply)
    if match is None:
        raise DeckError(f"unrecognized resume reply: {reply}")
    return tuple(int(group) for group in match.groups())  # type: ignore[return-value]


def upload(filename: str | Path, port: str, *, day: int | None = None,
           command_timeout: float = 5.0, commit_timeout: float = 120.0, baudrate: int = 115200,
           serial_factory: Callable[..., object] | None = None) -> None:
    if command_timeout <= 0 or commit_timeout <= 0:
        raise DeckError("command timeout must be positive")
    cards = load_deck(filename)
    payload = payload_for(cards)
    crc = zlib.crc32(payload) & UINT32_MAX
    factory = serial_factory or _open_serial
    print("Replacing the device collection; existing cards and review history will be reset.", file=sys.stderr)
    serial_port = factory(port, baudrate, command_timeout) if serial_factory is None else factory(port, baudrate=baudrate, timeout=max(command_timeout, commit_timeout))
    transaction_started = False
    try:
        _exchange(serial_port, "INFO", "INFO", command_timeout)
        if day is not None:
            if not isinstance(day, int) or isinstance(day, bool) or not 0 <= day <= UINT32_MAX // 1440:
                raise DeckError("day must be in range 0..2982616")
            _exchange(serial_port, f"DAY {day}", "OK DAY", command_timeout)
        begin = f"BEGIN {len(cards)} {crc} {len(payload)}"
        transaction_started = True
        begin_reply = _exchange(serial_port, begin, "BEGIN", command_timeout)
        begin_match = re.fullmatch(r"OK BEGIN resume=(\d+)", begin_reply)
        if begin_match is None:
            raise DeckError(f"unrecognized begin reply: {begin_reply}")
        offset = int(begin_match.group(1))
        if offset > len(payload):
            raise DeckError(f"device resume offset {offset} is past the payload")
        retries = 0
        while offset < len(payload):
            chunk = payload[offset:offset + MAX_CHUNK_BYTES]
            command = f"CHUNK {offset} {chunk.hex()}"
            try:
                chunk_reply = _exchange(serial_port, command, "CHUNK", command_timeout)
            except DeckError as exc:
                try:
                    resume_reply = _exchange(serial_port, "RESUME", "RESUME", command_timeout)
                except DeckError:
                    raise exc from None
                resumed_offset, resumed_count, resumed_crc, resumed_length = _resume_from(resume_reply)
                if (resumed_count, resumed_crc, resumed_length) != (len(cards), crc, len(payload)):
                    raise DeckError("device resume metadata does not match this deck") from None
                if resumed_offset < offset or resumed_offset > len(payload):
                    raise DeckError(f"device resume offset {resumed_offset} cannot continue this upload") from None
                retries += 1
                if retries > 8:
                    raise DeckError("upload did not advance after resume") from exc
                offset = resumed_offset
                continue
            accepted_match = re.fullmatch(r"OK CHUNK (\d+)", chunk_reply)
            if accepted_match is None:
                raise DeckError(f"unrecognized chunk reply: {chunk_reply}")
            accepted = int(accepted_match.group(1))
            if accepted < offset + len(chunk):
                raise DeckError(f"device accepted {accepted}, short of {offset + len(chunk)}")
            offset = accepted
        _exchange(serial_port, "COMMIT", "OK COMMIT", commit_timeout)
        transaction_started = False
        print(f"Imported {len(cards)} cards across {len({card.deck for card in cards})} decks.")
    except DeckError:
        if transaction_started:
            try:
                _exchange(serial_port, "ABORT", "OK ABORT", command_timeout)
            except DeckError:
                pass
        raise
    finally:
        serial_port.close()


def _exchange(serial_port, command: str, expected: str, timeout: float) -> str:
    payload = (command + "\n").encode("ascii")
    if len(payload) - 1 > MAX_LINE_BYTES:
        raise DeckError(f"command exceeds {MAX_LINE_BYTES} bytes")
    written = serial_port.write(payload)
    if written is not None and written != len(payload):
        raise DeckError(f"short write for {command}")
    flush = getattr(serial_port, "flush", None)
    if flush:
        flush()
    return _read_reply(serial_port, command, expected, timeout)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Inspect, convert, and upload bounded AI Passport Anki decks.")
    subparsers = parser.add_subparsers(dest="action", required=True)
    for action in ("inspect", "commands", "upload"):
        sub = subparsers.add_parser(action)
        sub.add_argument("input", type=Path)
        sub.add_argument("--map", type=Path, help="write device id to Anki card id JSON; this file is user data")
        if action == "commands":
            sub.add_argument("--output", required=True, type=Path)
            sub.add_argument("--day", type=int)
        elif action == "upload":
            sub.add_argument("--port", required=True)
            sub.add_argument("--baudrate", type=int, default=115200)
            sub.add_argument("--timeout", type=float, default=5.0)
            sub.add_argument("--commit-timeout", type=float, default=120.0)
            sub.add_argument("--day", type=int)
            sub.description = "Upload replaces all device cards and resets their review history."
    decks = subparsers.add_parser("anki-decks", help="list decks in a local Anki collection")
    pull = subparsers.add_parser("anki-pull", help="read Basic/JLPT cards from local Anki and write a TSV")
    sync = subparsers.add_parser("sync-back", help="replay device grades through AnkiConnect answerCards")
    for sub in (decks, pull, sync):
        sub.add_argument("--url", default=None, help="AnkiConnect URL, default http://127.0.0.1:8765 when no collection is given")
        sub.add_argument("--api-key", default=None, help="AnkiConnect API key; this is not an AnkiWeb password")
    decks.add_argument("--collection", type=Path, help="read-only collection.anki2, collection.anki21, or collection.anki21b")
    pull.add_argument("--deck", action="append", required=True, help="deck name; repeat to select more than one")
    pull.add_argument("--output", required=True, type=Path)
    pull.add_argument("--map", type=Path, help="device id to Anki card id JSON; user data, not for git")
    pull.add_argument("--collection", type=Path, help="read-only collection.anki2, collection.anki21, or collection.anki21b")
    pull.add_argument("--upload", action="store_true", help="upload the TSV over the v2 chunked USB protocol")
    pull.add_argument("--port")
    pull.add_argument("--baudrate", type=int, default=115200)
    pull.add_argument("--timeout", type=float, default=5.0)
    pull.add_argument("--commit-timeout", type=float, default=120.0)
    pull.add_argument("--day", type=int)
    sync.add_argument("--map", required=True, type=Path)
    sync.add_argument("--port", required=True)
    sync.add_argument("--receipt", type=Path, help="idempotency receipt; default is the map path plus .receipt.json")
    sync.add_argument("--dry-run", action="store_true", help="print the grades that would be sent and do not call Anki")
    sync.add_argument("--baudrate", type=int, default=115200)
    sync.add_argument("--timeout", type=float, default=5.0)
    return parser


def _anki_sync():
    import importlib.util
    name = "anki_sync"
    loaded = sys.modules.get(name)
    if loaded is not None:
        return loaded
    path = Path(__file__).with_name("anki_sync.py")
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise DeckError("anki_sync.py is missing")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def main(argv: Sequence[str] | None = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    try:
        if args.action == "inspect":
            cards = load_deck(args.input)
            front_max = max(len(card.front.encode("utf-8")) for card in cards)
            back_max = max(len(card.back.encode("utf-8")) for card in cards)
            print(f"Valid: {len(cards)} cards, {len({card.deck for card in cards})} decks, "
                  f"max front {front_max} bytes, max back {back_max} bytes")
            print("Upload replaces the device collection and resets review history.")
            if args.map:
                _write_map(cards, args.map)
        elif args.action == "commands":
            cards = load_deck(args.input)
            lines = export_commands(args.input, args.output, args.day)
            print(f"Wrote {len(lines)} protocol commands to {args.output}")
            print("Running these commands replaces the device collection and resets review history.")
            if args.map:
                _write_map(cards, args.map)
        elif args.action == "upload":
            upload(args.input, args.port, day=args.day, command_timeout=args.timeout,
                   commit_timeout=args.commit_timeout, baudrate=args.baudrate)
            if args.map:
                _write_map(load_deck(args.input), args.map)
        elif args.action == "anki-decks":
            names = _anki_sync().list_decks(sys.modules[__name__], url=args.url, collection=args.collection,
                                            api_key=args.api_key)
            for name in names:
                print(name)
        elif args.action == "anki-pull":
            if args.upload and not args.port:
                raise DeckError("anki-pull --upload requires --port")
            cards, source, imported_at_ms = _anki_sync().pull_decks(
                sys.modules[__name__], args.deck, url=args.url, collection=args.collection,
                api_key=args.api_key, map_path=args.map,
            )
            write_tsv(cards, args.output)
            print(f"Wrote {len(cards)} cards to {args.output}")
            if args.map:
                write_map(cards, args.map, source=source, imported_at_ms=imported_at_ms, decks=list(args.deck))
                print(f"Wrote id map to {args.map}")
            if args.upload:
                upload(args.output, args.port, day=args.day, command_timeout=args.timeout,
                       commit_timeout=args.commit_timeout, baudrate=args.baudrate)
        else:
            result = _anki_sync().sync_back(
                sys.modules[__name__], port=args.port, url=args.url or "http://127.0.0.1:8765",
                api_key=args.api_key, map_path=args.map, receipt_path=args.receipt, dry_run=args.dry_run,
                baudrate=args.baudrate, timeout=args.timeout,
            )
            if result.conflicts or result.dropped:
                return 2
    except (DeckError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
