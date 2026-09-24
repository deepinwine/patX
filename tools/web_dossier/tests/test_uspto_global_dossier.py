import threading
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


def _document_page(
    rows: str = "",
    empty_state: str = "",
    application_number: str = "CN202510469601.5",
    publication_number: str = "CN120134203A",
) -> str:
    return f"""
    <html><head><title>Global Dossier</title></head>
    <body data-page="global-dossier-document-list">
      <h1>Global Dossier</h1>
      <main data-application-number="{application_number}"
            data-publication-number="{publication_number}">
        <p class="case-number">Application {application_number}</p>
        <p class="publication-number">Publication {publication_number}</p>
        <table aria-label="Document list">
          <thead><tr>
            <th>Date</th><th>Document Description</th><th>Document Code</th>
            <th>Document ID</th><th>Direction</th>
          </tr></thead>
          <tbody>{rows}</tbody>
        </table>
        {empty_state}
      </main>
    </body></html>
    """


def _row(date: str, title: str, code: str = "210401-CN",
         remote_id: str = "doc-1", direction: str = "Official") -> str:
    return f"""
    <tr><td>{date}</td><td>{title}</td><td>{code}</td>
    <td>{remote_id}</td><td>{direction}</td></tr>
    """


def _case_section(application_number: str, rows: str, css_class: str = "") -> str:
    return f"""
    <section class="case-dossier {css_class}"
             data-application-number="{application_number}">
      <p class="case-number">Application {application_number}</p>
      <table aria-label="Document list">
        <thead><tr>
          <th>Date</th><th>Document Description</th><th>Document Code</th>
          <th>Document ID</th><th>Direction</th>
        </tr></thead>
        <tbody>{rows}</tbody>
      </table>
    </section>
    """


