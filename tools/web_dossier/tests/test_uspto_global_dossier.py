from pathlib import Path

import pytest

from web_dossier.browser.manager import BrowserManager
from web_dossier.models import DocumentType, ResultCode
from web_dossier.providers.uspto_global_dossier import (
    APP_URL,
    PUB_URL,
    UsptoGlobalDossierProvider,
    parse_uspto_document_list,
)


FIXTURE = (
    Path(__file__).parents[1]
    / "fixtures/uspto_global_dossier/cn202510469601_documents.html"
)


def _document_page(rows: str = "", empty_state: str = "") -> str:
    return f"""
    <html><head><title>Global Dossier</title></head>
    <body data-page="global-dossier-document-list">
      <h1>Global Dossier</h1>
      <table aria-label="Document list">
        <thead><tr>
          <th>Date</th><th>Document Description</th><th>Document Code</th>
          <th>Document ID</th><th>Direction</th>
        </tr></thead>
        <tbody>{rows}</tbody>
      </table>
      {empty_state}
    </body></html>
    """


def _row(date: str, title: str, code: str = "210401-CN",
         remote_id: str = "doc-1", direction: str = "Official") -> str:
    return f"""
    <tr><td>{date}</td><td>{title}</td><td>{code}</td>
    <td>{remote_id}</td><td>{direction}</td></tr>
    """


def test_parse_uspto_original_official_events_only():
    outcome = parse_uspto_document_list(
        FIXTURE.read_text(encoding="utf-8"), "CN202510469601.5", "CN120134203A"
    )
    assert outcome.code == ResultCode.OK
    assert [(d.document_type, d.official_date) for d in outcome.documents] == [
        (DocumentType.OFFICE_ACTION_FIRST, "2026-05-23")
    ]
    doc = outcome.documents[0]
    assert doc.document_version == "ORIGINAL"
    assert doc.source == "uspto_global_dossier"
    assert doc.document_code == "210401-CN"
    assert doc.remote_document_id
    assert doc.direction == "official"
    assert doc.document_title == "第一次审查意见通知书"
    assert doc.source_trace == ["uspto_global_dossier"]


def test_uspto_intercept_is_not_no_change():
    outcome = parse_uspto_document_list(
        "<html><body>Access Denied</body></html>", "CN202510469601.5", ""
    )
    assert outcome.code == ResultCode.ACCESS_DENIED


@pytest.mark.parametrize("html", ["", " ", "\n\t"])
def test_empty_response_is_temporary_error(html):
    outcome = parse_uspto_document_list(html, "CN202510469601.5", "")
    assert outcome.code == ResultCode.TEMPORARY_ERROR


@pytest.mark.parametrize(
    "body",
    [
        "Sign in to Global Dossier",
        "Login required",
        "Security verification required",
        "Verify you are human",
    ],
)
def test_login_and_security_pages_are_access_denied(body):
    outcome = parse_uspto_document_list(f"<html><body>{body}</body></html>")
    assert outcome.code == ResultCode.ACCESS_DENIED


def test_global_dossier_without_document_table_is_structure_changed():
    outcome = parse_uspto_document_list(
        "<html><title>Global Dossier</title><body><h1>Global Dossier</h1></body></html>"
    )
    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED


def test_explicit_empty_state_with_verified_headers_is_ok():
    html = _document_page(
        empty_state='<div class="empty-state">No documents found</div>'
    )
    outcome = parse_uspto_document_list(html, "CN202510469601.5", "")
    assert outcome.code == ResultCode.OK
    assert outcome.documents == []


def test_unparseable_date_does_not_create_document():
    rows = _row(
        "not-a-date",
        "First notice of examination opinions (ORIGINAL)",
        remote_id="bad-date",
    ) + _row(
        "2026-05-23",
        "Second notice of examination opinions (ORIGINAL)",
        code="210402-CN",
        remote_id="good-date",
    )
    outcome = parse_uspto_document_list(_document_page(rows))
    assert outcome.code == ResultCode.OK
    assert [document.remote_document_id for document in outcome.documents] == [
        "good-date"
    ]


def test_all_candidate_official_dates_failing_is_date_parse_failed():
    html = _document_page(
        _row(
            "pending",
            "First notice of examination opinions (ORIGINAL)",
            remote_id="bad-date",
        )
    )
    outcome = parse_uspto_document_list(html, "CN202510469601.5", "")
    assert outcome.code == ResultCode.DATE_PARSE_FAILED
    assert outcome.documents == []


