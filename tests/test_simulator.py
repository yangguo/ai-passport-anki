import os
import re
import struct
import subprocess
import zlib
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "host"
SIM = BUILD / "passport_sim"


@pytest.fixture(scope="session", autouse=True)
def build_simulator():
    result = subprocess.run(
        ["cmake", "--build", str(BUILD), "--target", "passport_sim", "--parallel"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert SIM.is_file()


def run_sim(*args, commands="", env=None):
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    return subprocess.run(
        [str(SIM), *map(str, args)],
        cwd=ROOT,
        input=commands,
        text=True,
        capture_output=True,
        env=merged_env,
        check=False,
    )


def run_sim_bytes(*args, commands=b""):
    return subprocess.run(
        [str(SIM), *map(str, args)],
        cwd=ROOT,
        input=commands,
        capture_output=True,
        check=False,
    )


def import_commands(cards):
    import struct
    import zlib

    payload = b""
    for card_id, deck, front, back in cards:
        deck_b, front_b, back_b = deck.encode(), front.encode(), back.encode()
        payload += struct.pack("<IBHH", card_id, len(deck_b), len(front_b), len(back_b)) + deck_b + front_b + back_b
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    commands = [f"BEGIN {len(cards)} {crc} {len(payload)}\n"]
    offset = 0
    while offset < len(payload):
        chunk = payload[offset:offset + 480]
        commands.append(f"CHUNK {offset} {chunk.hex()}\n")
        offset += len(chunk)
    commands.append("COMMIT\n")
    return "".join(commands)


def test_import_review_requires_flip_and_restores_snapshot(tmp_path):
    snapshot = tmp_path / "study.pak"
    cards = [(10, "中文 词汇", "你好", "Hello"), (20, "Other", "Q", "A")]
    commands = import_commands(cards) + "DECK 中文 词汇\nNEXT\nRATE GOOD\nFLIP\nRATE GOOD\nQUIT\n"

    first = run_sim("--state", snapshot, commands=commands)

    assert first.returncode == 0, first.stderr
    assert "OK COMMIT" in first.stdout
    assert "已选择牌组：中文 词汇" in first.stdout
    assert "正面：你好" in first.stdout
    assert "ERR FLIP_REQUIRED" in first.stdout
    assert "背面：Hello" in first.stdout
    assert "已评分：Good" in first.stdout
    assert snapshot.is_file()

    restarted = run_sim("--state", snapshot, commands="INFO\nDECK 中文 词汇\nNEXT\nQUIT\n")

    assert restarted.returncode == 0, restarted.stderr
    assert "OK INFO version=2 cards=2 day=0" in restarted.stdout
    assert "今天没有到期卡片" in restarted.stdout


def test_invalid_snapshot_is_rejected_without_overwrite(tmp_path):
    snapshot = tmp_path / "broken.pak"
    original = b"not a passport anki snapshot"
    snapshot.write_bytes(original)

    result = run_sim("--state", snapshot, commands="INFO\nQUIT\n")

    assert result.returncode != 0
    assert "ERR SNAPSHOT" in result.stderr
    assert snapshot.read_bytes() == original


def test_persistence_failure_keeps_review_card_and_snapshot(tmp_path):
    snapshot = tmp_path / "study.pak"
    imported = run_sim(
        "--state", snapshot,
        commands=import_commands([(1, "Deck", "Front", "Back")]) + "QUIT\n",
    )
    assert imported.returncode == 0, imported.stderr
    original = snapshot.read_bytes()
    backup = tmp_path / "saved.pak"

    process = subprocess.Popen(
        [str(SIM), "--state", str(snapshot)],
        cwd=ROOT,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    assert process.stdin is not None and process.stdout is not None
    process.stdin.write("DECK Deck\nNEXT\nFLIP\n")
    process.stdin.flush()
    shown = [process.stdout.readline() for _ in range(4)]
    assert "正面：Front" in "".join(shown)
    assert "背面：Back" in "".join(shown)

    snapshot.rename(backup)
    snapshot.mkdir()
    process.stdin.write("RATE GOOD\nRATE GOOD\nQUIT\n")
    process.stdin.flush()
    process.stdin.close()
    remaining = process.stdout.read()
    assert process.wait(timeout=5) == 0
    output = "".join(shown) + remaining

    snapshot.rmdir()
    backup.rename(snapshot)
    assert output.count("ERR STORAGE") == 2
    assert "已评分：Good" not in output
    assert snapshot.read_bytes() == original


def test_day_advance_and_no_due_cards(tmp_path):
    snapshot = tmp_path / "study.pak"
    cards = [(1, "Deck", "Front", "Back")]
    max_day = (2**32 - 1) // 1440
    result = run_sim(
        "--state", snapshot,
        commands=import_commands(cards)
        + "DECK Deck\nNEXT\nFLIP\nRATE EASY\nDAY 2\nINFO\n"
        + "NEXT\nADVANCE 5\nINFO\nNEXT\nDAY 1\n"
        + f"DAY {max_day}\nINFO\nADVANCE 256\nQUIT\n",
    )

    assert result.returncode == 0, result.stderr
    assert "OK DAY" in result.stdout
    assert "OK INFO version=2 cards=1 day=2" in result.stdout
    assert f"OK INFO version=2 cards=1 day={max_day}" in result.stdout
    assert result.stdout.count("今天没有到期卡片") == 2
    assert "OK ADVANCE now=2885" in result.stdout
    assert "ERR DAY" in result.stdout
    assert "ERR TIME" in result.stdout


def test_without_state_path_keeps_collection_in_memory():
    first = run_sim(commands=import_commands([(1, "Deck", "Front", "Back")]) + "INFO\nQUIT\n")
    restarted = run_sim(commands="INFO\nQUIT\n")

    assert first.returncode == 0, first.stderr
    assert "OK COMMIT" in first.stdout
    assert "OK INFO version=2 cards=1 day=0" in first.stdout
    assert restarted.returncode == 0, restarted.stderr
    assert "OK INFO version=2 cards=0 day=0" in restarted.stdout


@pytest.mark.parametrize(
    ("rating", "delay_minutes"),
    [("AGAIN", 1), ("HARD", 6), ("GOOD", 10), ("EASY", 5760)],
)
def test_each_rating_persists_core_due_time(tmp_path, rating, delay_minutes):
    snapshot = tmp_path / f"{rating.lower()}.pak"
    first = run_sim(
        "--state", snapshot,
        commands=import_commands([(1, "Deck", "Front", "Back")])
        + f"DECK Deck\nNEXT\nFLIP\nRATE {rating}\nQUIT\n",
    )
    restarted = run_sim(
        "--state", snapshot,
        commands="DECK Deck\nNEXT\n"
        + f"ADVANCE {delay_minutes - 1}\nNEXT\nADVANCE 1\nNEXT\nQUIT\n",
    )

    assert first.returncode == 0, first.stderr
    assert "OK COMMIT" in first.stdout
    assert f"已评分：{rating.title()}" in first.stdout
    assert restarted.returncode == 0, restarted.stderr
    assert restarted.stdout.count("今天没有到期卡片") == 2
    assert "正面：Front" in restarted.stdout


def test_imports_10000_cards_in_memory():
    cards = [(index, f"D{index % 10}", "Q", "A") for index in range(1, 10001)]
    result = run_sim(commands=import_commands(cards) + "INFO\nDECK D1\nNEXT\nQUIT\n")

    assert result.returncode == 0, result.stderr
    assert "OK INFO version=2 cards=10000 day=0" in result.stdout
    assert "正面：Q" in result.stdout


def test_binary_input_rejects_nul_internal_cr_and_recovers_after_long_line():
    commands = (
        import_commands([(1, "Deck", "Front", "Back")]).encode()
        + b"DECK Deck\nNEXT\nQUIT\x00junk\nRATE GOOD\rjunk\n"
        + b"x" * (1200 + 1) + b"\nFLIP\r\nRATE GOOD\r\nINFO\r\nQUIT\r\n"
    )

    result = run_sim_bytes(commands=commands)
    output = result.stdout.decode("utf-8")

    assert result.returncode == 0, result.stderr.decode("utf-8", errors="replace")
    assert output.count("ERR COMMAND") == 2
    assert "ERR LINE" in output
    assert "背面：Back" in output
    assert "已评分：Good" in output
    assert "OK INFO version=2 cards=1 day=0" in output
    assert output.endswith("再见。\n")


def _export_lines(output):
    meta = None
    chunks = []
    resumes = []
    for line in output.splitlines():
        if line.startswith("OK EXPORT ") or line.startswith("OK XRESUME "):
            match = re.fullmatch(
                r"OK (?:EXPORT|XRESUME) cards=(\d+) revlog=(\d+) crc=(\d+) length=(\d+) "
                r"epoch=(\d+) dropped=(\d+) now=(\d+) offset=(\d+)",
                line,
            )
            assert match, line
            parsed = {
                "kind": "EXPORT" if line.startswith("OK EXPORT") else "XRESUME",
                "cards": int(match.group(1)),
                "revlog": int(match.group(2)),
                "crc": int(match.group(3)),
                "length": int(match.group(4)),
                "epoch": int(match.group(5)),
                "dropped": int(match.group(6)),
                "now": int(match.group(7)),
                "offset": int(match.group(8)),
            }
            if parsed["kind"] == "EXPORT":
                meta = parsed
            else:
                resumes.append(parsed)
        elif line.startswith("OK XCHUNK "):
            match = re.fullmatch(r"OK XCHUNK (\d+) ([0-9a-fA-F]+)", line)
            assert match, line
            data = bytes.fromhex(match.group(2))
            chunks.append((int(match.group(1)) - len(data), data))
    assert meta is not None
    return meta, chunks, resumes


def _assemble(meta, chunks):
    image = bytearray()
    for offset, data in chunks:
        if offset == len(image) and len(image) < meta["length"]:
            image += data
    assert len(image) == meta["length"]
    assert zlib.crc32(image) & 0xFFFFFFFF == meta["crc"]
    return bytes(image)


def test_export_resume_crc_and_import_clears_the_log(tmp_path):
    snapshot = tmp_path / "study.pak"
    cards = [(index, "Deck", "Q", "A") for index in range(1, 31)]
    script = import_commands(cards)
    script += "DECK Deck\nNEXT\nFLIP\nRATE HARD\nEXPORT\nXCHUNK 0\nXRESUME\nXCHUNK 0\n"
    for offset in range(0, 4096, 480):
        script += f"XCHUNK {offset}\n"
    script += "XCHUNK 999999\nQUIT\n"
    result = run_sim("--state", snapshot, commands=script)
    assert result.returncode == 0, result.stderr
    meta, chunks, resumes = _export_lines(result.stdout)
    assert meta["cards"] == 30 and meta["revlog"] == 1 and meta["dropped"] == 0
    assert resumes and resumes[0]["crc"] == meta["crc"] and resumes[0]["length"] == meta["length"]
    assert "ERR RANGE" in result.stdout
    image = _assemble(meta, chunks)
    header = image[:32]
    assert header[:4] == b"PREV"
    _version, _header_len, epoch, _now, cards_n, sched_len, revlog, dropped, flags = struct.unpack_from("<HHIIHHIII", header, 4)
    assert (cards_n, sched_len, revlog, dropped, flags) == (30, 20, 1, 0, 0)
    review = image[32 + 30 * 20:]
    index, rating, reserved, _minute = struct.unpack("<HBBI", review)
    assert (index, rating, reserved) == (0, 1, 0)
    repeated = [data for offset, data in chunks if offset == 0]
    assert len(repeated) >= 2 and repeated[0] == repeated[1]

    replaced = run_sim(
        "--state", snapshot,
        commands=import_commands([(1, "Deck", "Front", "Back")]) + "EXPORT\nXCHUNK 0\nQUIT\n",
    )
    assert replaced.returncode == 0, replaced.stderr
    cleared, cleared_chunks, _resumes = _export_lines(replaced.stdout)
    assert cleared["cards"] == 1 and cleared["revlog"] == 0 and cleared["epoch"] != epoch
    cleared_image = _assemble(cleared, cleared_chunks)
    assert len(cleared_image) == 32 + 20


def test_export_before_begin_is_rejected():
    result = run_sim(commands="XCHUNK 0\nXRESUME\nQUIT\n")
    assert result.returncode == 0, result.stderr
    assert "ERR RANGE" in result.stdout
    assert "ERR TRANSACTION" in result.stdout
