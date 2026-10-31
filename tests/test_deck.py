import importlib.util
import json
import sqlite3
import sys
import zipfile
import zlib
from pathlib import Path

import pytest

MODULE_PATH = Path(__file__).parents[1] / "tools" / "deck.py"
spec = importlib.util.spec_from_file_location("deck", MODULE_PATH)
deck = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = deck
spec.loader.exec_module(deck)


def test_parse_tsv_accepts_bom_and_quoted_newline(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text('\ufeffid\tdeck\tfront\tback\n1\tBasics\t"line one\nline two"\tanswer\n', encoding="utf-8")
    cards = deck.load_deck(path)
    assert cards == [deck.Card(1, "Basics", "line one\nline two", "answer")]


def test_parse_anki_text_export_with_two_fields(tmp_path):
    path = tmp_path / "anki.txt"
    path.write_text("#separator:tab\n#html:false\nfront\tback\nquestion\tanswer\n", encoding="utf-8")
    cards = deck.load_deck(path)
    assert len(cards) == 1
    assert (cards[0].id, cards[0].deck, cards[0].front, cards[0].back) == (1, "Imported", "question", "answer")
    html_path = tmp_path / "anki-html.txt"
    html_path.write_text("#separator:tab\n#html:true\n<b>front</b>\tback<br>line\n", encoding="utf-8")
    html_card = deck.load_deck(html_path)[0]
    assert (html_card.front, html_card.back) == ("front", "back\nline")


@pytest.mark.parametrize("contents, message", [
    ("id\tdeck\tfront\tback\n1\tD\t\tA\n", "front.*empty"),
    ("id\tdeck\tfront\tback\n1\tD\tQ\tA\n1\tD\tQ2\tA2\n", "duplicate.*id"),
    ("id\tdeck\tfront\tback\n4294967296\tD\tQ\tA\n", "id.*range"),
    ("id\tdeck\tfront\tback\n1\tD\tQ\t" + "x" * 512 + "\n", "511 bytes"),
    ("id\tdeck\tfront\tback\n1\tD\tQ\x01x\tA\n", "control"),
    ("id\tdeck\tfront\tback\n1\tD\temoji 😀\tA\n", r"no pa_font_16 glyph.*U\+1F600"),
])
def test_tsv_rejects_invalid_card_text(tmp_path, contents, message):
    path = tmp_path / "bad.tsv"
    path.write_text(contents, encoding="utf-8")
    with pytest.raises(deck.DeckError, match=message):
        deck.load_deck(path)


def test_tsv_rejects_more_than_12000_cards(tmp_path):
    path = tmp_path / "too-many.tsv"
    rows = ["id\tdeck\tfront\tback"] + [f"{i}\tD\tQ\tA" for i in range(1, deck.MAX_CARDS + 2)]
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")
    with pytest.raises(deck.DeckError, match="12000 cards"):
        deck.load_deck(path)


def test_tsv_accepts_kana_pitch_marks_and_511_byte_side(tmp_path):
    path = tmp_path / "kana.tsv"
    back = "あいう①α…" + ("字" * 160)
    assert len(back.encode("utf-8")) <= 511
    path.write_text("id\tdeck\tfront\tback\n1\t語彙\t\"漢字\nかんじ\"\t" + back + "\n", encoding="utf-8")
    card = deck.load_deck(path)[0]
    assert card.front == "漢字\nかんじ"
    assert card.back.startswith("あいう①α…")


def test_commands_are_hex_encoded_bounded_and_finish_with_commit(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text("id\tdeck\tfront\tback\n1\t中文\t问题\t答案\n", encoding="utf-8")
    cards = deck.load_deck(path)
    commands = deck.commands_for(cards)
    payload = deck.payload_for(cards)
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    assert commands[0] == f"BEGIN 1 {crc} {len(payload)}"
    chunks = [line for line in commands if line.startswith("CHUNK ")]
    assert chunks, "payload must be sent in at least one chunk"
    reassembled = b"".join(bytes.fromhex(line.split(" ", 2)[2]) for line in chunks)
    assert reassembled == payload
    assert commands[-1] == "COMMIT"
    assert payload[4:9] == bytes([len("中文".encode()), 6, 0, 6, 0])
    assert all(len(line.encode("ascii")) <= deck.MAX_LINE_BYTES for line in commands)
    # Device console fragments lines over ~64 chars (no USB stream reassembly).
    assert all(len(line.encode("ascii")) <= 60 for line in commands)


def test_command_stream_rejects_embedded_nul_and_unsupported_glyph(tmp_path):
    path = tmp_path / "missing-font-glyph.tsv"
    path.write_text("id\tdeck\tfront\tback\n1\tD\t\u0104\tA\n", encoding="utf-8")
    with pytest.raises(deck.DeckError, match=r"no pa_font_16 glyph.*U\+0104"):
        deck.load_deck(path)

    path = tmp_path / "nul.tsv"
    path.write_bytes(b"id\tdeck\tfront\tback\n1\tD\tQ\tA\x00B\n")
    with pytest.raises(deck.DeckError, match="NUL"):
        deck.load_deck(path)


def _make_apkg(path, model_type=0, model_name="Basic", collection_name="collection.anki2", html="Question<br>two"):
    db_path = path.parent / f"{path.stem}-collection.sqlite"
    db = sqlite3.connect(db_path)
    db.execute("CREATE TABLE col (models TEXT, decks TEXT)")
    db.execute("CREATE TABLE notes (id INTEGER, mid INTEGER, flds TEXT)")
    db.execute("CREATE TABLE cards (id INTEGER PRIMARY KEY, nid INTEGER, did INTEGER)")
    db.execute("INSERT INTO col VALUES (?, ?)", (
        '{"10":{"id":10,"name":"' + model_name + '","type":' + str(model_type) + ',"flds":[{"name":"Front"},{"name":"Back"}]}}',
        '{"20":{"id":20,"name":"Language"}}',
    ))
    db.execute("INSERT INTO notes VALUES (1, 10, ?)", (html + "\x1f<b>Answer</b>\x1fignored extra field",))
    db.execute("INSERT INTO cards VALUES (1, 1, 20)")
    db.commit()
    db.close()
    with zipfile.ZipFile(path, "w") as archive:
        archive.write(db_path, collection_name)


def test_apkg_basic_converts_html_to_plain_text_and_deck_name(tmp_path):
    path = tmp_path / "basic.apkg"
    _make_apkg(path)
    cards = deck.load_deck(path)
    assert cards == [deck.Card(1, "Language", "Question\ntwo", "Answer", "1", "1")]


def test_apkg_rejects_cloze_and_custom_models(tmp_path):
    cloze = tmp_path / "cloze.apkg"
    _make_apkg(cloze, model_type=1, model_name="Cloze")
    with pytest.raises(deck.DeckError, match="Cloze.*unsupported"):
        deck.load_deck(cloze)
    custom = tmp_path / "custom.apkg"
    _make_apkg(custom, model_type=0, model_name="Custom")
    with pytest.raises(deck.DeckError, match="custom.*unsupported"):
        deck.load_deck(custom)


def test_apkg_rejects_missing_collection(tmp_path):
    empty = tmp_path / "empty.apkg"
    with zipfile.ZipFile(empty, "w") as archive:
        archive.writestr("media", "{}")
    with pytest.raises(deck.DeckError, match="collection.anki2.*collection.anki21"):
        deck.load_deck(empty)


def _make_anki21b_apkg(path, db_path, extra_members=None):
    import zstandard
    raw = db_path.read_bytes()
    blob = zstandard.ZstdCompressor().compress(raw)
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("collection.anki21b", blob)
        for name, data_path in (extra_members or {}).items():
            archive.write(data_path, name)


def test_apkg_prefers_anki21b_over_stub_anki2(tmp_path):
    stub = tmp_path / "stub.sqlite"
    db = sqlite3.connect(stub)
    db.execute("CREATE TABLE col (models TEXT, decks TEXT)")
    db.execute("CREATE TABLE notes (id INTEGER, mid INTEGER, flds TEXT)")
    db.execute("CREATE TABLE cards (id INTEGER PRIMARY KEY, nid INTEGER, did INTEGER)")
    db.execute("INSERT INTO col VALUES (?, ?)", (
        '{"10":{"id":10,"name":"Basic","type":0,"flds":[{"name":"Front"},{"name":"Back"}]}}',
        '{"20":{"id":20,"name":"Stub"}}',
    ))
    db.execute("INSERT INTO notes VALUES (1, 10, 'Wrong\x1fSide')")
    db.execute("INSERT INTO cards VALUES (1, 1, 20)")
    db.commit()
    db.close()
    real = tmp_path / "real-collection.sqlite"
    rdb = sqlite3.connect(real)
    rdb.execute("CREATE TABLE col (models TEXT, decks TEXT)")
    rdb.execute("CREATE TABLE notes (id INTEGER, mid INTEGER, flds TEXT)")
    rdb.execute("CREATE TABLE cards (id INTEGER PRIMARY KEY, nid INTEGER, did INTEGER)")
    rdb.execute("INSERT INTO col VALUES (?, ?)", (
        '{"10":{"id":10,"name":"Basic","type":0,"flds":[{"name":"Front"},{"name":"Back"}]}}',
        '{"20":{"id":20,"name":"Real"}}',
    ))
    rdb.execute("INSERT INTO notes VALUES (1, 10, 'Right\x1fSide')")
    rdb.execute("INSERT INTO cards VALUES (1, 1, 20)")
    rdb.commit()
    rdb.close()
    path = tmp_path / "mixed.apkg"
    _make_anki21b_apkg(path, real, {"collection.anki2": stub})
    cards = deck.load_deck(path)
    assert cards == [deck.Card(1, "Real", "Right", "Side", "1", "1")]


def test_apkg_rejects_corrupt_anki21b(tmp_path):
    path = tmp_path / "corrupt.apkg"
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("collection.anki21b", b"not zstd data at all............")
    with pytest.raises(deck.DeckError, match="anki21b"):
        deck.load_deck(path)


class FakeSerial:
    def __init__(self, responses):
        self.responses = [line.encode("ascii") + b"\n" for line in responses]
        self.writes = []
        self.timeout = 1

    def write(self, data):
        self.writes.append(data.decode("ascii").rstrip("\n"))
        return len(data)

    def readline(self):
        if self.responses:
            return self.responses.pop(0)
        return b""

    def close(self):
        pass


def test_upload_waits_for_matching_ack_and_succeeds_only_after_commit(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text("id\tdeck\tfront\tback\n1\tD\tQ\tA\n", encoding="utf-8")
    serial = FakeSerial([
        "boot: ready", "OK INFO version=2 cards=0 day=0 cap=12000 room=2097152",
        "OK BEGIN resume=0", "OK CHUNK 12", "OK COMMIT",
    ])
    deck.upload(path, port="fake", serial_factory=lambda *a, **k: serial)
    assert serial.writes[0] == "INFO"
    assert serial.writes[1].startswith("BEGIN 1 ")
    assert serial.writes[2].startswith("CHUNK 0 ")
    assert serial.writes[3] == "COMMIT"


def test_upload_aborts_on_err_and_never_reports_success(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text("id\tdeck\tfront\tback\n1\tD\tQ\tA\n", encoding="utf-8")
    serial = FakeSerial([
        "OK INFO version=2 cards=0 day=0 cap=12000 room=2097152", "OK BEGIN resume=0",
        "ERR INVALID", "ERR TRANSACTION", "OK ABORT",
    ])
    with pytest.raises(deck.DeckError, match="device error.*INVALID"):
        deck.upload(path, port="fake", serial_factory=lambda *a, **k: serial)
    assert serial.writes[-1] == "ABORT"


def test_upload_timeout_aborts_and_raises(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text("id\tdeck\tfront\tback\n1\tD\tQ\tA\n", encoding="utf-8")
    serial = FakeSerial(["OK INFO version=2 cards=0 day=0 cap=12000 room=2097152", "OK BEGIN resume=0", ""])
    with pytest.raises(deck.DeckError, match="timeout.*CHUNK"):
        deck.upload(path, port="fake", serial_factory=lambda *a, **k: serial, command_timeout=0.001)
    assert "ABORT" in serial.writes


def test_upload_rejects_mismatched_ack_instead_of_accepting_it(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text("id\tdeck\tfront\tback\n1\tD\tQ\tA\n", encoding="utf-8")
    serial = FakeSerial([
        "OK INFO version=2 cards=0 day=0 cap=12000 room=2097152", "OK CARD", "OK ABORT",
    ])
    with pytest.raises(deck.DeckError, match="timeout.*BEGIN"):
        deck.upload(path, port="fake", serial_factory=lambda *a, **k: serial, command_timeout=0.001)
    assert serial.writes[0] == "INFO"
    assert serial.writes[1].startswith("BEGIN 1 ")
    assert serial.writes[-1] == "ABORT"


def test_upload_rejects_unknown_device_protocol_version(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text("id\tdeck\tfront\tback\n1\tD\tQ\tA\n", encoding="utf-8")
    serial = FakeSerial(["OK INFO version=9 cards=0 day=0 cap=12000 room=2097152"])
    with pytest.raises(deck.DeckError, match="protocol version 9"):
        deck.upload(path, port="fake", serial_factory=lambda *a, **k: serial)
    assert serial.writes == ["INFO"]


def test_upload_rejects_deck_larger_than_device_cap(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text(
        "id\tdeck\tfront\tback\n1\tD\tQ\tA\n2\tD\tQ2\tA2\n",
        encoding="utf-8",
    )
    serial = FakeSerial(["OK INFO version=2 cards=0 day=0 cap=1 room=2097152"])
    with pytest.raises(deck.DeckError, match="capacity is 1"):
        deck.upload(path, port="fake", serial_factory=lambda *a, **k: serial)
    assert serial.writes == ["INFO"]


def test_upload_rejects_payload_larger_than_device_room(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text("id\tdeck\tfront\tback\n1\tD\tQ\tA\n", encoding="utf-8")
    payload = deck.payload_for(deck.load_deck(path))
    serial = FakeSerial(["OK INFO version=2 cards=0 day=0 cap=12000 room=1"])
    with pytest.raises(deck.DeckError, match=f"{len(payload)} bytes"):
        deck.upload(path, port="fake", serial_factory=lambda *a, **k: serial)
    assert serial.writes == ["INFO"]


def test_jlpt_model_strips_media_and_expands_furigana(tmp_path):
    path = tmp_path / "jlpt.apkg"
    db_path = tmp_path / "jlpt.sqlite"
    fields = "\x1f".join([
        "note",
        "漢字",
        "①",
        "名",
        "漢[かん]字[じ]",
        "<b>简体定义</b>",
        "繁體",
        "补充",
        "[sound:vocab.mp3]",
        "A", "", "", "",
        "例[れい]文",
        "例文",
        "例句释义",
        "例句繁体",
        "[sound:sent.mp3]",
        "", "", "", "", "", "", "", "", "",
        "1", "", "",
    ])
    names = [
        "NoteID", "VocabKanji", "VocabPitch", "VocabPoS", "VocabFurigana", "VocabDefSC", "VocabDefTC",
        "VocabPlus", "VocabAudio", "SentType1", "SentType2", "SentType3", "SentType4",
        "SentFurigana1", "SentKanji1", "SentDefSC1", "SentDefTC1", "SentAudio1",
        "SentFurigana2", "SentKanji2", "SentDefSC2", "SentDefTC2", "SentAudio2",
        "SentFurigana3", "SentKanji3", "SentDefSC3", "SentDefTC3", "SentAudio3",
        "SentFurigana4", "SentKanji4", "SentDefSC4", "SentDefTC4", "SentAudio4",
        "Order", "Alt1", "Alt2",
    ]
    model = {"id": 10, "name": "eggrolls-JLPT10k-v3.5", "type": 0,
             "flds": [{"name": name} for name in names]}
    db = sqlite3.connect(db_path)
    db.execute("CREATE TABLE col (models TEXT, decks TEXT)")
    db.execute("CREATE TABLE notes (id INTEGER, mid INTEGER, flds TEXT)")
    db.execute("CREATE TABLE cards (id INTEGER PRIMARY KEY, nid INTEGER, did INTEGER)")
    db.execute("INSERT INTO col VALUES (?, ?)", (
        json.dumps({"10": model}),
        json.dumps({"20": {"id": 20, "name": "JLPT::N5::1-高频"}}),
    ))
    db.execute("INSERT INTO notes VALUES (7, 10, ?)", (fields,))
    db.execute("INSERT INTO cards VALUES (99, 7, 20)")
    db.commit()
    db.close()
    with zipfile.ZipFile(path, "w") as archive:
        archive.write(db_path, "collection.anki2")
        archive.write(db_path, "collection.anki21")
        archive.writestr("0", b"not-media-extracted")
    card = deck.load_deck(path)[0]
    assert card.id == 1
    assert card.anki_id == "99"
    assert card.deck == "1-高频"
    assert card.front == "漢字\n漢（かん）字（じ）"
    assert "简体定义" in card.back
    assert "例（れい）文" in card.back
    assert "sound" not in card.back
    assert "繁體" not in card.back


def test_upload_resumes_from_device_offset(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text("id\tdeck\tfront\tback\n1\tD\tQ\tA\n", encoding="utf-8")
    cards = deck.load_deck(path)
    payload = deck.payload_for(cards)
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    serial = FakeSerial([
        "OK INFO version=2 cards=0 day=0 cap=12000 room=2097152",
        "OK BEGIN resume=0",
        "ERR BUSY",
        f"OK RESUME offset=0 count=1 crc={crc} length={len(payload)}",
        "OK CHUNK 12",
        "OK COMMIT",
    ])
    deck.upload(path, port="fake", serial_factory=lambda *a, **k: serial)
    assert "RESUME" in serial.writes
    assert serial.writes[-1] == "COMMIT"


def test_font_manifest_records_actual_requested_range_coverage():
    import json
    manifest_path = Path(__file__).parents[1] / "assets" / "fonts" / "pa_font_16.manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["font_symbol"] == "pa_font_16"
    assert manifest["source_revision"] == "f8d157532fbfaeda587e826d4cd5b21a49186f7c"
    assert manifest["requested_glyph_count"] == 22447
    assert manifest["supported_glyph_count"] == 21957
    assert manifest["unsupported_glyph_count"] == 490

    def covers(codepoint: int) -> bool:
        return any(start <= codepoint <= end for start, end in manifest["supported_ranges"])

    assert covers(0x3042) and covers(0x30A2) and covers(0x30F6)
    assert covers(0x2460) and covers(0x2473) and covers(0x24EA)
    assert covers(0x03B1) and covers(0x2026)


def test_apkg_nested_deck_names_use_leaf(tmp_path):
    db_path = tmp_path / "nested-collection.sqlite"
    db = sqlite3.connect(db_path)
    db.execute("CREATE TABLE notetypes (id INTEGER PRIMARY KEY, name TEXT)")
    db.execute("CREATE TABLE fields (ntid INTEGER, ord INTEGER, name TEXT)")
    db.execute("CREATE TABLE decks (id INTEGER PRIMARY KEY, name TEXT)")
    db.execute("CREATE TABLE notes (id INTEGER, mid INTEGER, flds TEXT)")
    db.execute("CREATE TABLE cards (id INTEGER PRIMARY KEY, nid INTEGER, did INTEGER)")
    db.execute("INSERT INTO notetypes VALUES (10, 'Basic')")
    db.execute("INSERT INTO fields VALUES (10, 0, 'Front'), (10, 1, 'Back')")
    db.execute("INSERT INTO decks VALUES (20, ?)", ("NEW-JLPT\x1feggrolls-JLPT10k-v3.5\x1f3-N3\x1f1-高频",))
    db.execute("INSERT INTO decks VALUES (21, 'Parent::Child')")
    db.execute("INSERT INTO notes VALUES (1, 10, 'Q1\x1fA1')")
    db.execute("INSERT INTO notes VALUES (2, 10, 'Q2\x1fA2')")
    db.execute("INSERT INTO cards VALUES (1, 1, 20)")
    db.execute("INSERT INTO cards VALUES (2, 2, 21)")
    db.commit()
    db.close()
    path = tmp_path / "nested.apkg"
    _make_anki21b_apkg(path, db_path)
    cards = deck.load_deck(path)
    assert [(card.deck, card.front) for card in cards] == [("1-高频", "Q1"), ("Child", "Q2")]


def test_apkg_anki21b_decompressed_cap_matches_design(tmp_path, monkeypatch):
    db_path = tmp_path / "tiny.sqlite"
    db = sqlite3.connect(db_path)
    db.execute("CREATE TABLE col (models TEXT, decks TEXT)")
    db.execute("CREATE TABLE notes (id INTEGER, mid INTEGER, flds TEXT)")
    db.execute("CREATE TABLE cards (id INTEGER PRIMARY KEY, nid INTEGER, did INTEGER)")
    db.execute("INSERT INTO col VALUES (?, ?)", (
        '{"10":{"id":10,"name":"Basic","type":0,"flds":[{"name":"Front"},{"name":"Back"}]}}',
        '{"20":{"id":20,"name":"D"}}',
    ))
    db.execute("INSERT INTO notes VALUES (1, 10, 'Q\x1fA')")
    db.execute("INSERT INTO cards VALUES (1, 1, 20)")
    db.commit()
    db.close()
    monkeypatch.setattr(deck, "APKG_DECOMPRESSED_MAX_BYTES", db_path.stat().st_size - 1)
    path = tmp_path / "capped.apkg"
    _make_anki21b_apkg(path, db_path)
    with pytest.raises(deck.DeckError, match="cannot decompress"):
        deck.load_deck(path)
