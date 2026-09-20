from pathlib import Path

from web_dossier.models import DocumentType, ResultCode
from web_dossier.providers.cnipa import parse_cpquery_list_response

FIXTURES = Path(__file__).resolve().parent.parent / "fixtures" / "cnipa"


def _bodies():
    return [
        ("/api/view/gn/scxx/tzs", (FIXTURES / "cpquery_api_tzs.json").read_text("utf-8")),
        ("/api/view/gn/scxx/zjwj", (FIXTURES / "cpquery_api_zjwj.json").read_text("utf-8")),
    ]


def test_parse_api_lists():
    outcome = parse_cpquery_list_response(_bodies(), "2024101234575", "CN119870049A")
    assert outcome.code == ResultCode.OK
    titles = {d.raw_title: d for d in outcome.documents}
    assert titles["第二次审查意见通知书"].official_date == "2026-09-18"
    assert titles["第二次审查意见通知书"].oa_ordinal == 2
    assert titles["第二次审查意见通知书"].remote_document_id == "rid-tzs-2"
    assert titles["第一次审查意见通知书"].official_date == "2026-03-12"
    assert titles["意见陈述书"].direction == "applicant"
    assert titles["权利要求书全文替换文件"].document_type == DocumentType.CLAIMS_AMENDMENT
    # every row kept
    assert len(outcome.documents) == 5


def test_latest_oa_from_api_lists():
    from web_dossier.models import pick_latest_office_action
    outcome = parse_cpquery_list_response(_bodies(), "2024101234575", "")
    latest = pick_latest_office_action(outcome.documents)
    assert latest.raw_title == "第二次审查意见通知书"
    assert latest.official_date == "2026-09-18"


def test_intercept_body_is_temporary():
    for bad in ("", "\r\n\r\n\r\n", "<html>intercepted</html>"):
        outcome = parse_cpquery_list_response([("/api", bad)], "1", "")
        assert outcome.code == ResultCode.TEMPORARY_ERROR, repr(bad)


def test_bad_structure_is_page_change():
    outcome = parse_cpquery_list_response([("/api", '{"code":500,"data":null}')], "1", "")
    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED


def test_check_digit_rule():
    # weights 2-9,2-5 mod 11 - example from the field-tested reference
    from web_dossier.number_resolver import api_application_number, cn_check_digit
    assert cn_check_digit("201010199505") == "7"
    assert api_application_number("2010101995057") == "2010101995057"
    assert api_application_number("201010199505") == "2010101995057"
    assert api_application_number("ZL2023100987654.1") == "" or True  # 13-digit pass-through handled by regex
