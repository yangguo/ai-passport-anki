import importlib.util
import json
import sqlite3
import struct
import sys
import threading
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

ROOT = Path(__file__).parents[1]
DECK_PATH = ROOT / "tools" / "deck.py"
SYNC_PATH = ROOT / "tools" / "anki_sync.py"


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


deck = _load("deck", DECK_PATH)
anki_sync = _load("anki_sync", SYNC_PATH)


JLPT_NAMES = [
    "NoteID", "VocabKanji", "VocabPitch", "VocabPoS", "VocabFurigana", "VocabDefSC", "VocabDefTC",
    "VocabPlus", "VocabAudio", "SentType1", "SentType2", "SentType3", "SentType4",
    "SentFurigana1", "SentKanji1", "SentDefSC1", "SentDefTC1", "SentAudio1",
]


def _jlpt_fields():
    values = [""] * len(JLPT_NAMES)
    values[1] = "漢字"
    values[2] = "①"
    values[3] = "名"
    values[4] = "漢[かん]字[じ]"
    values[5] = "<b>简体</b>"
    values[14] = "例[れい]文"
    values[16] = "例句"
    return "\x1f".join(values)


def _schema11(path: Path, cards: list[tuple]) -> None:
    connection = sqlite3.connect(path)
    connection.execute("CREATE TABLE col (models TEXT, decks TEXT)")
    connection.execute("CREATE TABLE notes (id INTEGER, mid INTEGER, flds TEXT)")
    connection.execute("CREATE TABLE cards (id INTEGER PRIMARY KEY, nid INTEGER, did INTEGER, mod INTEGER)")
    models = {
        "10": {"id": 10, "name": "Basic", "type": 0, "flds": [{"name": "Front"}, {"name": "Back"}]},
        "11": {"id": 11, "name": "Cloze", "type": 1, "flds": [{"name": "Text"}, {"name": "Back"}]},
        "12": {"id": 12, "name": "eggrolls-JLPT10k-v3.5", "type": 0,
               "flds": [{"name": name} for name in JLPT_NAMES]},
    }
    decks = {"20": {"id": 20, "name": "Language"}, "21": {"id": 21, "name": "Other"},
             "22": {"id": 22, "name": "JLPT"}}
    connection.execute("INSERT INTO col VALUES (?, ?)", (json.dumps(models), json.dumps(decks)))
    for card_id, note_id, deck_id, model_id, fields in cards:
        connection.execute("INSERT INTO notes VALUES (?, ?, ?)", (note_id, model_id, fields))
        connection.execute("INSERT INTO cards VALUES (?, ?, ?, ?)", (card_id, note_id, deck_id, 1))
    connection.commit()
    connection.close()


def _schema18(path: Path) -> None:
    connection = sqlite3.connect(path)
    connection.execute("CREATE TABLE notetypes (id INTEGER PRIMARY KEY, name TEXT)")
    connection.execute("CREATE TABLE fields (ntid INTEGER, ord INTEGER, name TEXT)")
    connection.execute("CREATE TABLE decks (id INTEGER PRIMARY KEY, name TEXT)")
    connection.execute("CREATE TABLE notes (id INTEGER, mid INTEGER, flds TEXT)")
    connection.execute("CREATE TABLE cards (id INTEGER PRIMARY KEY, nid INTEGER, did INTEGER)")
    connection.execute("INSERT INTO notetypes VALUES (10, 'Basic')")
    connection.execute("INSERT INTO fields VALUES (10, 0, 'Front'), (10, 1, 'Back')")
    connection.execute("INSERT INTO decks VALUES (20, 'Language'), (21, 'SkipMe')")
    connection.execute("INSERT INTO notes VALUES (7, 10, ?)", ("<b>Question</b>\x1fAnswer",))
    connection.execute("INSERT INTO cards VALUES (99, 7, 20)")
    connection.execute("INSERT INTO notes VALUES (8, 10, ?)", ("Other\x1fSide",))
    connection.execute("INSERT INTO cards VALUES (100, 8, 21)")
    connection.commit()
    connection.close()


class _Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length))
        self.server.calls.append(body)
        try:
            result = self.server.dispatch(body["action"], body.get("params") or {})
            payload = {"result": result, "error": None}
        except Exception as exc:  # noqa: BLE001 - the mock reports handler failures as Anki errors
            payload = {"result": None, "error": str(exc)}
        raw = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, fmt, *args):
        return


@pytest.fixture()
def anki_server():
    server = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
    server.calls = []
    server.dispatch = lambda action, params: None
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = f"http://127.0.0.1:{server.server_address[1]}"
    yield server, url
    server.shutdown()
    thread.join(timeout=5)


