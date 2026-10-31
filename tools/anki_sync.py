"""Host-side Anki pull and review write-back.

The device stays offline. This module talks to AnkiConnect on localhost, or reads a
copy of collection.anki2 / collection.anki21 / collection.anki21b. It never logs into
AnkiWeb and never writes device-computed intervals into Anki.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import sqlite3
import struct
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zlib
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_URL = "http://127.0.0.1:8765"
_EXPORT_REPLY = re.compile(
    r"OK (?:EXPORT|XRESUME) cards=(?P<cards>\d+) revlog=(?P<revlog>\d+) crc=(?P<crc>\d+) "
    r"length=(?P<length>\d+) epoch=(?P<epoch>\d+) dropped=(?P<dropped>\d+) now=(?P<now>\d+) "
    r"offset=(?P<offset>\d+)"
)
_XCHUNK_REPLY = re.compile(r"OK XCHUNK (?P<end>\d+) (?P<hex>[0-9a-fA-F]*)")
_RATINGS = ("Again", "Hard", "Good", "Easy")


@dataclass
class SyncResult:
    applied: int = 0
    already: int = 0
    planned: int = 0
    conflicts: list[str] = field(default_factory=list)
    dropped: int = 0
    dry_run: bool = False


def now_ms() -> int:
    return int(time.time() * 1000)


def anki_desktop_running() -> bool:
    """True when a process comm is the official Anki desktop binary."""
    proc = Path("/proc")
    if not proc.is_dir():
        return False
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            comm = (entry / "comm").read_text(encoding="utf-8", errors="replace").strip()
        except OSError:
            continue
        if comm in {"anki", "Anki"}:
            return True
    return False


def invoke_anki(deck, url: str, action: str, params: dict | None = None, api_key: str | None = None,
                timeout: float = 30.0):
    body: dict = {"action": action, "version": 6, "params": params or {}}
    if api_key:
        body["key"] = api_key
    request = urllib.request.Request(
        url, data=json.dumps(body).encode("utf-8"), headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except (OSError, urllib.error.URLError, json.JSONDecodeError, TimeoutError, UnicodeError) as exc:
        raise deck.DeckError(f"AnkiConnect {action} failed: {exc}") from exc
    if not isinstance(payload, dict):
        raise deck.DeckError(f"AnkiConnect {action} returned a non-object")
    if payload.get("error"):
        raise deck.DeckError(f"AnkiConnect {action}: {payload['error']}")
    return payload.get("result")


def _open_collection_copy(deck, path: Path):
    is_21b = path.suffix == ".anki21b"
    if not is_21b and path.suffix not in {".anki2", ".anki21"}:
        raise deck.DeckError("collection must be collection.anki2, collection.anki21, or collection.anki21b")
    if not path.is_file():
        raise deck.DeckError(f"collection not found: {path}")
    if anki_desktop_running():
        print(
            "warning: Anki appears to be running. This read-only copy can miss data still in the "
            "write-ahead log. Quit Anki, or use AnkiConnect at 127.0.0.1:8765.",
            file=sys.stderr,
        )
    temp = tempfile.TemporaryDirectory(prefix="passport-anki-col-")
    dest = Path(temp.name) / path.name
    try:
        if is_21b:
            if path.stat().st_size > deck.APKG_DB_MAX_BYTES:
                raise deck.DeckError(
                    f"collection exceeds {deck.APKG_DB_MAX_BYTES} byte conversion limit"
                )
            sqlite_dest = dest.with_suffix(".sqlite")
            sqlite_dest.write_bytes(deck._decompress_anki21b(path.read_bytes(), path.name))
            dest = sqlite_dest
        else:
            shutil.copy2(path, dest)
            for suffix in ("-wal", "-shm"):
                extra = Path(str(path) + suffix)
                if extra.is_file():
                    shutil.copy2(extra, Path(temp.name) / extra.name)
        connection = sqlite3.connect(f"file:{dest}?mode=ro", uri=True)
    except (OSError, sqlite3.Error, deck.DeckError) as exc:
        temp.cleanup()
        if isinstance(exc, deck.DeckError):
            raise
        raise deck.DeckError(f"cannot open a read-only copy of the collection: {exc}") from exc
    connection.row_factory = sqlite3.Row
    return connection, temp


def _deck_names_from_connection(deck, connection: sqlite3.Connection) -> list[str]:
    tables = deck._table_names(connection)
    if {"notetypes", "fields", "decks"} <= tables:
        try:
            return sorted({deck._leaf_deck_name(str(row["name"])) for row in connection.execute("SELECT name FROM decks ORDER BY name")})
        except sqlite3.Error as exc:
            raise deck.DeckError(f"collection schema cannot list decks: {exc}") from exc
    if "col" not in tables:
        raise deck.DeckError("collection schema has neither decks nor col.decks")
    try:
        row = connection.execute("SELECT decks FROM col LIMIT 1").fetchone()
    except sqlite3.Error as exc:
        raise deck.DeckError(f"collection schema cannot list decks: {exc}") from exc
    if row is None:
        raise deck.DeckError("collection has no metadata row")
    try:
        payload = json.loads(row["decks"])
    except (TypeError, json.JSONDecodeError) as exc:
        raise deck.DeckError(f"collection deck list is not JSON: {exc}") from exc
    return sorted({deck._leaf_deck_name(str(value.get("name", ""))) for value in payload.values() if isinstance(value, dict)})


def list_decks(deck, *, url: str | None, collection: Path | None, api_key: str | None) -> list[str]:
    if collection is not None and url is None:
        connection, temp = _open_collection_copy(deck, collection)
        try:
            return _deck_names_from_connection(deck, connection)
        finally:
            connection.close()
            temp.cleanup()
    target = url or DEFAULT_URL
    try:
        names = invoke_anki(deck, target, "deckNames", api_key=api_key)
    except deck.DeckError:
        if collection is None:
            raise
        print(f"warning: AnkiConnect at {target} is unavailable; listing {collection}", file=sys.stderr)
        connection, temp = _open_collection_copy(deck, collection)
        try:
            return _deck_names_from_connection(deck, connection)
        finally:
            connection.close()
            temp.cleanup()
    if not isinstance(names, list) or not all(isinstance(name, str) for name in names):
        raise deck.DeckError("AnkiConnect deckNames did not return a list of names")
    return sorted({deck._leaf_deck_name(str(name)) for name in names})


def _load_map(path: Path | None) -> dict | None:
    if path is None or not path.is_file():
        return None
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(exc) from exc
    if isinstance(document, dict) and "cards" in document and isinstance(document["cards"], dict):
        return document
    if isinstance(document, dict) and all(isinstance(value, str) for value in document.values()):
        return {
            "version": 1,
            "imported_at_ms": None,
            "cards": {
                key: {"anki_card_id": value, "anki_note_id": "", "deck": ""}
                for key, value in document.items()
            },
        }
    raise RuntimeError("map file is not a device-id object")


def _reassign(deck, cards: list, existing: dict | None) -> list:
    used: set[int] = set()
    by_anki: dict[str, int] = {}
    if existing is not None:
        for device_text, entry in existing["cards"].items():
            if not str(device_text).isdigit():
                raise deck.DeckError(f"map device id {device_text!r} is not an integer")
            device_id = int(device_text)
            anki_id = entry["anki_card_id"] if isinstance(entry, dict) else str(entry)
            by_anki[str(anki_id)] = device_id
            used.add(device_id)
    next_id = 1
    assigned = []
    for card in sorted(cards, key=lambda item: int(item.anki_id)):
        device_id = by_anki.get(card.anki_id)
        if device_id is None:
            while next_id in used:
                next_id += 1
            device_id = next_id
            used.add(device_id)
            next_id += 1
        assigned.append(deck.Card(device_id, card.deck, card.front, card.back, card.anki_id, card.anki_note_id))
    return deck._finish(assigned)


def _escape_deck(name: str) -> str:
    return name.replace("\\", "\\\\").replace('"', '\\"')


def _card_from_info(deck, info: dict):
    try:
        model_name = str(info["modelName"])
        ordered = sorted(info["fields"].items(), key=lambda item: int(item[1]["order"]))
        names = [name for name, _meta in ordered]
        values = [str(meta["value"]) for _name, meta in ordered]
        kind = 1 if "cloze" in model_name.lower() else 0
        model = {"name": model_name, "type": kind, "flds": [{"name": name} for name in names]}
        leaf = deck._leaf_deck_name(str(info["deckName"]))
        return deck.card_from_note(model, values, leaf, int(info["note"]), int(info["cardId"]), 1)
    except (KeyError, TypeError, ValueError) as exc:
        raise deck.DeckError(f"AnkiConnect cardsInfo is missing fields: {exc}") from exc


def _pull_connect(deck, names: list[str], url: str, api_key: str | None, existing: dict | None) -> list:
    known = invoke_anki(deck, url, "deckNames", api_key=api_key)
    if not isinstance(known, list):
        raise deck.DeckError("AnkiConnect deckNames did not return a list of names")
    leaf_to_full: dict[str, list[str]] = {}
    for full in known:
        leaf_to_full.setdefault(deck._leaf_deck_name(str(full)), []).append(str(full))
    wanted = {deck._leaf_deck_name(str(name)) for name in names}
    missing = [name for name in names if deck._leaf_deck_name(str(name)) not in leaf_to_full]
    if missing:
        raise deck.DeckError(f"deck {missing[0]!r} is not in the open Anki collection")
    found = []
    seen: set[int] = set()
    for leaf in sorted(wanted):
        for full in leaf_to_full[leaf]:
            card_ids = invoke_anki(deck, url, "findCards", {"query": f'deck:"{_escape_deck(full)}"'}, api_key)
            if not isinstance(card_ids, list):
                raise deck.DeckError(f"AnkiConnect findCards failed for {full}")
            for start in range(0, len(card_ids), 100):
                batch = card_ids[start:start + 100]
                infos = invoke_anki(deck, url, "cardsInfo", {"cards": batch}, api_key)
                if not isinstance(infos, list):
                    raise deck.DeckError("AnkiConnect cardsInfo did not return a list")
                for info in infos:
                    if deck._leaf_deck_name(str(info.get("deckName"))) not in wanted:
                        continue
                    card_id = int(info["cardId"])
                    if card_id in seen:
                        continue
                    seen.add(card_id)
                    found.append(_card_from_info(deck, info))
    if not found:
        raise deck.DeckError("deck has no cards")
    return _reassign(deck, found, existing)


def _pull_collection(deck, names: list[str], path: Path, existing: dict | None) -> list:
    connection, temp = _open_collection_copy(deck, path)
    try:
        known = set(_deck_names_from_connection(deck, connection))
        wanted = {deck._leaf_deck_name(str(name)) for name in names}
        missing = [name for name in names if deck._leaf_deck_name(str(name)) not in known]
        if missing:
            raise deck.DeckError(f"deck {missing[0]!r} is not in the collection")
        return _reassign(deck, deck.read_sqlite_collection(connection, wanted), existing)
    finally:
        connection.close()
        temp.cleanup()


def pull_decks(deck, names: list[str], *, url: str | None, collection: Path | None, api_key: str | None,
               map_path: Path | None):
    if not names:
        raise deck.DeckError("select at least one deck")
    try:
        existing = _load_map(map_path)
    except RuntimeError as exc:
        raise deck.DeckError(f"cannot read map: {exc}") from exc
    imported_at_ms = now_ms()
    if collection is not None and url is None:
        return _pull_collection(deck, names, collection, existing), "collection", imported_at_ms
    target = url or DEFAULT_URL
    try:
        cards = _pull_connect(deck, names, target, api_key, existing)
    except deck.DeckError:
        if collection is None:
            raise
        print(f"warning: AnkiConnect at {target} is unavailable; reading {collection}", file=sys.stderr)
        return _pull_collection(deck, names, collection, existing), "collection", imported_at_ms
    return cards, "anki-connect", imported_at_ms


def _parse_meta(reply: str) -> dict[str, int]:
    match = _EXPORT_REPLY.fullmatch(reply.strip())
    if match is None:
        raise RuntimeError(reply)
    return {key: int(value) for key, value in match.groupdict().items()}


def fetch_export(deck, serial_port, timeout: float) -> tuple[bytes, dict[str, int]]:
    """Read the device export, resuming after a lost chunk reply. The image is checked with CRC."""
    meta = _parse_meta(deck._exchange(serial_port, "EXPORT", "EXPORT", timeout))
    blob = bytearray()
    resumes = 0
    while len(blob) < meta["length"]:
        command = f"XCHUNK {len(blob)}"
        try:
            reply = deck._exchange(serial_port, command, "XCHUNK", timeout)
        except deck.DeckError as exc:
            if not str(exc).startswith("timeout"):
                raise
            resumes += 1
            if resumes > 8:
                raise deck.DeckError("export did not advance after resume") from exc
            try:
                resumed = _parse_meta(deck._exchange(serial_port, "XRESUME", "XRESUME", timeout))
            except deck.DeckError:
                raise exc from None
            if (resumed["crc"], resumed["length"], resumed["epoch"], resumed["revlog"]) != (
                meta["crc"], meta["length"], meta["epoch"], meta["revlog"],
            ):
                raise deck.DeckError("export resume metadata does not match this snapshot") from None
            continue
        match = _XCHUNK_REPLY.fullmatch(reply.strip())
        if match is None:
            raise deck.DeckError(f"unrecognized export chunk reply: {reply}")
        data = bytes.fromhex(match.group("hex"))
        end = int(match.group("end"))
        if not data or end != len(blob) + len(data) or end > meta["length"]:
            raise deck.DeckError(f"device export chunk at {len(blob)} is not contiguous")
        blob += data
        resumes = 0
    if (zlib.crc32(blob) & 0xFFFFFFFF) != meta["crc"]:
        raise deck.DeckError("export CRC does not match")
    return bytes(blob), meta


def parse_export(deck, blob: bytes) -> dict:
    if len(blob) < 32 or blob[:4] != b"PREV":
        raise deck.DeckError("export image is not a PREV snapshot")
    version, header_len = struct.unpack_from("<HH", blob, 4)
    epoch, now = struct.unpack_from("<II", blob, 8)
    cards, sched_len = struct.unpack_from("<HH", blob, 16)
    revlog, dropped, flags = struct.unpack_from("<III", blob, 20)
    if version != 1 or header_len != 32 or sched_len != 20 or flags != 0:
        raise deck.DeckError("export header is an unsupported PREV version")
    expect = 32 + cards * 20 + revlog * 8
    if len(blob) != expect:
        raise deck.DeckError("export length does not match the header")
    schedule = []
    for index in range(cards):
        offset = 32 + index * 20
        due, interval, ease, reviews, lapses, stage, step, _deck_id, pad, card_id = struct.unpack_from(
            "<IHHHHBBBBI", blob, offset,
        )
        if pad != 0 or card_id == 0:
            raise deck.DeckError(f"schedule record {index} is invalid")
        schedule.append({"index": index, "id": card_id, "due": due, "interval": interval, "ease": ease,
                         "reviews": reviews, "lapses": lapses, "stage": stage, "step": step})
    reviews_out = []
    base = 32 + cards * 20
    for number in range(revlog):
        index, rating, reserved, minute = struct.unpack_from("<HBBI", blob, base + number * 8)
        if reserved != 0 or rating > 3 or index >= cards:
            raise deck.DeckError(f"review record {number + 1} is invalid")
        reviews_out.append({
            "seq": number + 1,
            "index": index,
            "device_id": schedule[index]["id"],
            "rating": rating,
            "minute": minute,
        })
    return {"epoch": epoch, "now": now, "cards": cards, "revlog": revlog, "dropped": dropped,
            "schedule": schedule, "reviews": reviews_out}


def _atomic_json(path: Path, document: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as handle:
        handle.write(json.dumps(document, ensure_ascii=False, indent=2) + "\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def _load_receipt(path: Path, epoch: int) -> dict:
    if not path.is_file():
        return {"version": 1, "epoch": epoch, "applied": [], "pending": None}
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(exc) from exc
    if not isinstance(document, dict) or document.get("epoch") != epoch:
        return {"version": 1, "epoch": epoch, "applied": [], "pending": None}
    document.setdefault("applied", [])
    document.setdefault("pending", None)
    return document


def _reviews_of(deck, call, anki_id: str):
    try:
        result = call("getReviewsOfCards", {"cards": [int(anki_id)]})
    except deck.DeckError as exc:
        text = str(exc)
        if "getReviewsOfCards" in text:
            return None
        raise
    if not isinstance(result, dict):
        return None
    rows = result.get(str(anki_id))
    if rows is None:
        rows = result.get(anki_id)
    return rows if isinstance(rows, list) else []


def _conflict(deck, call, anki_id: str, imported_at_ms: int | None, receipt: dict) -> str | None:
    info = call("cardsInfo", {"cards": [int(anki_id)]})
    if not isinstance(info, list) or not info:
        return f"anki card {anki_id} was not found"
    card = info[0]
    mod = card.get("mod")
    reviews = _reviews_of(deck, call, anki_id)
    claimed = {
        int(item["revlog_id"]) for item in receipt["applied"]
        if str(item.get("anki_card_id")) == anki_id and item.get("revlog_id") is not None
    }
    if reviews is not None and imported_at_ms is not None:
        newer = [row for row in reviews if int(row["id"]) > imported_at_ms and int(row["id"]) not in claimed]
        if newer:
            return f"anki card {anki_id} has {len(newer)} review(s) after the pull that this tool did not write"
    ours = any(str(item.get("anki_card_id")) == anki_id for item in receipt["applied"])
    if mod is not None and imported_at_ms is not None and int(mod) > imported_at_ms // 1000 and not ours:
        return f"anki card {anki_id} was modified in Anki after the pull (mod={mod})"
    if mod is None and reviews is None:
        return f"anki card {anki_id}: AnkiConnect returned neither mod nor reviews, so it was not answered"
    return None


def _match_pending(reviews: list, pending: dict, claimed: set[int]) -> dict | None:
    floor = int(pending["started_ms"]) - 5000
    matches = [
        row for row in reviews
        if int(row.get("ease", -1)) == int(pending["ease"]) and int(row["id"]) >= floor
        and int(row["id"]) not in claimed
    ]
    if not matches:
        return None
    chosen = min(matches, key=lambda row: int(row["id"]))
    return {
        "seq": pending["seq"],
        "device_id": pending["device_id"],
        "anki_card_id": pending["anki_card_id"],
        "ease": pending["ease"],
        "revlog_id": int(chosen["id"]),
    }


def sync_back(deck, *, port: str, url: str, api_key: str | None, map_path: Path, receipt_path: Path | None,
              dry_run: bool, baudrate: int = 115200, timeout: float = 5.0, serial_factory=None,
              after_pending=None) -> SyncResult:
    """Replay new device grades through answerCards. Re-running skips grades already in the receipt."""
    try:
        mapping = _load_map(map_path)
    except RuntimeError as exc:
        raise deck.DeckError(f"cannot read map: {exc}") from exc
    if mapping is None:
        raise deck.DeckError(f"map file not found: {map_path}")
    if timeout <= 0:
        raise deck.DeckError("command timeout must be positive")
    receipt_file = receipt_path or map_path.with_suffix(".receipt.json")
    factory = serial_factory or deck._open_serial
    if serial_factory is None:
        serial_port = factory(port, baudrate, timeout)
    else:
        serial_port = factory(port, baudrate=baudrate, timeout=timeout)
    result = SyncResult(dry_run=dry_run)
    try:
        deck._exchange(serial_port, "INFO", "INFO", timeout)
        blob, meta = fetch_export(deck, serial_port, timeout)
    finally:
        serial_port.close()
    parsed = parse_export(deck, blob)
    if (parsed["cards"], parsed["revlog"], parsed["epoch"], parsed["dropped"]) != (
        meta["cards"], meta["revlog"], meta["epoch"], meta["dropped"],
    ):
        raise deck.DeckError("export header does not match the device reply")
    result.dropped = parsed["dropped"]
    if result.dropped:
        print(f"warning: the device dropped {result.dropped} grade(s) that are not in the review log",
              file=sys.stderr)
    try:
        receipt = _load_receipt(receipt_file, parsed["epoch"])
    except RuntimeError as exc:
        raise deck.DeckError(f"cannot read receipt: {exc}") from exc
    applied_seqs = {int(item["seq"]) for item in receipt["applied"]}
    result.already = sum(1 for review in parsed["reviews"] if review["seq"] in applied_seqs)

    def call(action: str, params: dict | None = None):
        return invoke_anki(deck, url, action, params, api_key)

    pending = receipt.get("pending")
    if pending and not dry_run:
        reviews = _reviews_of(deck, call, str(pending["anki_card_id"]))
        if reviews is None:
            raise deck.DeckError(
                f"grade seq {pending['seq']} may already have been sent; AnkiConnect getReviewsOfCards "
                "is required before retrying, so it was not sent again"
            )
        claimed = {int(item["revlog_id"]) for item in receipt["applied"] if item.get("revlog_id") is not None}
        matched = _match_pending(reviews, pending, claimed)
        if matched is not None:
            receipt["applied"].append(matched)
            receipt["pending"] = None
            _atomic_json(receipt_file, receipt)
            applied_seqs.add(int(matched["seq"]))
            result.already += 1
            print(f"recovered in-flight grade seq {matched['seq']} from Anki revlog {matched['revlog_id']}")

    print("Answer time is this computer's clock, not the device's logical minute. "
          "Device intervals are not written to Anki.")
    print(f"Device schedule records: {parsed['cards']}. Review log: {parsed['revlog']}.")
    conflicts_for: set[int] = set()
    checked: set[int] = set()
    imported_at_ms = mapping.get("imported_at_ms")
    if imported_at_ms is None:
        print("warning: map has no imported_at_ms; reviews made in Anki after the pull cannot be detected",
              file=sys.stderr)
    for review in parsed["reviews"]:
        if review["seq"] in applied_seqs:
            continue
        entry = mapping["cards"].get(str(review["device_id"]))
        if not isinstance(entry, dict) or "anki_card_id" not in entry:
            raise deck.DeckError(f"device id {review['device_id']} is not in the map")
        anki_id = str(entry["anki_card_id"])
        if review["device_id"] not in checked:
            checked.add(review["device_id"])
            reason = _conflict(deck, call, anki_id, imported_at_ms, receipt)
            if reason:
                conflicts_for.add(review["device_id"])
                result.conflicts.append(reason)
                print(f"conflict: {reason}", file=sys.stderr)
        if review["device_id"] in conflicts_for:
            continue
        ease = int(review["rating"]) + 1
        result.planned += 1
        label = _RATINGS[review["rating"]]
        print(f"device {review['device_id']} anki {anki_id} ease {ease} ({label}) "
              f"logical minute {review['minute']} seq {review['seq']}")
        if dry_run:
            continue
        pending_row = {
            "seq": review["seq"],
            "device_id": review["device_id"],
            "anki_card_id": anki_id,
            "ease": ease,
            "started_ms": now_ms(),
        }
        receipt["pending"] = pending_row
        _atomic_json(receipt_file, receipt)
        if after_pending is not None:
            after_pending()
        answered = call("answerCards", {"answers": [{"cardId": int(anki_id), "ease": ease}]})
        if answered != [True]:
            raise deck.DeckError(f"AnkiConnect answerCards did not accept anki card {anki_id}")
        revlog_id = None
        reviews = _reviews_of(deck, call, anki_id)
        if reviews:
            claimed = {int(item["revlog_id"]) for item in receipt["applied"] if item.get("revlog_id") is not None}
            matched = _match_pending(reviews, pending_row, claimed)
            if matched is not None:
                revlog_id = matched["revlog_id"]
        receipt["applied"].append({
            "seq": review["seq"],
            "device_id": review["device_id"],
            "anki_card_id": anki_id,
            "ease": ease,
            "revlog_id": revlog_id,
        })
        receipt["pending"] = None
        _atomic_json(receipt_file, receipt)
        applied_seqs.add(review["seq"])
        result.applied += 1
    if dry_run:
        print(f"dry-run: {result.planned} grade(s) would be sent, {result.already} already recorded")
    else:
        print(f"Applied {result.applied} grade(s); {result.already} already recorded.")
        print("Sync to AnkiWeb from official Anki. This tool does not contact AnkiWeb.")
    return result
