from pathlib import Path
from threading import Event

import pytest

from web_dossier.models import Confidence, DocumentType, ResultCode
from web_dossier.providers.epo_global_dossier import (
    EPO_DOSSIER_URL,
    EpoGlobalDossierProvider,
    fetch_html_public,
    parse_epo_document_list,
)


FIXTURE = (
    Path(__file__).parents[1]
    / "fixtures/epo_global_dossier/cn202510469601_documents.html"
)


def _page(rows: str = "", *, empty: str = "", application="CN202510469601.5",
          publication="CN120134203A", headers: str = "") -> str:
    headers = headers or """
      <th>Date</th><th>Document description</th><th>Document code</th>
      <th>Document ID</th><th>Direction</th><th>Version</th>
    """
    return f"""
    <html><head><title>European Patent Register - Global Dossier</title></head>
    <body><h1>Global Dossier</h1>
      <p>Application number: {application}</p>
      <p>Publication number: {publication}</p>
      <table aria-label="Document list">
        <thead><tr>{headers}</tr></thead><tbody>{rows}</tbody>
      </table>{empty}
    </body></html>
    """


def _row(date="2026-05-23", title="First notice of examination opinions",
         code="210401-CN", remote_id="doc-1",
         direction="Patent office to applicant", version="ORIGINAL") -> str:
    return f"""
    <tr><td>{date}</td><td>{title}</td><td>{code}</td><td>{remote_id}</td>
    <td>{direction}</td><td>{version}</td></tr>
    """


def test_epo_keeps_only_original_official_high_confidence_remindable_events():
    outcome = parse_epo_document_list(
        FIXTURE.read_text(encoding="utf-8"),
        "CN202510469601.5",
        "CN120134203A",
    )

    assert outcome.code == ResultCode.OK
    assert [(d.document_title, d.official_date) for d in outcome.documents] == [
        ("第一次审查意见通知书", "2026-05-23")
    ]
    document = outcome.documents[0]
    assert document.remote_document_id == (
        "20251046960152104012026052310110680664683123_CN"
    )
    assert document.source == "epo_global_dossier"
    assert document.document_code == "210401-CN"
    assert document.document_version == "ORIGINAL"
    assert document.direction == "official"
    assert document.confidence == Confidence.HIGH
    assert document.source_trace == ["epo_global_dossier"]


def test_epo_parses_compact_register_table_with_row_document_id():
    html = """
    <html><head><title>European Patent Register</title></head><body>
      <h2>Global Dossier</h2>
      <div class="applicationInfo">CN202510469601 (CN)</div>
      <table class="docList">
        <thead><tr><th>Date</th><th>Document title</th><th>Version</th></tr></thead>
        <tbody>
          <tr id="20251046960152104012026052310110680664683123_CN">
            <td>23.05.2026</td><td>First notice of examination opinions</td>
            <td>ORIGINAL</td>
          </tr>
          <tr id="translated"><td>23.05.2026</td>
            <td>First notice of examination opinions</td><td>TRANSLATED</td></tr>
          <tr id="applicant"><td>01.07.2026</td>
            <td>Response to first office action</td><td>ORIGINAL</td></tr>
        </tbody>
      </table>
    </body></html>
    """

    outcome = parse_epo_document_list(
        html, "CN202510469601.5", "CN120134203A"
    )

    assert outcome.code == ResultCode.OK
    assert [(document.remote_document_id, document.official_date) for document in outcome.documents] == [
        ("20251046960152104012026052310110680664683123_CN", "2026-05-23")
    ]


@pytest.mark.parametrize("html", ["", " ", "\n\t"])
def test_epo_empty_response_is_temporary_error(html):
    outcome = parse_epo_document_list(html, "CN202510469601.5", "")
    assert outcome.code == ResultCode.TEMPORARY_ERROR


