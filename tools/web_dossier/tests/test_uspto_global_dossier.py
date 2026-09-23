"""USPTO Global Dossier public JSON API provider tests (offline, fixtures)."""
from pathlib import Path

from web_dossier.models import DocumentType, ResultCode
from web_dossier.providers.uspto_global_dossier import parse_uspto_doc_payload

FIXTURE = (Path(__file__).parents[1] /
           "fixtures/uspto_global_dossier/cn2025469601_payload.json")


def test_parse_uspto_original_official_events_only():
    outcome = parse_uspto_doc_payload(
        FIXTURE.read_text(encoding="utf-8"), "CN202510469601.5", "CN120134203A"
    )
    assert outcome.code == ResultCode.OK
    assert [(d.document_type, d.official_date) for d in outcome.documents] == [
        (DocumentType.OFFICE_ACTION_FIRST, "2026-05-23")
    ]
    doc = outcome.documents[0]
    assert doc.document_version == "ORIGINAL"
    assert doc.source == "uspto_global_dossier"
    assert doc.remote_document_id == (
        "20251046960152104012026052310110680664683123_CN"
    )
    assert doc.document_code == "210401-CN"
    # canonical Chinese title so cross-provider event_keys collapse
    assert doc.document_title == "第一次审查意见通知书"
    assert len(doc.event_key()) == 64


def test_uspto_rate_limit_is_surfaced():
    outcome = parse_uspto_doc_payload(
        '{\n"ERROR" : "429 - CLOUDFRONT RATE LIMITED, TRY AGAIN LATER"\n}',
        "CN202510469601.5", "")
    assert outcome.code == ResultCode.RATE_LIMITED


def test_uspto_blank_body_is_temporary():
    outcome = parse_uspto_doc_payload("\r\n\r\n\r\n", "CN202510469601.5", "")
    assert outcome.code == ResultCode.TEMPORARY_ERROR


def test_uspto_missing_cn_member_is_case_not_found():
    outcome = parse_uspto_doc_payload(
        '{"country": "CN", "list": [{"countryCode": "US"}]}', "CN202510469601.5", "")
    assert outcome.code == ResultCode.CASE_NOT_FOUND


def test_uspto_sse_fallback_parses_docs():
    sse = ('data:{"docs":[{"docCode": "210401-CN", '
           '"docDesc": "First notice of examination opinions (ORIGINAL)", '
           '"docId": "20251046960152104012026052310110680664683123_CN", '
           '"legalDateStr": "05/23/2026"}]}\n\n')
    outcome = parse_uspto_doc_payload(sse, "CN202510469601.5", "")
    assert outcome.code == ResultCode.OK
    assert outcome.documents[0].official_date == "2026-05-23"
