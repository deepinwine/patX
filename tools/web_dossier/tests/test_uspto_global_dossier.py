"""USPTO Global Dossier public page provider - pure parser tests."""
from pathlib import Path

from web_dossier.models import DocumentType, ResultCode
from web_dossier.providers.uspto_global_dossier import parse_uspto_document_list

FIXTURE = Path(__file__).parents[1] / "fixtures/uspto_global_dossier/cn202510469601_documents.html"


def test_parse_uspto_original_official_events_only():
    outcome = parse_uspto_document_list(
        FIXTURE.read_text(encoding="utf-8"), "CN202510469601.5", "CN120134203A"
    )
    assert outcome.code == ResultCode.OK
    assert [(d.document_type, d.official_date) for d in outcome.documents] == [
        (DocumentType.OFFICE_ACTION_FIRST, "2026-05-23")
    ]
    assert outcome.documents[0].document_version == "ORIGINAL"
    assert outcome.documents[0].source == "uspto_global_dossier"
    assert outcome.documents[0].remote_document_id == (
        "20251046960152104012026052310110680664683123_CN"
    )
    # canonical Chinese title so cross-provider event_keys collapse
    assert outcome.documents[0].document_title == "第一次审查意见通知书"
    assert len(outcome.documents[0].event_key()) == 64


def test_uspto_intercept_is_not_no_change():
    outcome = parse_uspto_document_list("<html>Access Denied</html>", "CN202510469601.5", "")
    assert outcome.code == ResultCode.ACCESS_DENIED


def test_uspto_blank_body_is_temporary():
    outcome = parse_uspto_document_list("\r\n\r\n\r\n", "CN202510469601.5", "")
    assert outcome.code == ResultCode.TEMPORARY_ERROR


def test_uspto_identity_without_table_is_structure_change():
    outcome = parse_uspto_document_list(
        "<html><head><title>Global Dossier</title></head><body></body></html>",
        "CN202510469601.5", "")
    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED


def test_uspto_explicit_no_documents_is_empty_ok():
    outcome = parse_uspto_document_list(
        "<html><head><title>Global Dossier</title></head>"
        "<body>No documents found for this application.</body></html>",
        "CN202510469601.5", "")
    assert outcome.code == ResultCode.OK
    assert outcome.documents == []
