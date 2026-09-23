"""Unified official-event classification shared by every dossier provider."""
from web_dossier.document_classifier import classify_official_document, event_title_cn
from web_dossier.models import Confidence, DocumentType, ProsecutionDocument


def test_global_dossier_titles_and_codes():
    assert classify_official_document(
        "First notice of examination opinions (ORIGINAL)", "210401-CN"
    ) == (DocumentType.OFFICE_ACTION_FIRST, 1, Confidence.HIGH)
    assert classify_official_document(
        "Second notice of examination opinions (ORIGINAL)", ""
    )[:2] == (DocumentType.OFFICE_ACTION_SECOND, 2)
    assert classify_official_document("Decision to reject (ORIGINAL)", "")[:2] == (
        DocumentType.REJECTION_DECISION, 0
    )
    assert classify_official_document("Notification to grant patent right (ORIGINAL)", "")[:2] == (
        DocumentType.GRANT_NOTICE, 0
    )
    assert classify_official_document("Notification to make rectification (ORIGINAL)", "")[:2] == (
        DocumentType.CORRECTION_NOTICE, 0
    )


def test_final_is_not_an_oa_ordinal():
    doc_type, ordinal, confidence = classify_official_document(
        "Final rejection decision (ORIGINAL)", ""
    )
    assert (doc_type, ordinal, confidence) == (
        DocumentType.REJECTION_DECISION, 0, Confidence.HIGH
    )


def test_code_and_title_disagree_is_unknown_low():
    doc_type, ordinal, confidence = classify_official_document(
        "Notification to grant patent right (ORIGINAL)", "210401-CN"
    )
    assert (doc_type, confidence) == (DocumentType.UNKNOWN, Confidence.LOW)


def test_cross_provider_event_key_ignores_provider_specific_id():
    uspto = ProsecutionDocument(
        source="uspto_global_dossier", application_number="CN202510469601.5",
        document_type=DocumentType.OFFICE_ACTION_FIRST,
        document_title="第一次审查意见通知书", official_date="2026-05-23",
        remote_document_id="uspto-123",
    )
    epo = ProsecutionDocument(
        source="epo_global_dossier", application_number="CN202510469601.5",
        document_type=DocumentType.OFFICE_ACTION_FIRST,
        document_title="第一次审查意见通知书", official_date="2026-05-23",
        remote_document_id="epo-456",
    )
    assert uspto.event_key() == epo.event_key()
    assert event_title_cn(DocumentType.REJECTION_DECISION, 0, "") == "驳回决定"
    assert event_title_cn(DocumentType.GRANT_NOTICE, 0, "") == "授权通知"
    assert event_title_cn(DocumentType.CORRECTION_NOTICE, 0, "") == "补正通知"
    assert event_title_cn(DocumentType.OTHER_OFFICIAL, 0, "费用减缴审批") == "费用减缴审批"
    assert event_title_cn(DocumentType.OTHER_OFFICIAL, 0, "") == "其他官方通知"


def test_document_dict_carries_new_fields():
    doc = ProsecutionDocument(document_code="210401-CN", source="uspto_global_dossier")
    d = doc.to_dict()
    assert d["document_version"] == "ORIGINAL"
    assert d["document_code"] == "210401-CN"
    assert d["source_trace"] == []
    assert len(d["event_key"]) == 64