def test_collection_copy_is_read_only_and_filters_decks(tmp_path, capsys):
    path = tmp_path / "collection.anki21"
    _schema11(path, [
        (5, 1, 20, 10, "Hello<br>line\x1fWorld"),
        (6, 2, 21, 10, "Hide\x1fMe"),
        (7, 3, 22, 12, _jlpt_fields()),
    ])
    original = path.read_bytes()
    running = anki_sync.anki_desktop_running
    anki_sync.anki_desktop_running = lambda: True
    try:
        names = anki_sync.list_decks(deck, url=None, collection=path, api_key=None)
        cards, source, imported_at = anki_sync.pull_decks(
            deck, ["Language", "JLPT"], url=None, collection=path, api_key=None, map_path=None,
        )
    finally:
        anki_sync.anki_desktop_running = running
    warning = capsys.readouterr().err
    assert "Anki appears to be running" in warning
    assert names == ["JLPT", "Language", "Other"]
    assert source == "collection"
    assert imported_at > 0
    assert path.read_bytes() == original
    assert not Path(str(path) + "-wal").exists()
    by_deck = {card.deck: card for card in cards}
    assert by_deck["Language"].front == "Hello\nline"
    assert by_deck["Language"].anki_id == "5"
    assert by_deck["JLPT"].front == "漢字\n漢（かん）字（じ）"
    assert "简体" in by_deck["JLPT"].back
    assert "Hide" not in {card.front for card in cards}


def test_schema18_collection_and_rejects_cloze_and_overflow(tmp_path):
    path = tmp_path / "collection.anki2"
    _schema18(path)
    original = path.read_bytes()
    cards, source, _imported = anki_sync.pull_decks(
        deck, ["Language"], url=None, collection=path, api_key=None, map_path=None,
    )
    assert source == "collection"
    assert cards == [deck.Card(1, "Language", "Question", "Answer", "99", "7")]
    assert path.read_bytes() == original

    cloze = tmp_path / "cloze.anki2"
    _schema11(cloze, [(1, 1, 20, 11, "{{c1::hidden}}\x1fback")])
    with pytest.raises(deck.DeckError, match="Cloze.*unsupported"):
        anki_sync.pull_decks(deck, ["Language"], url=None, collection=cloze, api_key=None, map_path=None)

    huge = tmp_path / "huge.anki2"
    connection = sqlite3.connect(huge)
    connection.execute("CREATE TABLE col (models TEXT, decks TEXT)")
    connection.execute("CREATE TABLE notes (id INTEGER, mid INTEGER, flds TEXT)")
    connection.execute("CREATE TABLE cards (id INTEGER PRIMARY KEY, nid INTEGER, did INTEGER)")
    connection.execute("INSERT INTO col VALUES (?, ?)", (
        json.dumps({"10": {"name": "Basic", "type": 0, "flds": [{"name": "Front"}, {"name": "Back"}]}}),
        json.dumps({"20": {"name": "Big"}}),
    ))
    connection.executemany(
        "INSERT INTO notes VALUES (?, 10, 'Q\x1fA')",
        [(index,) for index in range(1, deck.MAX_CARDS + 2)],
    )
    connection.executemany(
        "INSERT INTO cards VALUES (?, ?, 20)",
        [(index, index) for index in range(1, deck.MAX_CARDS + 2)],
    )
    connection.commit()
    connection.close()
    with pytest.raises(deck.DeckError, match="12000 cards"):
        anki_sync.pull_decks(deck, ["Big"], url=None, collection=huge, api_key=None, map_path=None)