def test_non_remindable_official_row_still_counts_for_date_failure():
    html = _document_page(
        _row(
            "pending",
            "检索报告 (ORIGINAL)",
            code="110101-CN",
            remote_id="bad-search-date",
        )
    )
    outcome = parse_uspto_document_list(html, "CN202510469601.5", "")
    assert outcome.code == ResultCode.DATE_PARSE_FAILED
    assert outcome.documents == []


class _FakePage:
    def __init__(self, html: str):
        self._html = html
        self.waited_for = None

    def wait_for_load_state(self, state, timeout):
        self.waited_for = (state, timeout)

    def content(self):
        return self._html


class _FakeBrowserManager:
    def __init__(self, html: str):
        self.page = _FakePage(html)
        self.urls = []

    def open_page(self, url, cancel):
        self.urls.append(url)
        return self.page


def test_provider_prefers_normalized_application_url():
    manager = _FakeBrowserManager(FIXTURE.read_text(encoding="utf-8"))
    provider = UsptoGlobalDossierProvider(browser_manager=manager)
    outcome = provider.list_documents(
        " cn 202510469601.5 ", "CN120134203A", None
    )
    assert outcome.code == ResultCode.OK
    assert manager.urls == [APP_URL.format(application="202510469601.5")]
    assert manager.page.waited_for[0] == "domcontentloaded"


def test_provider_uses_publication_url_when_application_is_unusable():
    manager = _FakeBrowserManager(FIXTURE.read_text(encoding="utf-8"))
    provider = UsptoGlobalDossierProvider(browser_manager=manager)
    outcome = provider.list_documents("not-an-application", "cn 120134203 a", None)
    assert outcome.code == ResultCode.OK
    assert manager.urls == [PUB_URL.format(publication="120134203A")]
    assert outcome.documents[0].application_number == ""
    assert outcome.documents[0].publication_number == "CN120134203A"


def test_provider_does_not_open_browser_for_unrecognized_numbers():
    manager = _FakeBrowserManager(FIXTURE.read_text(encoding="utf-8"))
    provider = UsptoGlobalDossierProvider(browser_manager=manager)
    outcome = provider.list_documents("bad-app", "bad-publication", None)
    assert outcome.code == ResultCode.RESOLVE_FAILED
    assert manager.urls == []


def test_provider_open_failure_is_network_error():
    manager = _FakeBrowserManager("")
    manager.open_page = lambda url, cancel: None
    provider = UsptoGlobalDossierProvider(browser_manager=manager)
    outcome = provider.list_documents("CN202510469601.5", "", None)
    assert outcome.code == ResultCode.NETWORK_ERROR


def test_provider_public_page_auth_and_download_contract():
    provider = UsptoGlobalDossierProvider(
        browser_manager=_FakeBrowserManager(FIXTURE.read_text(encoding="utf-8"))
    )
    assert provider.health_check()
    assert provider.check_auth(None) == ResultCode.OK
    assert provider.ensure_login(None) == ResultCode.OK
    resolved = provider.resolve_case("CN202510469601.5", "CN120134203A", None)
    assert resolved.code == ResultCode.OK
    assert resolved.resolved_application_number == "CN202510469601.5"
    assert provider.download_document(None, "unused", None) == (
        ResultCode.UNSUPPORTED_JURISDICTION
    )


def test_browser_manager_can_skip_system_browser_takeover(monkeypatch, tmp_path):
    launched = {}

    class FakePage:
        url = "about:blank"

        def is_closed(self):
            return False

    class FakeContext:
        pages = [FakePage()]

        def new_page(self):
            return self.pages[0]

    class FakeChromium:
        def launch_persistent_context(self, profile_dir, *, headless):
            launched.update(profile_dir=profile_dir, headless=headless)
            return FakeContext()

    class FakePlaywright:
        chromium = FakeChromium()

    class FakeStarter:
        def start(self):
            return FakePlaywright()

    monkeypatch.setattr("playwright.sync_api.sync_playwright", lambda: FakeStarter())
    manager = BrowserManager(
        "uspto_global_dossier",
        prefer_system_browser=False,
        headless=True,
        profile_dir=str(tmp_path / "profile"),
    )
    monkeypatch.setattr(
        manager, "_attach", lambda *args, **kwargs: pytest.fail("unexpected attach")
    )
    monkeypatch.setattr(
        manager,
        "_spawn_and_attach",
        lambda: pytest.fail("unexpected system browser spawn"),
    )

    assert manager.launch()
    assert launched == {
        "profile_dir": str(tmp_path / "profile"),
        "headless": True,
    }
