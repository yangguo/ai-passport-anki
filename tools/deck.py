#!/usr/bin/env python3
"""Validate AI Passport Anki decks and upload them over USB Serial/JTAG."""
from __future__ import annotations

import argparse
import csv
import html.parser
import json
import re
import sqlite3
import sys
import tempfile
import zipfile
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Callable, Sequence

MAX_CARDS = 64
MAX_DECK_BYTES = 47
MAX_TEXT_BYTES = 255
MAX_LINE_BYTES = 1200
UINT32_MAX = 0xFFFFFFFF
PROTOCOL_VERSION = 1
APKG_DB_MAX_BYTES = 16 * 1024 * 1024
FONT_MANIFEST = Path(__file__).resolve().parents[1] / "assets/fonts/pa_font_16.manifest.json"


class DeckError(Exception):
    """Invalid input deck or failed device exchange."""


@dataclass(frozen=True)
class Card:
    id: int
    deck: str
    front: str
    back: str


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


def _strip_html(source: str) -> str:
    parser = _PlainText()
    try:
        parser.feed(source)
        parser.close()
    except Exception as exc:
        raise DeckError(f"invalid HTML field: {exc}") from exc
    return "".join(parser.parts).strip()


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
        if not (0x20 <= codepoint <= 0x7E or 0x00A0 <= codepoint <= 0x024F
                or 0x3000 <= codepoint <= 0x303F or 0x4E00 <= codepoint <= 0x9FFF
                or 0xFF00 <= codepoint <= 0xFFEF):
            raise DeckError(f"{where}: unsupported character U+{codepoint:04X}")
        if not _font_contains(codepoint):
            raise DeckError(f"{where}: no pa_font_16 glyph for U+{codepoint:04X}")


def _validate_card(card: Card, row: int) -> Card:
    card = Card(card.id, card.deck.replace("\r\n", "\n").replace("\r", "\n"),
                card.front.replace("\r\n", "\n").replace("\r", "\n"),
                card.back.replace("\r\n", "\n").replace("\r", "\n"))
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
        raise DeckError(f"deck exceeds {MAX_CARDS} cards")
    seen: set[int] = set()
    normalized: list[Card] = []
    for row, card in enumerate(cards, 1):
        card = _validate_card(card, row)
        if card.id in seen:
            raise DeckError(f"row {row}: duplicate id {card.id}")
        seen.add(card.id)
        normalized.append(card)
    if len({card.deck for card in normalized}) > MAX_CARDS:
        raise DeckError(f"deck exceeds {MAX_CARDS} distinct decks")
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
            values = [_strip_html(value) for value in fields[2:4]] if html_export else fields[2:4]
            cards.append(Card(card_id, fields[1], values[0], values[1]))
    else:
        # Anki text exports commonly contain two fields and no column header.
        if len(rows[0]) != 2:
            raise DeckError("expected header id<TAB>deck<TAB>front<TAB>back or a two-field Anki text export")
        for index, fields in enumerate(rows, 1):
            if len(fields) != 2:
                raise DeckError(f"row {index}: expected 2 Anki text fields, got {len(fields)}")
            values = [_strip_html(value) for value in fields] if html_export else fields
            cards.append(Card(index, "Imported", values[0], values[1]))
    return _finish(cards)


def _apkg_database(path: Path) -> bytes:
    try:
        archive = zipfile.ZipFile(path, "r")
    except (OSError, zipfile.BadZipFile) as exc:
        raise DeckError(f"invalid APKG archive: {exc}") from exc
    with archive:
        names = set(archive.namelist())
        if "collection.anki21b" in names:
            raise DeckError("collection.anki21b compressed APKG is unsupported; export UTF-8 TSV from Anki")
        member = next((name for name in ("collection.anki2", "collection.anki21") if name in names), None)
        if member is None:
            raise DeckError("APKG must contain collection.anki2 or collection.anki21")
        info = archive.getinfo(member)
        if info.file_size > APKG_DB_MAX_BYTES:
            raise DeckError(f"APKG collection exceeds {APKG_DB_MAX_BYTES} byte conversion limit")
        try:
            return archive.read(member)
        except (OSError, zipfile.BadZipFile, RuntimeError) as exc:
            raise DeckError(f"cannot read APKG collection: {exc}") from exc


