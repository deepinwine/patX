import json
from pathlib import Path

from web_dossier.providers.cnipa import (build_fetch_file_url, build_document_filename,
                                         parse_file_infos, sniff_format)

FIXTURES = Path(__file__).resolve().parent.parent / "fixtures" / "cnipa"


def test_fetch_url_never_encodes_slash():
    url = build_fetch_file_url({
        "osslujing": "tzs/2024/rid/000001.PNG", "wenjianhzm": "PNG",
        "timestamp": "1789000000", "sign": "abc==", "isDN": "true",
        "ds": "TZS", "wenjiandm": "100000"})
    assert "%2F" not in url
    assert url.startswith("/api/pcshoss/view/fetch-file?osslujing=tzs/2024/rid/000001.PNG")
    assert "sign=abc==" in url and "isDN=true" in url


def test_parse_file_infos():
    pages, hzm = parse_file_infos((FIXTURES / "cpquery_file_infos.json").read_text("utf-8"))
    assert hzm == "PNG"
    assert len(pages) == 3
    assert pages[0]["osslujing"].endswith("000001.PNG")
    assert pages[0]["isDN"] == "true" and pages[1]["isDN"] == "false"
    assert pages[0]["ds"] == "TZS" and pages[0]["wenjiandm"] == "100000"


def test_parse_file_infos_errors():
    assert parse_file_infos("")[0] is None
    assert parse_file_infos("<html>")[0] is None
    assert parse_file_infos(json.dumps({"code": 200, "data": {"ossLujingList": []}}))[0] is None


def test_magic_sniffing_overrides_manifest():
    # The manifest says PNG; the body decides.
    assert sniff_format(b"%PDF-1.7\n%patx") == "pdf"
    assert sniff_format(b"\x89PNG\r\n\x1a\nIHDR") == "png"
    assert sniff_format(b"GIF89a") == "unknown"


def test_filename_dedupe_and_layout():
    existing = set()
    def mk():
        return build_document_filename("2026-09-18", "TZS", "第二次审查意见通知书",
                                       ".pdf", lambda n: n in existing, pages=3)
    first = mk(); existing.add(first)
    second = mk()
    assert first == "2026-09-18_通知书_第二次审查意见通知书_3页.pdf"
    assert second.endswith("_3页_2.pdf")
    nodate = build_document_filename("", "ZJWJ", "意见陈述书", ".pdf", lambda n: False)
    assert nodate.startswith("无日期_中间文件_")
