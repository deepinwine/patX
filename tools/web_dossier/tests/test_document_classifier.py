import datetime

from web_dossier.document_classifier import classify_cn_title, oa_title_cn
from web_dossier.models import DocumentType


def test_office_action_ordinals():
    cases = [
        ("第一次审查意见通知书", DocumentType.OFFICE_ACTION_FIRST, 1),
        ("第二次审查意见通知书", DocumentType.OFFICE_ACTION_SECOND, 2),
        ("第3次审查意见通知书", DocumentType.OFFICE_ACTION_NTH, 3),
        ("第三次审查意见通知书", DocumentType.OFFICE_ACTION_NTH, 3),
        ("第１０次审查意见通知书", DocumentType.OFFICE_ACTION_NTH, 10),  # fullwidth
        ("审查意见通知书", DocumentType.OFFICE_ACTION_UNKNOWN, 0),
        ("第二次 审查意见 通知书", DocumentType.OFFICE_ACTION_SECOND, 2),  # spacing noise
        ("一通", DocumentType.OFFICE_ACTION_FIRST, 1),
    ]
    for title, expected_type, ordinal in cases:
        got_type, got_ord, _conf = classify_cn_title(title)
        assert got_type == expected_type, title
        assert got_ord == ordinal, title


def test_other_document_types():
    assert classify_cn_title("驳回决定")[0] == DocumentType.REJECTION_DECISION
    assert classify_cn_title("授权通知")[0] == DocumentType.GRANT_NOTICE
    assert classify_cn_title("授予专利权通知")[0] == DocumentType.GRANT_NOTICE
    assert classify_cn_title("补正通知书")[0] == DocumentType.CORRECTION_NOTICE
    assert classify_cn_title("意见陈述书")[0] == DocumentType.RESPONSE_TO_OFFICE_ACTION
    assert classify_cn_title("权利要求书全文替换文件")[0] == DocumentType.CLAIMS_AMENDMENT
    assert classify_cn_title("第一次检索报告")[0] == DocumentType.SEARCH_REPORT
    # An OA never masquerades as these:
    assert classify_cn_title("缴费通知书")[0] == DocumentType.OTHER_OFFICIAL
    assert classify_cn_title("不明文件")[0] == DocumentType.UNKNOWN


def test_canonical_oa_titles():
    assert oa_title_cn(DocumentType.OFFICE_ACTION_FIRST, 1) == "第一次审查意见通知书"
    assert oa_title_cn(DocumentType.OFFICE_ACTION_SECOND, 2) == "第二次审查意见通知书"
    assert oa_title_cn(DocumentType.OFFICE_ACTION_NTH, 3) == "第三次审查意见通知书"
    assert oa_title_cn(DocumentType.OFFICE_ACTION_UNKNOWN, 0) == "审查意见通知书"


def test_latest_oa_is_true_oa():
    from web_dossier.models import ProsecutionDocument, pick_latest_office_action
    docs = [
        ProsecutionDocument(document_type=DocumentType.SEARCH_REPORT, official_date="2026-09-18"),
        ProsecutionDocument(document_type=DocumentType.OFFICE_ACTION_FIRST, official_date="2026-03-12"),
        ProsecutionDocument(document_type=DocumentType.OFFICE_ACTION_SECOND, official_date="2026-09-18", oa_ordinal=2),
        ProsecutionDocument(document_type=DocumentType.RESPONSE_TO_OFFICE_ACTION, official_date="2026-09-19"),
        ProsecutionDocument(document_type=DocumentType.OFFICE_ACTION_UNKNOWN, official_date=""),  # no date
    ]
    latest = pick_latest_office_action(docs)
    # same-day tie: the OA with the higher ordinal wins over the search report
    assert latest is docs[2]

    none = pick_latest_office_action(docs[:1])
    assert none is None


def test_dates():
    from web_dossier.models import normalize_date
    today = datetime.date(2026, 9, 20)
    assert normalize_date("2026-09-18", today) == "2026-09-18"
    assert normalize_date("2026.09.18", today) == "2026-09-18"
    assert normalize_date("2026/09/18", today) == "2026-09-18"
    assert normalize_date("2026年09月18日", today) == "2026-09-18"
    assert normalize_date("２０２６年９月１８日", today) == "2026-09-18"  # fullwidth
    assert normalize_date("2026-02-30", today) == ""      # invalid calendar
    assert normalize_date("2027-01-01", today) == ""      # future: refuse
    assert normalize_date("", today) == ""
    assert normalize_date("无日期", today) == ""


def test_fingerprint_dedup():
    from web_dossier.models import ProsecutionDocument
    a = ProsecutionDocument(document_title="第二次审查意见通知书", official_date="2026-09-18",
                            application_number="CN202410123456.7")
    b = ProsecutionDocument(document_title="第二次审查意见通知书", official_date="2026-09-18",
                            application_number="CN202410123456.7")
    c = ProsecutionDocument(document_title="第二次审查意见通知书", official_date="2026-09-19",
                            application_number="CN202410123456.7")
    assert a.fingerprint_value() == b.fingerprint_value()
    assert a.fingerprint_value() != c.fingerprint_value()
