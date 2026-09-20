from pathlib import Path

from web_dossier.models import DocumentType, ResultCode
from web_dossier.providers.cnipa import (
    CNIPAWebProvider,
    looks_like_login_page,
    looks_logged_in,
    parse_dossier_documents,
)

FIXTURES = Path(__file__).resolve().parent.parent / "fixtures" / "cnipa"


def _fixture(name):
    return (FIXTURES / name).read_text("utf-8")


def test_parse_full_dossier():
    outcome = parse_dossier_documents(_fixture("dossier_list.html"),
                                      "CN202410123457.5", "CN119870049A")
    assert outcome.code == ResultCode.OK
    types = [d.document_type for d in outcome.documents]
    assert DocumentType.OFFICE_ACTION_SECOND in types
    assert DocumentType.OFFICE_ACTION_FIRST in types
    assert DocumentType.RESPONSE_TO_OFFICE_ACTION in types

    by_title = {d.raw_title: d for d in outcome.documents}
    assert by_title["第二次审查意见通知书"].official_date == "2026-09-18"
    assert by_title["第一次审查意见通知书"].official_date == "2026-03-12"


def test_latest_oa_from_fixture():
    outcome = parse_dossier_documents(_fixture("dossier_list.html"), "CN202410123457.5", "")
    provider = CNIPAWebProvider(fixture_dir=str(FIXTURES))
    latest = provider.get_latest_office_action(outcome)
    assert latest is not None
    assert latest.official_date == "2026-09-18"
    assert latest.oa_ordinal == 2


def test_parse_no_oa_fixture_dates_formats():
    outcome = parse_dossier_documents(_fixture("dossier_list_no_oa.html"), "CN2023100987654.1", "")
    assert outcome.code == ResultCode.OK
    dates = {d.raw_title: d.official_date for d in outcome.documents}
    # 2026年08月14日 / 2026.06.30 / 2026/01/05 all normalize
    assert dates["补正通知书"] == "2026-08-14"
    assert dates["第一次检索报告"] == "2026-06-30"
    assert dates["权利要求书全文替换文件"] == "2026-01-05"
    assert all(not d.document_type.is_office_action for d in outcome.documents)


def test_structure_change_detected():
    outcome = parse_dossier_documents("<html><body><p>维护中</p></body></html>", "X", "")
    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED


def test_login_page_detection():
    login = _fixture("login_page.html")
    dossier = _fixture("dossier_list.html")
    assert looks_like_login_page(login)
    assert not looks_logged_in(login)
    assert looks_logged_in(dossier)


def test_fixture_mode_provider_flow():
    provider = CNIPAWebProvider(fixture_dir=str(FIXTURES))
    assert provider.health_check()
    import threading
    cancel = threading.Event()
    assert provider.check_auth(cancel) == ResultCode.OK

    # publication-only input resolves through the fixture's 申请号 line
    outcome = provider.list_documents("", "CN119870049A", cancel)
    assert outcome.code == ResultCode.OK
    assert outcome.resolved_application_number == "2024101234575"
    assert any(d.document_type.is_office_action for d in outcome.documents)


def test_resolve_failure_on_garbage():
    provider = CNIPAWebProvider(fixture_dir=str(FIXTURES))
    import threading
    outcome = provider.resolve_case("", "", threading.Event())
    assert outcome.code == ResultCode.RESOLVE_FAILED