def _multi_case_page(body: str, heading_number: str = "") -> str:
    return f"""
    <html><body><h1>Global Dossier {heading_number}</h1>{body}</body></html>
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


@pytest.mark.parametrize(
    "wrapper",
    [
        '<section hidden>{}</section>',
        '<section aria-hidden="true">{}</section>',
        '<section style="display: none">{}</section>',
        '<section style="visibility:hidden">{}</section>',
        '<template>{}</template>',
    ],
)
def test_hidden_ancestor_empty_state_is_not_an_explicit_empty_result(wrapper):
    hidden_empty = wrapper.format(
        '<div class="empty-state">No documents found</div>'
    )
    outcome = parse_uspto_document_list(
        _document_page(empty_state=hidden_empty), "CN202510469601.5", ""
    )
    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED


@pytest.mark.parametrize("tag", ["script", "style", "template"])
def test_non_rendered_captcha_text_does_not_trigger_access_denied(tag):
    html = _document_page(
        _row(
            "2026-05-23",
            "First notice of examination opinions (ORIGINAL)",
        )
    ).replace("</body>", f"<{tag}>captcha</{tag}></body>")
    outcome = parse_uspto_document_list(html, "CN202510469601.5", "")
    assert outcome.code == ResultCode.OK
    assert len(outcome.documents) == 1


def test_parser_rejects_document_list_for_a_different_case():
    html = _document_page(
        _row(
            "2026-05-23",
            "First notice of examination opinions (ORIGINAL)",
            remote_id="case-a-document",
        ),
        application_number="CN202510469601.5",
    )
    outcome = parse_uspto_document_list(html, "CN202510469602.3", "")
    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED
    assert outcome.documents == []


def test_target_heading_with_previous_case_table_is_not_ready():
    from web_dossier.providers.uspto_global_dossier import (
        inspect_uspto_document_state,
    )

    case_a_table = _case_section(
        "CN202510469601.5",
        _row(
            "2026-05-23",
            "First notice of examination opinions (ORIGINAL)",
            remote_id="case-a-document",
        ),
    )
    html = _multi_case_page(case_a_table, heading_number="CN202510469602.3")

    assert inspect_uspto_document_state(html, "CN202510469602.3") == "PENDING"
    outcome = parse_uspto_document_list(html, "CN202510469602.3", "")
    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED
    assert outcome.documents == []


def test_hidden_previous_table_before_visible_target_table_selects_target_only():
    case_a = _case_section(
        "CN202510469601.5",
        _row(
            "2026-05-23",
            "First notice of examination opinions (ORIGINAL)",
            remote_id="case-a-document",
        ),
        css_class="d-none",
    )
    case_b = _case_section(
        "CN202510469602.3",
        _row(
            "2026-05-24",
            "First notice of examination opinions (ORIGINAL)",
            remote_id="case-b-document",
        ),
    )
    html = _multi_case_page(case_a + case_b)

    outcome = parse_uspto_document_list(html, "CN202510469602.3", "")

    assert outcome.code == ResultCode.OK
    assert [document.remote_document_id for document in outcome.documents] == [
        "case-b-document"
    ]


def test_hidden_void_element_does_not_hide_following_document_table():
    html = _document_page(
        _row(
            "2026-05-23",
            "First notice of examination opinions (ORIGINAL)",
        )
    ).replace('<table aria-label="Document list">', (
        '<input type="hidden" hidden value="state">'
        '<table aria-label="Document list">'
    ))
    outcome = parse_uspto_document_list(html, "CN202510469601.5", "")
    assert outcome.code == ResultCode.OK
    assert len(outcome.documents) == 1


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


def test_non_remindable_official_row_date_failure_does_not_block():
    html = _document_page(
        _row(
            "pending",
            "检索报告 (ORIGINAL)",
            code="110101-CN",
            remote_id="bad-search-date",
        )
    )
    outcome = parse_uspto_document_list(html, "CN202510469601.5", "")
    assert outcome.code == ResultCode.OK
    assert outcome.documents == []


def test_invalid_remindable_date_is_not_masked_by_valid_non_remindable_row():
    rows = _row(
        "pending",
        "First notice of examination opinions (ORIGINAL)",
        remote_id="bad-oa-date",
    ) + _row(
        "2026-05-23",
        "检索报告 (ORIGINAL)",
        code="110101-CN",
        remote_id="valid-search-date",
    )
    outcome = parse_uspto_document_list(_document_page(rows))
    assert outcome.code == ResultCode.DATE_PARSE_FAILED
    assert outcome.documents == []


def test_applicant_to_office_direction_is_not_official():
    html = _document_page(
        _row(
            "2026-05-23",
            "First notice of examination opinions (ORIGINAL)",
            direction="Applicant to Office",
        )
    )
    outcome = parse_uspto_document_list(html, "CN202510469601.5", "")
    assert outcome.code == ResultCode.OK
    assert outcome.documents == []


class _FakePage:
    def __init__(
        self,
        html: str,
        *,
        ready_html: str = "",
        wait_error=None,
        evaluate_results=None,
        on_poll=None,
        legacy_wait_updates: bool = True,
    ):
        self._html = html
        self._ready_html = ready_html
        self._wait_error = wait_error
        self._evaluate_results = list(evaluate_results or [True])
        self._on_poll = on_poll
        self._legacy_wait_updates = legacy_wait_updates
        self.waited_for = None
        self.waited_for_function = None
        self.evaluated = []
        self.poll_waits = []
        self.content_calls = 0

    def wait_for_load_state(self, state, timeout):
        self.waited_for = (state, timeout)

    def wait_for_function(self, expression, *, timeout):
        self.waited_for_function = (expression, timeout)
        if self._wait_error is not None:
            raise self._wait_error
        if self._ready_html and self._legacy_wait_updates:
            self._html = self._ready_html

    def evaluate(self, expression, argument):
        self.evaluated.append((expression, argument))
        if self._wait_error is not None:
            raise self._wait_error
        if len(self._evaluate_results) > 1:
            return self._evaluate_results.pop(0)
        return self._evaluate_results[0]

    def wait_for_timeout(self, milliseconds):
        self.poll_waits.append(milliseconds)
        if self._on_poll is not None:
            self._on_poll()
        if self._ready_html:
            self._html = self._ready_html
            self._ready_html = ""

    def content(self):
        self.content_calls += 1
        return self._html


class _FakeBrowserManager:
    def __init__(self, html: str, *, status=None, ready_html: str = "",
                 wait_error=None, evaluate_results=None, on_poll=None,
                 spa_statuses=None, responses=None):
        self.page = _FakePage(
            html,
            ready_html=ready_html,
            wait_error=wait_error,
            evaluate_results=evaluate_results,
            on_poll=on_poll,
        )
        self.urls = []
        self.fresh_requests = []
        self.last_navigation_status = status
        self.last_spa_statuses = list(spa_statuses or [])
        self.responses = list(responses or [])

    def open_page(self, url, cancel, *, fresh=False, response_filter=None):
        self.urls.append(url)
        self.fresh_requests.append(fresh)
        for response in self.responses:
            if response_filter is None or response_filter(response):
                self.last_spa_statuses.append(response.status)
        return self.page


def test_provider_prefers_normalized_application_url():
    manager = _FakeBrowserManager(FIXTURE.read_text(encoding="utf-8"))
    provider = UsptoGlobalDossierProvider(browser_manager=manager)
    outcome = provider.list_documents(
        " cn 202510469601.5 ", "CN120134203A", None
    )
    assert outcome.code == ResultCode.OK
    assert manager.urls == [APP_URL.format(application="202510469601.5")]
    assert manager.page.waited_for is None
    assert manager.page.content_calls == 1


def test_provider_waits_for_spa_document_state_before_reading_html():
    shell = "<html><body><h1>Global Dossier</h1><div id='app'></div></body></html>"
    manager = _FakeBrowserManager(
        shell,
        ready_html=FIXTURE.read_text(encoding="utf-8"),
        evaluate_results=[False, True],
    )
    provider = UsptoGlobalDossierProvider(browser_manager=manager)

    outcome = provider.list_documents("CN202510469601.5", "", None)

    assert outcome.code == ResultCode.OK
    assert len(outcome.documents) == 1
    assert manager.page.content_calls == 2
    assert manager.page.poll_waits


def test_provider_spa_wait_timeout_is_structure_changed(monkeypatch):
    monkeypatch.setattr(
        "web_dossier.providers.uspto_global_dossier.SPA_WAIT_TIMEOUT_SECONDS",
        0.0,
    )
    manager = _FakeBrowserManager(
        "<html><body><h1>Global Dossier</h1><div id='app'></div></body></html>",
    )
    provider = UsptoGlobalDossierProvider(browser_manager=manager)

    outcome = provider.list_documents("CN202510469601.5", "", None)

    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED
    assert manager.page.content_calls == 1


def test_consecutive_cases_do_not_reuse_the_previous_case_table():
    case_a_html = _document_page(
        _row(
            "2026-05-23",
            "First notice of examination opinions (ORIGINAL)",
            remote_id="case-a-document",
        ),
        application_number="CN202510469601.5",
    )
    case_b_html = _document_page(
        _row(
            "2026-05-24",
            "First notice of examination opinions (ORIGINAL)",
            remote_id="case-b-document",
        ),
        application_number="CN202510469602.3",
    )

    class StaleThenFreshManager(_FakeBrowserManager):
        def __init__(self):
            super().__init__(case_a_html)
            self.calls = 0

        def open_page(self, url, cancel, *, fresh=False, response_filter=None):
            self.urls.append(url)
            self.fresh_requests.append(fresh)
            self.calls += 1
            if self.calls == 2:
                self.page._html = case_a_html
                self.page._ready_html = case_b_html
                self.page._evaluate_results = [False, True]
                self.page._legacy_wait_updates = False
            return self.page

    manager = StaleThenFreshManager()
    provider = UsptoGlobalDossierProvider(browser_manager=manager)

    first = provider.list_documents("CN202510469601.5", "", None)
    second = provider.list_documents("CN202510469602.3", "", None)

    assert [document.remote_document_id for document in first.documents] == [
        "case-a-document"
    ]
    assert second.code == ResultCode.OK
    assert [document.remote_document_id for document in second.documents] == [
        "case-b-document"
    ]
    assert second.documents[0].application_number == "CN202510469602.3"
    assert manager.fresh_requests == [True, True]


def test_cancel_during_spa_wait_returns_temporary_error():
    cancel = threading.Event()
    manager = _FakeBrowserManager(
        "<html><body><h1>Global Dossier</h1><div id='app'></div></body></html>",
        evaluate_results=[False, False],
        on_poll=cancel.set,
    )
    provider = UsptoGlobalDossierProvider(browser_manager=manager)

    outcome = provider.list_documents("CN202510469601.5", "", cancel)

    assert outcome.code == ResultCode.TEMPORARY_ERROR
    assert "取消" in outcome.message
    assert manager.page.poll_waits


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


def test_provider_navigation_exception_is_network_error():
    manager = _FakeBrowserManager("")

    def raise_navigation_error(url, cancel):
        raise OSError("navigation failed")

    manager.open_page = raise_navigation_error
    provider = UsptoGlobalDossierProvider(browser_manager=manager)

    outcome = provider.list_documents("CN202510469601.5", "", None)

    assert outcome.code == ResultCode.NETWORK_ERROR


@pytest.mark.parametrize(
    ("status", "expected"),
    [
        (401, ResultCode.ACCESS_DENIED),
        (403, ResultCode.ACCESS_DENIED),
        (429, ResultCode.RATE_LIMITED),
        (503, ResultCode.NETWORK_ERROR),
    ],
)
def test_provider_maps_navigation_block_status(status, expected):
    manager = _FakeBrowserManager(
        "<html><body><div id='app'></div></body></html>", status=status
    )
    provider = UsptoGlobalDossierProvider(browser_manager=manager)

    outcome = provider.list_documents("CN202510469601.5", "", None)

    assert outcome.code == expected
    assert manager.page.waited_for_function is None


def test_provider_maps_spa_child_request_rate_limit():
    class FakeRequest:
        resource_type = "fetch"
        post_data = '{"applicationNumber":"CN202510469601.5"}'

    class DossierResponse:
        status = 429
        url = "https://globaldossier.uspto.gov/api/documents"
        request = FakeRequest()

    manager = _FakeBrowserManager(
        FIXTURE.read_text(encoding="utf-8"),
        status=None,
        responses=[DossierResponse()],
    )
    provider = UsptoGlobalDossierProvider(browser_manager=manager)

    outcome = provider.list_documents("CN202510469601.5", "", None)

    assert outcome.code == ResultCode.RATE_LIMITED


def test_unrelated_same_origin_404_and_500_do_not_fail_valid_dossier():
    class FakeRequest:
        resource_type = "xhr"
        post_data = ""

    class UnrelatedResponse:
        status = 404
        url = "https://globaldossier.uspto.gov/api/user-preferences"
        request = FakeRequest()

    class UnrelatedProfileResponse:
        status = 500
        url = "https://globaldossier.uspto.gov/api/profile"
        request = FakeRequest()

    UnrelatedProfileResponse.request.post_data = (
        '{"applicationNumber":"CN202510469601.5"}'
    )

    manager = _FakeBrowserManager(
        FIXTURE.read_text(encoding="utf-8"),
        responses=[UnrelatedResponse(), UnrelatedProfileResponse()],
    )
    provider = UsptoGlobalDossierProvider(browser_manager=manager)

    outcome = provider.list_documents("CN202510469601.5", "", None)

    assert outcome.code == ResultCode.OK
    assert len(outcome.documents) == 1


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


def test_browser_manager_without_system_browser_forces_headless(monkeypatch, tmp_path):
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


def test_browser_manager_exposes_navigation_http_status(tmp_path):
    class FakeResponse:
        status = 403

    class FakePage:
        def on(self, event, callback):
            pass

        def goto(self, url, *, timeout, wait_until):
            return FakeResponse()

    manager = BrowserManager(
        "uspto_global_dossier",
        prefer_system_browser=False,
        profile_dir=str(tmp_path / "profile"),
    )
    manager._page = FakePage()

    assert manager.open_page("https://example.invalid", None) is manager._page
    assert manager.last_navigation_status == 403


def test_browser_manager_cnipa_default_does_not_install_spa_listener(tmp_path):
    events = []

    class FakeResponse:
        status = 200

    class FakePage:
        def on(self, event, callback):
            events.append(event)

        def goto(self, url, *, timeout, wait_until):
            return FakeResponse()

    manager = BrowserManager(
        "cnipa",
        profile_dir=str(tmp_path / "profile"),
    )
    page = FakePage()
    manager._page = page

    assert manager.open_page("https://cpquery.cponline.cnipa.gov.cn", None) is page
    assert events == []


def test_browser_manager_records_spa_status_when_hash_navigation_has_no_response(
    tmp_path,
):
    class FakeRequest:
        resource_type = "xhr"

    class FakeChildResponse:
        status = 429
        url = "https://globaldossier.uspto.gov/api/documents"
        request = FakeRequest()

    class FakePage:
        def on(self, event, callback):
            assert event == "response"
            self.response_callback = callback

        def goto(self, url, *, timeout, wait_until):
            self.response_callback(FakeChildResponse())
            return None

    manager = BrowserManager(
        "uspto_global_dossier",
        prefer_system_browser=False,
        profile_dir=str(tmp_path / "profile"),
    )
    manager._page = FakePage()

    assert manager.open_page("https://globaldossier.uspto.gov/#/result", None)
    assert manager.last_navigation_status is None
    assert manager.last_spa_statuses == [429]


def test_browser_manager_fresh_generation_ignores_old_and_unrelated_responses(
    tmp_path,
):
    class FakeRequest:
        resource_type = "xhr"

        def __init__(self, post_data):
            self.post_data = post_data

    class FakeResponse:
        def __init__(self, status, target):
            self.status = status
            self.url = "https://globaldossier.uspto.gov/api/documents"
            self.request = FakeRequest(target)

    class FakePage:
        def __init__(self):
            self.closed = False

        def on(self, event, callback):
            self.response_callback = callback

        def goto(self, url, *, timeout, wait_until):
            return None

        def close(self):
            self.closed = True

    initial_page = FakePage()
    case_a_page = FakePage()
    case_b_page = FakePage()

    class FakeContext:
        def __init__(self):
            self.pages_to_create = [case_a_page, case_b_page]

        def new_page(self):
            return self.pages_to_create.pop(0)

    manager = BrowserManager(
        "uspto_global_dossier",
        prefer_system_browser=False,
        profile_dir=str(tmp_path / "profile"),
    )
    manager._page = initial_page
    manager._context = FakeContext()

    filter_a = lambda response: "CN2025104696015" in response.request.post_data
    filter_b = lambda response: "CN2025104696023" in response.request.post_data
    manager.open_page(
        "https://globaldossier.uspto.gov/#/A",
        None,
        fresh=True,
        response_filter=filter_a,
    )
    manager.open_page(
        "https://globaldossier.uspto.gov/#/B",
        None,
        fresh=True,
        response_filter=filter_b,
    )

    case_a_page.response_callback(FakeResponse(429, "CN2025104696015"))
    case_b_page.response_callback(FakeResponse(404, "unrelated"))
    assert manager.last_spa_statuses == []

    case_b_page.response_callback(FakeResponse(429, "CN2025104696023"))
    assert manager.last_spa_statuses == [429]
    assert initial_page.closed
    assert case_a_page.closed


def test_browser_manager_cleans_playwright_when_persistent_launch_fails(
    monkeypatch, tmp_path
):
    stopped = []

    class FakeChromium:
        def launch_persistent_context(self, profile_dir, *, headless):
            raise RuntimeError("chromium launch failed")

    class FakePlaywright:
        chromium = FakeChromium()

        def stop(self):
            stopped.append(True)

    fake_playwright = FakePlaywright()

    class FakeStarter:
        def start(self):
            return fake_playwright

    monkeypatch.setattr("playwright.sync_api.sync_playwright", lambda: FakeStarter())
    manager = BrowserManager(
        "uspto_global_dossier",
        prefer_system_browser=False,
        profile_dir=str(tmp_path / "profile"),
    )

    with pytest.raises(RuntimeError, match="chromium launch failed"):
        manager.launch()

    assert stopped == [True]
    assert manager._pw is None
    assert manager._context is None
    assert manager._page is None


def test_browser_manager_close_orders_context_browser_and_playwright():
    closed = []

    class FakeContext:
        def close(self):
            closed.append("context")

    class FakeBrowser:
        def close(self):
            closed.append("browser")

    class FakePlaywright:
        def stop(self):
            closed.append("playwright")

    manager = BrowserManager("uspto_global_dossier")
    manager._context = FakeContext()
    manager._browser = FakeBrowser()
    manager._pw = FakePlaywright()
    manager._owns_context = True

    manager.close()

    assert closed == ["context", "browser", "playwright"]
