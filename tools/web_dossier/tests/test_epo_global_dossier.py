"""EPO register Global Dossier provider - pure parser tests."""
from pathlib import Path

from web_dossier.models import DocumentType, ResultCode
from web_dossier.providers.epo_global_dossier import parse_epo_document_list
from web_dossier.providers.uspto_global_dossier import parse_uspto_doc_payload

FIXTURE = Path(__file__).parents[1] / "fixtures/epo_global_dossier/cn202510469601_documents.html"
USPTO_FIXTURE = (Path(__file__).parents[1] /
                 "fixtures/uspto_global_dossier/cn2025469601_payload.json")


def test_epo_keeps_original_and_drops_translation():
    outcome = parse_epo_document_list(FIXTURE.read_text(encoding="utf-8"),
                                      "CN202510469601.5", "CN120134203A")
    assert outcome.code == ResultCode.OK
    assert [(d.document_title, d.official_date) for d in outcome.documents] == [
        ("第一次审查意见通知书", "2026-05-23")
    ]
    assert outcome.documents[0].remote_document_id == (
        "20251046960152104012026052310110680664683123_CN"
    )
    assert outcome.documents[0].document_version == "ORIGINAL"
    assert outcome.documents[0].source == "epo_global_dossier"


def test_epo_and_uspto_collapse_to_same_event_key():
    epo = parse_epo_document_list(FIXTURE.read_text(encoding="utf-8"),
                                  "CN202510469601.5", "")
    uspto = parse_uspto_doc_payload(USPTO_FIXTURE.read_text(encoding="utf-8"),
                                    "CN202510469601.5", "")
    assert epo.documents[0].event_key() == uspto.documents[0].event_key()


def test_epo_blank_body_is_temporary():
    outcome = parse_epo_document_list("", "CN202510469601.5", "")
    assert outcome.code == ResultCode.TEMPORARY_ERROR


def test_epo_intercept_is_access_denied():
    outcome = parse_epo_document_list("<html>403 Forbidden</html>",
                                      "CN202510469601.5", "")
    assert outcome.code == ResultCode.ACCESS_DENIED


def test_epo_identity_without_table_is_structure_change():
    outcome = parse_epo_document_list(
        "<html><head><title>European Patent Register</title></head><body></body></html>",
        "CN202510469601.5", "")
    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED
