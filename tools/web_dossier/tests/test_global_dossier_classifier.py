from dataclasses import replace

import pytest

from web_dossier.document_classifier import (
    classify_cn_title,
    classify_official_document,
    event_title_cn,
    oa_title_cn,
)
from web_dossier.models import Confidence, DocumentType, ProsecutionDocument, ResultCode


def test_global_dossier_titles_and_codes():
    assert classify_official_document(
        "First notice of examination opinions (ORIGINAL)", "210401-CN"
    ) == (DocumentType.OFFICE_ACTION_FIRST, 1, Confidence.HIGH)
    assert classify_official_document(
        "Second notice of examination opinions (ORIGINAL)", ""
    ) == (DocumentType.OFFICE_ACTION_SECOND, 2, Confidence.HIGH)
    assert classify_official_document("Decision to reject (ORIGINAL)", "") == (
        DocumentType.REJECTION_DECISION,
        0,
        Confidence.HIGH,
    )
    assert classify_official_document(
        "Notification to grant patent right (ORIGINAL)", ""
    ) == (DocumentType.GRANT_NOTICE, 0, Confidence.HIGH)
    assert classify_official_document(
        "Notification to make rectification (ORIGINAL)", ""
    ) == (DocumentType.CORRECTION_NOTICE, 0, Confidence.HIGH)


def test_final_is_not_an_oa_ordinal():
    doc_type, ordinal, confidence = classify_official_document(
        "Final rejection decision (ORIGINAL)", ""
    )
    assert (doc_type, ordinal, confidence) == (
        DocumentType.REJECTION_DECISION,
        0,
        Confidence.HIGH,
    )


def test_final_rejection_without_decision_is_still_a_rejection():
    assert classify_official_document("Final rejection (ORIGINAL)", "") == (
        DocumentType.REJECTION_DECISION,
        0,
        Confidence.HIGH,
    )


def test_final_rejection_conflicts_with_first_oa_code():
    assert classify_official_document(
        "Final rejection (ORIGINAL)", "210401-CN"
    ) == (DocumentType.UNKNOWN, 0, Confidence.LOW)


@pytest.mark.parametrize(
    "title",
    [
        "Non-final rejection (ORIGINAL)",
        "Non final rejection (ORIGINAL)",
        "Non‑final rejection (ORIGINAL)",
        "Non–final rejection (ORIGINAL)",
    ],
)
def test_non_final_rejection_is_not_a_rejection_decision(title):
    assert classify_official_document(title, "") == (
        DocumentType.UNKNOWN,
        0,
        Confidence.LOW,
    )


@pytest.mark.parametrize(
    "title",
    [
        "Response to final rejection decision",
        "Response to notification to grant patent right",
        "Request for correction",
        "Amendment after notification to grant (TRANSLATED)",
    ],
)
def test_applicant_english_titles_are_not_official_events(title):
    assert classify_official_document(title, "") == (
        DocumentType.UNKNOWN,
        0,
        Confidence.LOW,
    )


def test_code_and_title_conflict_requires_manual_classification():
    assert classify_official_document(
        "Second notice of examination opinions (ORIGINAL)", "210401-CN"
    ) == (DocumentType.UNKNOWN, 0, Confidence.LOW)


@pytest.mark.parametrize(
    "title",
    [
        "Notification to grant (ORIGINAL)",
        "Registration formalities (ORIGINAL)",
    ],
)
def test_additional_explicit_grant_titles(title):
    assert classify_official_document(title, "") == (
        DocumentType.GRANT_NOTICE,
        0,
        Confidence.HIGH,
    )


@pytest.mark.parametrize(
    ("title", "ordinal"),
    [
        ("Third notice of examination opinions (ORIGINAL)", 3),
        ("Fourth office action (ORIGINAL)", 4),
        ("Tenth examination opinions (ORIGINAL)", 10),
    ],
)
def test_later_english_office_actions_preserve_ordinal(title, ordinal):
    assert classify_official_document(title, "") == (
        DocumentType.OFFICE_ACTION_NTH,
        ordinal,
        Confidence.HIGH,
    )


def test_cross_provider_event_key_ignores_provider_specific_id():
    uspto = ProsecutionDocument(
        source="uspto_global_dossier",
        application_number="CN202510469601.5",
        document_type=DocumentType.OFFICE_ACTION_FIRST,
        document_title="第一次审查意见通知书",
        official_date="2026-05-23",
        remote_document_id="uspto-123",
    )
    epo = ProsecutionDocument(
        source="epo_global_dossier",
        application_number="CN202510469601.5",
        document_type=DocumentType.OFFICE_ACTION_FIRST,
        document_title="第一次审查意见通知书",
        official_date="2026-05-23",
        remote_document_id="epo-456",
    )
    assert uspto.event_key() == epo.event_key()
    assert event_title_cn(DocumentType.REJECTION_DECISION, 0, "") == "驳回决定"