@pytest.mark.parametrize(
    "body",
    [
        "Access denied",
        "Login required",
        "Sign in",
        "Security verification",
        "Captcha",
    ],
)
def test_epo_intercept_login_and_security_pages_are_access_denied(body):
    outcome = parse_epo_document_list(f"<html><body>{body}</body></html>")
    assert outcome.code == ResultCode.ACCESS_DENIED


def test_epo_unknown_page_and_changed_headers_are_structure_changes():
    unknown = parse_epo_document_list("<html><body>maintenance</body></html>")
    changed = parse_epo_document_list(
        _page(
            _row(),
            headers="<th>When</th><th>What</th><th>Identifier</th>",
        ),
        "CN202510469601.5",
        "",
    )

    assert unknown.code == ResultCode.PAGE_STRUCTURE_CHANGED
    assert changed.code == ResultCode.PAGE_STRUCTURE_CHANGED


def test_epo_rejects_document_table_bound_to_a_different_case():
    outcome = parse_epo_document_list(
        _page(_row(), application="CN202510469602.3", publication="CN120134204A"),
        "CN202510469601.5",
        "CN120134203A",
    )

    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED
    assert outcome.documents == []


def test_epo_accepts_explicit_application_publication_aliases():
    by_application = parse_epo_document_list(
        _page(_row(remote_id="same-case")), "CN202510469601.5", ""
    )
    by_publication = parse_epo_document_list(
        _page(_row(remote_id="same-case")), "", "CN120134203A"
    )

    assert by_application.code == ResultCode.OK
    assert by_publication.code == ResultCode.OK
    assert by_publication.documents[0].application_number == "CN202510469601.5"
    assert by_publication.documents[0].publication_number == "CN120134203A"


def test_epo_accepts_same_case_identifiers_with_omitted_check_or_kind_code():
    application_alias = parse_epo_document_list(
        _page(_row()), "202510469601", ""
    )
    publication_alias = parse_epo_document_list(
        _page(_row()), "", "CN120134203"
    )

    assert application_alias.code == ResultCode.OK
    assert application_alias.resolved_application_number == "CN202510469601.5"
    assert publication_alias.code == ResultCode.OK
    assert publication_alias.documents[0].publication_number == "CN120134203A"


def test_epo_verified_explicit_empty_document_list_is_ok():
    outcome = parse_epo_document_list(
        _page(
            '<tr class="empty-state"><td colspan="6">'
            "No documents found</td></tr>"
        ),
        "CN202510469601.5",
        "CN120134203A",
    )

    assert outcome.code == ResultCode.OK
    assert outcome.documents == []


def test_epo_unrelated_empty_state_does_not_validate_an_empty_document_table():
    outcome = parse_epo_document_list(
        _page(empty='<aside class="empty-state">No records</aside>'),
        "CN202510469601.5",
        "CN120134203A",
    )

    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED
    assert outcome.documents == []


def test_epo_hidden_void_element_does_not_hide_following_document_table():
    html = _page(_row()).replace("<body>", "<body><input hidden>")

    outcome = parse_epo_document_list(html, "CN202510469601.5", "")

    assert outcome.code == ResultCode.OK
    assert [document.remote_document_id for document in outcome.documents] == [
        "doc-1"
    ]


def test_epo_navigation_sign_in_link_is_not_treated_as_an_intercept_page():
    html = _page(_row()).replace(
        "<body>", '<body><nav><a href="/login">Sign in</a></nav>'
    )

    outcome = parse_epo_document_list(html, "CN202510469601.5", "")

    assert outcome.code == ResultCode.OK
    assert [document.remote_document_id for document in outcome.documents] == [
        "doc-1"
    ]


def test_epo_empty_table_without_explicit_empty_state_is_structure_change():
    outcome = parse_epo_document_list(
        _page(), "CN202510469601.5", "CN120134203A"
    )
    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED


def test_epo_data_row_missing_required_columns_is_structure_change():
    outcome = parse_epo_document_list(
        _page(
            "<tr><td>2026-05-23</td>"
            "<td>First notice of examination opinions</td></tr>"
        ),
        "CN202510469601.5",
        "",
    )

    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED
    assert outcome.documents == []


