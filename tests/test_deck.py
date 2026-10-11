import importlib.util
import sys
import io
import sqlite3
import zipfile
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
    ("id\tdeck\tfront\tback\n1\tD\tQ\t" + "x" * 256 + "\n", "255 bytes"),
    ("id\tdeck\tfront\tback\n1\tD\tQ\x01x\tA\n", "control"),
    ("id\tdeck\tfront\tback\n1\tD\temoji 😀\tA\n", r"unsupported.*U\+"),
])
def test_tsv_rejects_invalid_card_text(tmp_path, contents, message):
    path = tmp_path / "bad.tsv"
    path.write_text(contents, encoding="utf-8")
    with pytest.raises(deck.DeckError, match=message):
        deck.load_deck(path)


def test_tsv_rejects_more_than_64_cards(tmp_path):
    path = tmp_path / "too-many.tsv"
    rows = ["id\tdeck\tfront\tback"] + [f"{i}\tD\tQ{i}\tA{i}" for i in range(1, 66)]
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")
    with pytest.raises(deck.DeckError, match="64 cards"):
        deck.load_deck(path)


def test_commands_are_hex_encoded_bounded_and_finish_with_commit(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text("id\tdeck\tfront\tback\n1\t中文\t问题\t答案\n", encoding="utf-8")
    commands = deck.commands_for(deck.load_deck(path))
    assert commands == [
        "BEGIN 1",
        "CARD 1 e4b8ade69687 e997aee9a298 e7ad94e6a188",
        "COMMIT",
    ]
    assert all(len(line.encode("ascii")) <= deck.MAX_LINE_BYTES for line in commands)


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
    assert cards == [deck.Card(1, "Language", "Question\ntwo", "Answer")]


def test_apkg_rejects_cloze_and_custom_models(tmp_path):
    cloze = tmp_path / "cloze.apkg"
    _make_apkg(cloze, model_type=1, model_name="Cloze")
    with pytest.raises(deck.DeckError, match="Cloze.*unsupported"):
        deck.load_deck(cloze)
    custom = tmp_path / "custom.apkg"
    _make_apkg(custom, model_type=0, model_name="Custom")
    with pytest.raises(deck.DeckError, match="custom.*unsupported"):
        deck.load_deck(custom)


def test_apkg_rejects_compressed_and_missing_collection(tmp_path):
    compressed = tmp_path / "compressed.apkg"
    with zipfile.ZipFile(compressed, "w") as archive:
        archive.writestr("collection.anki21b", b"compressed")
    with pytest.raises(deck.DeckError, match="anki21b.*unsupported"):
        deck.load_deck(compressed)
    empty = tmp_path / "empty.apkg"
    with zipfile.ZipFile(empty, "w") as archive:
        archive.writestr("media", "{}")
    with pytest.raises(deck.DeckError, match="collection.anki2.*collection.anki21"):
        deck.load_deck(empty)


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
        "boot: ready", "OK INFO version=1 cards=0 day=0",
        "OK BEGIN", "OK CARD", "OK COMMIT",
    ])
    deck.upload(path, port="fake", serial_factory=lambda *a, **k: serial)
    assert serial.writes == ["INFO", "BEGIN 1", "CARD 1 44 51 41", "COMMIT"]


def test_upload_aborts_on_err_and_never_reports_success(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text("id\tdeck\tfront\tback\n1\tD\tQ\tA\n", encoding="utf-8")
    serial = FakeSerial(["OK INFO version=1 cards=0 day=0", "OK BEGIN", "ERR INVALID", "OK ABORT"])
    with pytest.raises(deck.DeckError, match="device error.*INVALID"):
        deck.upload(path, port="fake", serial_factory=lambda *a, **k: serial)
    assert serial.writes[-1] == "ABORT"


def test_upload_timeout_aborts_and_raises(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text("id\tdeck\tfront\tback\n1\tD\tQ\tA\n", encoding="utf-8")
    serial = FakeSerial(["OK INFO version=1 cards=0 day=0", "OK BEGIN", ""])
    with pytest.raises(deck.DeckError, match="timeout.*CARD"):
        deck.upload(path, port="fake", serial_factory=lambda *a, **k: serial, command_timeout=0.001)
    assert "ABORT" in serial.writes


def test_upload_rejects_mismatched_ack_instead_of_accepting_it(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text("id\tdeck\tfront\tback\n1\tD\tQ\tA\n", encoding="utf-8")
    serial = FakeSerial([
        "OK INFO version=1 cards=0 day=0", "OK CARD", "OK ABORT",
    ])
    with pytest.raises(deck.DeckError, match="timeout.*BEGIN 1"):
        deck.upload(path, port="fake", serial_factory=lambda *a, **k: serial, command_timeout=0.001)
    assert serial.writes == ["INFO", "BEGIN 1", "ABORT"]


def test_upload_rejects_unknown_device_protocol_version(tmp_path):
    path = tmp_path / "cards.tsv"
    path.write_text("id\tdeck\tfront\tback\n1\tD\tQ\tA\n", encoding="utf-8")
    serial = FakeSerial(["OK INFO version=2 cards=0 day=0"])
    with pytest.raises(deck.DeckError, match="protocol version 2"):
        deck.upload(path, port="fake", serial_factory=lambda *a, **k: serial)
    assert serial.writes == ["INFO"]


def test_font_manifest_records_actual_requested_range_coverage():
    import json
    manifest_path = Path(__file__).parents[1] / "assets" / "fonts" / "pa_font_16.manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["font_symbol"] == "pa_font_16"
    assert manifest["source_revision"] == "f8d157532fbfaeda587e826d4cd5b21a49186f7c"
    assert manifest["requested_glyph_count"] == 21823
    assert manifest["supported_glyph_count"] == 21509
    assert manifest["unsupported_glyph_count"] == 314