@pytest.mark.parametrize(
    ("document_type", "ordinal", "first_title", "second_title"),
    [
        (
            DocumentType.OFFICE_ACTION_NTH,
            3,
            "第3次审查意见通知书",
            "第三次审查意见通知书",
        ),
        (
            DocumentType.GRANT_NOTICE,
            0,
            "办理登记手续通知书",
            "授权通知",
        ),
        (
            DocumentType.CORRECTION_NOTICE,
            0,
            "补正通知书",
            "补正通知",
        ),
    ],
)
def test_event_key_normalizes_semantically_equivalent_titles(
    document_type, ordinal, first_title, second_title
):
    first = ProsecutionDocument(
        jurisdiction="CN",
        application_number="CN202510469601.5",
        document_type=document_type,
        oa_ordinal=ordinal,
        official_date="2026-05-23",
        document_title=first_title,
    )
    second = replace(first, document_title=second_title)
    assert first.event_key() == second.event_key()


def test_event_key_normalizes_stable_identity_text_fields():
    document = ProsecutionDocument(
        jurisdiction="CN",
        application_number="CN202510469601.5",
        document_type=DocumentType.REJECTION_DECISION,
        official_date="2026-05-23",
        document_title="驳回决定",
    )
    noisy = replace(
        document,
        jurisdiction=" cn ",
        application_number=" CN202510469601.5 ",
        official_date=" 2026-05-23 ",
    )
    assert document.event_key() == noisy.event_key()


def test_event_key_uses_all_official_event_identity_fields():
    baseline = ProsecutionDocument(
        jurisdiction="CN",
        application_number="CN202510469601.5",
        source="cnipa",
        remote_document_id="cnipa-123",
        document_type=DocumentType.OFFICE_ACTION_NTH,
        oa_ordinal=3,
        official_date="2026-05-23",
        document_title="第三次 审查意见通知书",
    )
    equivalent = replace(
        baseline,
        source="epo_global_dossier",
        remote_document_id="epo-456",
        document_title="第三次审查意见通知书",
    )
    assert baseline.event_key() == equivalent.event_key()

    for field_name, value in (
        ("jurisdiction", "EP"),
        ("application_number", "CN202510469602.3"),
        ("document_type", DocumentType.OFFICE_ACTION_SECOND),
        ("oa_ordinal", 4),
        ("official_date", "2026-05-24"),
    ):
        assert baseline.event_key() != replace(baseline, **{field_name: value}).event_key()

    first_other = replace(
        baseline,
        document_type=DocumentType.OTHER_OFFICIAL,
        oa_ordinal=0,
        document_title="缴费通知书",
    )
    second_other = replace(first_other, document_title="恢复权利通知书")
    assert first_other.event_key() != second_other.event_key()


def test_serialization_exposes_official_event_metadata_and_legacy_fingerprint():
    document = ProsecutionDocument(
        document_code="210401-CN",
        document_version="ORIGINAL",
        source_trace=["uspto_global_dossier", "epo_global_dossier"],
    )
    serialized = document.to_dict()
    assert serialized["document_code"] == "210401-CN"
    assert serialized["document_version"] == "ORIGINAL"
    assert serialized["source_trace"] == ["uspto_global_dossier", "epo_global_dossier"]
    assert serialized["event_key"] == document.event_key()
    assert serialized["fingerprint"] == document.fingerprint_value()
    assert ResultCode.NEW_OFFICIAL_EVENT.value == "NEW_OFFICIAL_EVENT"
    assert ResultCode.NEW_OFFICE_ACTION.value == "NEW_OFFICE_ACTION"


def test_chinese_classification_and_canonical_titles_remain_compatible():
    assert classify_cn_title("第一次审查意见通知书") == (
        DocumentType.OFFICE_ACTION_FIRST,
        1,
        Confidence.HIGH,
    )
    assert classify_official_document("第二次审查意见通知书") == (
        DocumentType.OFFICE_ACTION_SECOND,
        2,
        Confidence.HIGH,
    )
    assert oa_title_cn(DocumentType.OFFICE_ACTION_NTH, 3) == "第三次审查意见通知书"


@pytest.mark.parametrize(
    ("document_type", "ordinal", "fallback", "expected"),
    [
        (DocumentType.OFFICE_ACTION_SECOND, 2, "", "第二次审查意见通知书"),
        (DocumentType.REJECTION_DECISION, 0, "", "驳回决定"),
        (DocumentType.GRANT_NOTICE, 0, "", "授权通知"),
        (DocumentType.CORRECTION_NOTICE, 0, "", "补正通知"),
        (DocumentType.SEARCH_REPORT, 0, "检索报告原文", "检索报告原文"),
        (DocumentType.UNKNOWN, 0, "", "其他官方通知"),
    ],
)
def test_event_title_cn(document_type, ordinal, fallback, expected):
    assert event_title_cn(document_type, ordinal, fallback) == expected