def _read_apkg(path: Path) -> list[Card]:
    database = _apkg_database(path)
    if len(database) > APKG_DB_MAX_BYTES:
        raise DeckError("APKG collection exceeds conversion limit")
    try:
        with tempfile.TemporaryDirectory(prefix="passport-anki-") as directory:
            db_path = Path(directory) / "collection.sqlite"
            db_path.write_bytes(database)
            connection = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
            connection.row_factory = sqlite3.Row
            try:
                collection = connection.execute("SELECT models, decks FROM col LIMIT 1").fetchone()
                if collection is None:
                    raise DeckError("APKG collection has no metadata")
                models = json.loads(collection["models"])
                decks = json.loads(collection["decks"])
                notes = {int(row["id"]): row for row in connection.execute("SELECT id, mid, flds FROM notes")}
                model_by_id = {int(key): model for key, model in models.items()}
                cards: list[Card] = []
                seen_notes: set[int] = set()
                query = "SELECT id AS cid, nid, did FROM cards ORDER BY id LIMIT ?"
                for index, row in enumerate(connection.execute(query, (MAX_CARDS + 1,)), 1):
                    note_id = int(row["nid"])
                    if note_id in seen_notes:
                        raise DeckError("multiple APKG cards from one note are unsupported")
                    seen_notes.add(note_id)
                    note = notes.get(note_id)
                    if note is None:
                        raise DeckError(f"APKG card {row['cid']} references a missing note")
                    model = model_by_id.get(int(note["mid"]))
                    if not model:
                        raise DeckError(f"APKG note {note_id} references a missing model")
                    name = str(model.get("name", ""))
                    kind = int(model.get("type", -1))
                    if kind == 1 or name.lower() == "cloze":
                        raise DeckError("Cloze APKG notes are unsupported")
                    if kind != 0 or name != "Basic":
                        raise DeckError(f"custom APKG model {name!r} is unsupported; only Basic is converted")
                    fields = str(note["flds"]).split("\x1f")
                    if len(fields) < 2:
                        raise DeckError(f"Basic APKG note {note_id} must have at least two text fields")
                    deck_obj = decks.get(str(row["did"]))
                    if not deck_obj:
                        raise DeckError(f"APKG card {row['cid']} references a missing deck")
                    cards.append(Card(index, str(deck_obj.get("name", "")),
                                      _strip_html(fields[0]), _strip_html(fields[1])))
                if connection.execute("SELECT COUNT(*) FROM cards").fetchone()[0] > MAX_CARDS:
                    raise DeckError(f"APKG exceeds {MAX_CARDS} cards")
            finally:
                connection.close()
    except DeckError:
        raise
    except (OSError, sqlite3.Error, ValueError, TypeError, KeyError, json.JSONDecodeError) as exc:
        raise DeckError(f"unsupported or invalid legacy APKG collection: {exc}") from exc
    return _finish(cards)


def load_deck(filename: str | Path) -> list[Card]:
    path = Path(filename)
    suffix = path.suffix.lower()
    if suffix == ".apkg":
        return _read_apkg(path)
    if suffix in (".tsv", ".txt", ".anki"):
        return _read_tsv(path)
    raise DeckError("input must be a UTF-8 TSV/Anki text export or legacy .apkg")


def _hex(value: str) -> str:
    return value.encode("utf-8").hex()


def commands_for(cards: Sequence[Card], day: int | None = None) -> list[str]:
    _finish(list(cards))
    commands = []
    if day is not None:
        if not isinstance(day, int) or isinstance(day, bool) or not 0 <= day <= UINT32_MAX // 1440:
            raise DeckError("day must be in range 0..2982616")
        commands.append(f"DAY {day}")
    commands.append(f"BEGIN {len(cards)}")
    commands.extend(f"CARD {card.id} {_hex(card.deck)} {_hex(card.front)} {_hex(card.back)}" for card in cards)
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
    while time.monotonic() < deadline:
        raw = serial_port.readline()
        if not raw:
            continue
        try:
            reply = raw.decode("ascii", errors="strict").strip()
        except UnicodeDecodeError:
            continue
        if reply.startswith("ERR "):
            raise DeckError(f"device error to {command}: {reply[4:]}")
        if expected == "INFO":
            match = re.fullmatch(r"OK INFO version=(\d+) cards=(\d+) day=(\d+)", reply)
            if match:
                if int(match.group(1)) != PROTOCOL_VERSION:
                    raise DeckError(f"device protocol version {match.group(1)} is unsupported")
                return reply
        elif reply == expected:
            return reply
    raise DeckError(f"timeout waiting for {expected} acknowledgment to {command}")


def upload(filename: str | Path, port: str, *, day: int | None = None,
           command_timeout: float = 5.0, baudrate: int = 115200,
           serial_factory: Callable[..., object] | None = None) -> None:
    if command_timeout <= 0:
        raise DeckError("command timeout must be positive")
    cards = load_deck(filename)
    lines = commands_for(cards, day)
    factory = serial_factory or _open_serial
    print("Replacing the device collection; existing cards and review history will be reset.", file=sys.stderr)
    serial_port = factory(port, baudrate, command_timeout) if serial_factory is None else factory(port, baudrate=baudrate, timeout=command_timeout)
    transaction_started = False
    try:
        _exchange(serial_port, "INFO", "INFO", command_timeout)
        if day is not None:
            _exchange(serial_port, lines[0], "OK DAY", command_timeout)
            lines = lines[1:]
        begin = lines[0]
        transaction_started = True
        _exchange(serial_port, begin, "OK BEGIN", command_timeout)
        for command in lines[1:-1]:
            _exchange(serial_port, command, "OK CARD", command_timeout)
        _exchange(serial_port, "COMMIT", "OK COMMIT", command_timeout)
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
        if action == "commands":
            sub.add_argument("--output", required=True, type=Path)
            sub.add_argument("--day", type=int)
        elif action == "upload":
            sub.add_argument("--port", required=True)
            sub.add_argument("--baudrate", type=int, default=115200)
            sub.add_argument("--timeout", type=float, default=5.0)
            sub.add_argument("--day", type=int)
            sub.description = "Upload replaces all device cards and resets their review history."
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    try:
        if args.action == "inspect":
            cards = load_deck(args.input)
            print(f"Valid: {len(cards)} cards, {len({card.deck for card in cards})} decks")
            print("Upload replaces the device collection and resets review history.")
        elif args.action == "commands":
            lines = export_commands(args.input, args.output, args.day)
            print(f"Wrote {len(lines)} protocol commands to {args.output}")
            print("Running these commands replaces the device collection and resets review history.")
        else:
            upload(args.input, args.port, day=args.day, command_timeout=args.timeout, baudrate=args.baudrate)
    except (DeckError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