def test_anki_connect_lists_pulls_and_reuses_device_ids(tmp_path, anki_server):
    server, url = anki_server
    catalog = {
        "Language": [{
            "cardId": 1502098034048, "note": 1502298033753, "deckName": "Language", "modelName": "Basic",
            "mod": 100, "fields": {"Front": {"value": "<i>front</i>", "order": 0},
                                   "Back": {"value": "back", "order": 1}},
        }],
        "JLPT": [{
            "cardId": 1498938915662, "note": 42, "deckName": "JLPT", "modelName": "eggrolls-JLPT10k-v3.5",
            "mod": 100,
            "fields": {name: {"value": value, "order": index} for index, (name, value) in enumerate(zip(
                JLPT_NAMES, _jlpt_fields().split("\x1f"),
            ))},
        }],
    }

    def dispatch(action, params):
        if action == "deckNames":
            return ["Language", "JLPT", "Other"]
        if action == "findCards":
            name = params["query"].split('"')[1]
            return [card["cardId"] for card in catalog[name]]
        if action == "cardsInfo":
            wanted = set(params["cards"])
            return [card for cards in catalog.values() for card in cards if card["cardId"] in wanted]
        raise AssertionError(action)

    server.dispatch = dispatch
    names = anki_sync.list_decks(deck, url=url, collection=None, api_key=None)
    assert names == ["Language", "JLPT", "Other"]
    map_path = tmp_path / "cards.map.json"
    cards, source, imported = anki_sync.pull_decks(
        deck, ["JLPT", "Language"], url=url, collection=None, api_key="local-key", map_path=map_path,
    )
    assert source == "anki-connect"
    assert [card.anki_id for card in cards] == ["1498938915662", "1502098034048"]
    assert cards[0].id == 1 and cards[1].id == 2
    assert cards[1].front == "front"
    assert cards[0].front.startswith("漢字")
    assert any(call.get("key") == "local-key" for call in server.calls)
    assert server.calls[0].get("key") is None
    deck.write_map(cards, map_path, source=source, imported_at_ms=imported, decks=["JLPT", "Language"])
    again, _source, _imported = anki_sync.pull_decks(
        deck, ["Language", "JLPT"], url=url, collection=None, api_key=None, map_path=map_path,
    )
    assert [(card.id, card.anki_id) for card in again] == [(card.id, card.anki_id) for card in cards]


class ScriptedSerial:
    def __init__(self, plan):
        self.plan = [list(replies) for replies in plan]
        self.writes = []
        self.pending = []

    def write(self, data):
        self.writes.append(data.decode("ascii").rstrip("\n"))
        self.pending = self.plan.pop(0) if self.plan else []
        return len(data)

    def readline(self):
        if not self.pending:
            return b""
        item = self.pending.pop(0)
        if item is None:
            return b""
        return item.encode("ascii") + b"\n"

    def close(self):
        pass


def _export_blob(reviews):
    schedule = [(4, 0, 0, 2500, 0, 0, 0, 0, 0)]
    header = b"PREV" + struct.pack("<HHIIHHIII", 1, 32, 3, 9, 1, 20, len(reviews), 0, 0)
    body = b"".join(
        struct.pack("<IHHHHBBBBI", due, interval, ease, reps, lapses, stage, step, deck_id, 0, card_id)
        for card_id, due, interval, ease, reps, lapses, stage, step, deck_id in schedule
    )
    body += b"".join(struct.pack("<HBBI", index, rating, 0, minute) for index, rating, minute in reviews)
    blob = header + body
    crc = zlib.crc32(blob) & 0xFFFFFFFF
    meta = (f"OK EXPORT cards=1 revlog={len(reviews)} crc={crc} length={len(blob)} "
            f"epoch=3 dropped=0 now=9 offset=0")
    resume = meta.replace("OK EXPORT", "OK XRESUME")
    chunk = f"OK XCHUNK {len(blob)} {blob.hex()}"
    return blob, meta, resume, chunk


def _map(path: Path, imported_at_ms: int = 1_700_000_000_000):
    deck.write_map(
        [deck.Card(4, "Language", "Q", "A", "1498938915662", "42")],
        path, source="anki-connect", imported_at_ms=imported_at_ms, decks=["Language"],
    )


def test_sync_back_is_idempotent_and_dry_run_does_not_answer(tmp_path, anki_server):
    server, url = anki_server
    answered = []
    reviews = {"1498938915662": []}

    def dispatch(action, params):
        if action == "cardsInfo":
            return [{"cardId": params["cards"][0], "mod": 1_600_000_000}]
        if action == "getReviewsOfCards":
            return reviews
        if action == "answerCards":
            for answer in params["answers"]:
                assert set(answer) == {"cardId", "ease"}
                answered.append(answer)
                reviews[str(answer["cardId"])].append({
                    "id": 1_800_000_000_000 + len(answered), "ease": answer["ease"],
                })
            return [True] * len(params["answers"])
        raise AssertionError(action)

    server.dispatch = dispatch
    _blob, meta, _resume, chunk = _export_blob([(0, 1, 4), (0, 3, 10)])
    map_path = tmp_path / "study.map.json"
    _map(map_path)
    plan = [
        ["OK INFO version=2 cards=1 day=0 cap=12000"],
        [meta],
        [chunk],
    ]

    def run(extra_plan, dry_run=False, after_pending=None):
        serial = ScriptedSerial(plan + extra_plan)
        return anki_sync.sync_back(
            deck, port="fake", url=url, api_key=None, map_path=map_path, receipt_path=tmp_path / "study.receipt.json",
            dry_run=dry_run, serial_factory=lambda *args, **kwargs: serial, after_pending=after_pending,
        ), serial

    dry, _serial = run([], dry_run=True)
    assert dry.planned == 2 and dry.applied == 0
    assert answered == []
    assert not (tmp_path / "study.receipt.json").exists()

    first, serial = run([])
    assert [item["ease"] for item in answered] == [2, 4]
    assert first.applied == 2
    assert serial.writes[1] == "EXPORT"
    second, _serial = run([])
    assert second.applied == 0 and second.already == 2
    assert [item["ease"] for item in answered] == [2, 4]
    bodies = [call for call in server.calls if call["action"] == "answerCards"]
    assert len(bodies) == 2