def test_epo_reports_date_failure_for_remindable_official_candidates():
    outcome = parse_epo_document_list(
        _page(_row(date="not-a-date", title="Decision to reject", code="")),
        "CN202510469601.5",
        "",
    )
    assert outcome.code == ResultCode.DATE_PARSE_FAILED
    assert outcome.documents == []


def test_epo_does_not_report_ok_empty_for_malformed_official_row():
    outcome = parse_epo_document_list(
        _page(_row(remote_id="")), "CN202510469601.5", ""
    )

    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED
    assert outcome.documents == []


def test_epo_rejects_official_row_whose_id_names_another_application():
    outcome = parse_epo_document_list(
        _page(_row(remote_id="202510469602521040120260523_CN")),
        "CN202510469601.5",
        "",
    )

    assert outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED
    assert outcome.documents == []


def test_epo_provider_uses_public_application_entry_and_maps_http_statuses():
    calls = []

    def fetch(url):
        calls.append(url)
        return 200, FIXTURE.read_text(encoding="utf-8")

    provider = EpoGlobalDossierProvider(fetch_html=fetch)
    outcome = provider.list_documents(
        "CN202510469601.5", "CN120134203A", Event()
    )

    assert outcome.code == ResultCode.OK
    assert calls == [EPO_DOSSIER_URL.format(application="202510469601")]
    assert "ops" not in calls[0].lower()
    assert provider.check_auth(Event()) == ResultCode.OK
    assert provider.ensure_login(Event()) == ResultCode.OK
    assert provider.download_document(outcome.documents[0], "/tmp", Event()) == (
        ResultCode.UNSUPPORTED_JURISDICTION
    )

    rate_limited = EpoGlobalDossierProvider(
        fetch_html=lambda _url: (429, "")
    ).list_documents("CN202510469601.5", "", Event())
    unavailable = EpoGlobalDossierProvider(
        fetch_html=lambda _url: (503, "")
    ).list_documents("CN202510469601.5", "", Event())
    assert rate_limited.code == ResultCode.RATE_LIMITED
    assert unavailable.code == ResultCode.NETWORK_ERROR


def test_epo_public_fetch_uses_browser_headers_without_credentials(monkeypatch):
    captured = {}

    class Response:
        status = 200

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def read(self):
            return b"<html>ok</html>"

    def urlopen(request, timeout):
        captured["request"] = request
        captured["timeout"] = timeout
        return Response()

    monkeypatch.setattr("urllib.request.urlopen", urlopen)
    status, body = fetch_html_public("https://register.epo.org/example")

    headers = {key.lower(): value for key, value in captured["request"].header_items()}
    assert status == 200
    assert body == "<html>ok</html>"
    assert headers["user-agent"].startswith("Mozilla/5.0")
    assert headers["accept-language"] == "en"
    assert "authorization" not in headers
    assert "cookie" not in headers


def test_epo_provider_never_treats_publication_as_application():
    fetched = []
    provider = EpoGlobalDossierProvider(
        fetch_html=lambda url: (fetched.append(url), "")[1]
    )

    resolved = provider.resolve_case("CN120134203A", "", Event())
    listed = provider.list_documents("CN120134203A", "", Event())
    publication_only = provider.list_documents("", "CN120134203A", Event())

    assert resolved.code == ResultCode.RESOLVE_FAILED
    assert listed.code == ResultCode.RESOLVE_FAILED
    assert publication_only.code == ResultCode.RESOLVE_FAILED
    assert fetched == []


def test_epo_provider_honours_cancellation_before_fetch():
    cancel = Event()
    cancel.set()
    fetched = []
    outcome = EpoGlobalDossierProvider(
        fetch_html=lambda url: fetched.append(url)
    ).list_documents("CN202510469601.5", "", cancel)

    assert outcome.code == ResultCode.TEMPORARY_ERROR
    assert "取消" in outcome.message
    assert fetched == []