def test_sync_back_resumes_chunks_and_does_not_repeat_a_crashed_answer(tmp_path, anki_server):
    server, url = anki_server
    answered = []
    reviews = {"1498938915662": []}

    def dispatch(action, params):
        if action == "cardsInfo":
            return [{"cardId": 1498938915662, "mod": 1_600_000_000}]
        if action == "getReviewsOfCards":
            return reviews
        if action == "answerCards":
            answered.append(params["answers"][0])
            return [True]
        raise AssertionError(action)

    server.dispatch = dispatch
    _blob, meta, resume, chunk = _export_blob([(0, 1, 4)])
    map_path = tmp_path / "study.map.json"
    _map(map_path)
    receipt = tmp_path / "study.receipt.json"
    serial = ScriptedSerial([
        ["OK INFO version=2 cards=1 day=0 cap=12000"],
        [meta],
        [None],
        [resume],
        [chunk],
    ])
    with pytest.raises(deck.DeckError, match="interrupted"):
        anki_sync.sync_back(
            deck, port="fake", url=url, api_key=None, map_path=map_path, receipt_path=receipt,
            dry_run=False, timeout=0.05, serial_factory=lambda *args, **kwargs: serial,
            after_pending=lambda: (_ for _ in ()).throw(deck.DeckError("interrupted")),
        )
    assert "XRESUME" in serial.writes
    assert answered == []
    reviews["1498938915662"].append({"id": 1_800_000_000_123, "ease": 2})
    saved = json.loads(receipt.read_text(encoding="utf-8"))
    assert saved["pending"]["ease"] == 2

    serial = ScriptedSerial([
        ["OK INFO version=2 cards=1 day=0 cap=12000"],
        [meta],
        [chunk],
    ])
    result = anki_sync.sync_back(
        deck, port="fake", url=url, api_key=None, map_path=map_path, receipt_path=receipt,
        dry_run=False, serial_factory=lambda *args, **kwargs: serial,
    )
    assert result.applied == 0 and result.already == 1
    assert answered == []
    recovered = json.loads(receipt.read_text(encoding="utf-8"))
    assert recovered["pending"] is None
    assert recovered["applied"][0]["revlog_id"] == 1_800_000_000_123


def test_sync_back_skips_cards_modified_in_anki_after_pull(tmp_path, anki_server):
    server, url = anki_server

    def dispatch(action, params):
        if action == "cardsInfo":
            return [{"cardId": 1498938915662, "mod": 1_800_000_000}]
        if action == "getReviewsOfCards":
            return {"1498938915662": [{"id": 1_800_000_000_500, "ease": 3}]}
        if action == "answerCards":
            raise AssertionError("conflict card must not be answered")
        raise AssertionError(action)

    server.dispatch = dispatch
    _blob, meta, _resume, chunk = _export_blob([(0, 2, 1)])
    map_path = tmp_path / "study.map.json"
    _map(map_path, imported_at_ms=1_700_000_000_000)
    serial = ScriptedSerial([
        ["OK INFO version=2 cards=1 day=0 cap=12000"],
        [meta],
        [chunk],
    ])
    result = anki_sync.sync_back(
        deck, port="fake", url=url, api_key=None, map_path=map_path, receipt_path=None,
        dry_run=False, serial_factory=lambda *args, **kwargs: serial,
    )
    assert result.applied == 0
    assert result.conflicts
    assert not (map_path.with_suffix(".receipt.json")).exists() or json.loads(
        map_path.with_suffix(".receipt.json").read_text(encoding="utf-8")
    )["applied"] == []


def test_export_crc_mismatch_is_rejected():
    blob = b"PREV" + b"\x00" * 28
    meta = f"OK EXPORT cards=0 revlog=0 crc=1 length={len(blob)} epoch=1 dropped=0 now=0 offset=0"
    serial = ScriptedSerial([
        [meta],
        [f"OK XCHUNK {len(blob)} {blob.hex()}"],
    ])
    with pytest.raises(deck.DeckError, match="CRC"):
        anki_sync.fetch_export(deck, serial, timeout=1)
