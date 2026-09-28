from __future__ import annotations

import io
import json
import base64
import math
import threading
import time

import pytest

from web_dossier import models, service
from web_dossier.models import DocumentType, ProsecutionDocument, ResultCode
from web_dossier.providers.base import ProtocolSerializationError, SyncOutcome
from web_dossier.providers.chain import ProviderAttempt
from web_dossier.providers import cnipa as cnipa_module


APPLICATION_NUMBER = "CN202510469601.5"
PUBLICATION_NUMBER = "CN120134203A"


@pytest.fixture(autouse=True)
def _restore_service_caches():
    previous_providers = dict(service._PROVIDERS)
    previous_chain = getattr(service, "_CHAIN", None)
    service._PROVIDERS.clear()
    service._CHAIN = None
    yield
    service._PROVIDERS.clear()
    service._PROVIDERS.update(previous_providers)
    service._CHAIN = previous_chain


def _document(
    document_type=DocumentType.REJECTION_DECISION,
    *,
    official_date="2026-05-23",
    oa_ordinal=0,
    remote_document_id="rid-1",
    direction="official",
    document_version="ORIGINAL",
):
    return ProsecutionDocument(
        application_number=APPLICATION_NUMBER,
        publication_number=PUBLICATION_NUMBER,
        source="epo_global_dossier",
        remote_document_id=remote_document_id,
        document_type=document_type,
        document_title=document_type.value,
        official_date=official_date,
        oa_ordinal=oa_ordinal,
        direction=direction,
        document_version=document_version,
    )


class FakeChain:
    def __init__(self, outcome=None, exception=None):
        self.outcome = outcome
        self.exception = exception
        self.calls = []
        self.ensure_login_calls = 0

    def list_documents(self, application_number, publication_number, cancel):
        self.calls.append((application_number, publication_number, cancel))
        if self.exception is not None:
            raise self.exception
        return self.outcome

    def ensure_login(self, _cancel):
        self.ensure_login_calls += 1
        raise AssertionError("sync_case must never initiate a login flow")


def _install_chain(monkeypatch, outcome=None, exception=None):
    chain = FakeChain(outcome=outcome, exception=exception)
    monkeypatch.setattr(service, "_get_chain", lambda: chain)
    return chain


def test_sync_case_returns_actual_provider_attempts_and_latest_event(monkeypatch):
    older = _document(
        DocumentType.OFFICE_ACTION_FIRST,
        official_date="2026-05-01",
        oa_ordinal=1,
        remote_document_id="oa-1",
    )
    latest = _document(
        DocumentType.REJECTION_DECISION,
        official_date="2026-05-23",
        remote_document_id="reject-1",
    )
    attempts = [
        ProviderAttempt("uspto_global_dossier", ResultCode.NETWORK_ERROR, "offline"),
        ProviderAttempt("epo_global_dossier", ResultCode.OK, ""),
    ]
    outcome = SyncOutcome(
        code=ResultCode.OK,
        auth_state="NOT_REQUIRED",
        provider_used="epo_global_dossier",
        attempts=attempts,
        resolved_application_number=APPLICATION_NUMBER,
        documents=[older, latest],
    )
    chain = _install_chain(monkeypatch, outcome)

    payload = service._op_sync_case(
        {
            "application_number": APPLICATION_NUMBER,
            "publication_number": PUBLICATION_NUMBER,
        },
        threading.Event(),
    )

    assert chain.calls[0][:2] == (APPLICATION_NUMBER, PUBLICATION_NUMBER)
    assert payload["provider_used"] == "epo_global_dossier"
    assert payload["attempts"] == [
        {"provider": "uspto_global_dossier", "code": "NETWORK_ERROR", "message": "offline"},
        {"provider": "epo_global_dossier", "code": "OK", "message": ""},
    ]
    assert [item["remote_document_id"] for item in payload["documents"]] == [
        "oa-1",
        "reject-1",
    ]
    assert payload["latest_event"]["remote_document_id"] == "reject-1"
    assert payload["latest_oa"] is None
    assert payload["code"] == "OK"


def test_sync_case_surfaces_auth_required_without_calling_login(monkeypatch):
    outcome = SyncOutcome(
        code=ResultCode.AUTH_REQUIRED,
        message="login needed",
        auth_state="AUTH_REQUIRED",
        provider_used="cnipa",
        attempts=[ProviderAttempt("cnipa", ResultCode.AUTH_REQUIRED, "login needed")],
    )
    chain = _install_chain(monkeypatch, outcome)

    payload = service._op_sync_case(
        {"application_number": APPLICATION_NUMBER}, threading.Event()
    )

    assert payload["ok"] is False
    assert payload["code"] == "AUTH_REQUIRED"
    assert payload["provider_used"] == "cnipa"
    assert payload["attempts"][0]["code"] == "AUTH_REQUIRED"
    assert payload["documents"] == []
    assert payload["latest_event"] is None
    assert payload["latest_oa"] is None
    assert chain.ensure_login_calls == 0


def test_sync_case_ok_with_no_documents_has_stable_empty_protocol(monkeypatch):
    _install_chain(
        monkeypatch,
        SyncOutcome(
            code=ResultCode.OK,
            provider_used="epo_global_dossier",
            attempts=[ProviderAttempt("epo_global_dossier", ResultCode.OK)],
            documents=[],
        ),
    )

    payload = service._op_sync_case({}, threading.Event())

    assert payload == {
        "ok": True,
        "code": "OK",
        "message": "",
        "auth_state": "NOT_INITIALIZED",
        "provider_used": "epo_global_dossier",
        "attempts": [
            {"provider": "epo_global_dossier", "code": "OK", "message": ""}
        ],
        "resolved_application_number": "",
        "documents": [],
        "latest_event": None,
        "latest_oa": None,
    }


def test_latest_official_event_uses_date_ordinal_and_remote_id_ordering():
    documents = [
        _document(
            DocumentType.OFFICE_ACTION_FIRST,
            oa_ordinal=1,
            remote_document_id="z-low-ordinal",
        ),
        _document(
            DocumentType.OFFICE_ACTION_NTH,
            oa_ordinal=3,
            remote_document_id="a-third",
        ),
        _document(
            DocumentType.OFFICE_ACTION_NTH,
            oa_ordinal=3,
            remote_document_id="z-third",
        ),
    ]

    latest = models.pick_latest_official_event(documents)

    assert latest.remote_document_id == "z-third"


def test_latest_official_event_filters_translated_applicant_invalid_and_unknown():
    valid = _document(
        DocumentType.OTHER_OFFICIAL,
        official_date="2026-01-02",
        remote_document_id="valid",
    )
    rejected = [
        _document(
            DocumentType.GRANT_NOTICE,
            official_date="2026-12-30",
            remote_document_id="translated",
            document_version="TRANSLATED",
        ),
        _document(
            DocumentType.REJECTION_DECISION,
            official_date="2026-12-31",
            remote_document_id="applicant",
            direction="applicant",
        ),
        _document(
            DocumentType.CORRECTION_NOTICE,
            official_date="2026-02-30",
            remote_document_id="invalid-date",
        ),
        _document(
            DocumentType.UNKNOWN,
            official_date="2026-12-31",
            remote_document_id="unknown",
        ),
        _document(
            DocumentType.OFFICE_ACTION_UNKNOWN,
            official_date="2026-12-31",
            remote_document_id="unknown-oa",
        ),
        _document(
            DocumentType.GRANT_NOTICE,
            official_date="2026-12-31",
            remote_document_id="padded-direction",
            direction="  OFFICIAL  ",
        ),
        _document(
            DocumentType.GRANT_NOTICE,
            official_date="2026-12-31",
            remote_document_id="lower-version",
            document_version="original",
        ),
    ]

    assert models.pick_latest_official_event([*rejected, valid]) is valid
    assert models.pick_latest_official_event(None) is None
    assert models.pick_latest_official_event(object()) is None


def test_get_chain_is_lazy_singleton_and_constructors_do_not_launch_browser(monkeypatch):
    manager_calls = []
    provider_calls = []

    class FakeManager:
        def __init__(self, provider, **kwargs):
            manager_calls.append((provider, kwargs))

        def launch(self):
            pytest.fail("constructing the chain must not launch a browser")

        def open_page(self, *_args, **_kwargs):
            pytest.fail("constructing the chain must not open a page")

    class FakeUspto:
        provider_id = "uspto_global_dossier"

        def __init__(self, browser_manager=None):
            provider_calls.append((self.provider_id, browser_manager))

    class FakeEpo:
        provider_id = "epo_global_dossier"

        def __init__(self):
            provider_calls.append((self.provider_id, None))

    class FakeCnipa:
        provider_id = "cnipa"

        def __init__(self, browser_manager=None):
            provider_calls.append((self.provider_id, browser_manager))

    monkeypatch.setattr("web_dossier.browser.manager.BrowserManager", FakeManager)
    monkeypatch.setattr(
        "web_dossier.providers.uspto_global_dossier.UsptoGlobalDossierProvider",
        FakeUspto,
    )
    monkeypatch.setattr(
        "web_dossier.providers.epo_global_dossier.EpoGlobalDossierProvider", FakeEpo
    )
    monkeypatch.setattr(service, "CNIPAWebProvider", FakeCnipa)
    monkeypatch.setattr(service, "_CHAIN", None, raising=False)
    service._PROVIDERS.clear()

    first = service._get_chain()
    second = service._get_chain()

    assert first is second
    assert [provider.provider_id for provider in first.providers] == [
        "uspto_global_dossier",
        "epo_global_dossier",
        "cnipa",
    ]
    assert manager_calls == [
        (
            "uspto_global_dossier",
            {"headless": True, "prefer_system_browser": False},
        ),
        ("cnipa", {}),
    ]
    assert [provider for provider, _manager in provider_calls] == [
        "uspto_global_dossier",
        "epo_global_dossier",
        "cnipa",
    ]


def test_real_chain_gates_cnipa_without_opening_browser_until_explicit_login(monkeypatch):
    managers = {}

    class FakeManager:
        def __init__(self, provider, **_kwargs):
            self.provider = provider
            self._context = None
            self._page = None
            self.launch_calls = 0
            self.open_page_calls = 0
            self.ensure_login_calls = 0
            self.state_checks = 0
            managers[provider] = self

        def cpquery_session_state(self, expected_origin, **_kwargs):
            assert expected_origin == cnipa_module.BASE_URL
            self.state_checks += 1
            return "NOT_INITIALIZED"

        def launch(self):
            self.launch_calls += 1
            return True

        def open_page(self, *_args, **_kwargs):
            self.open_page_calls += 1
            self.launch()
            raise AssertionError("background sync must not open CNIPA")

        def ensure_login(self, *_args, **_kwargs):
            self.ensure_login_calls += 1
            self.launch()
            return ResultCode.OK

    class FakeUspto:
        provider_id = "uspto_global_dossier"

        def __init__(self, browser_manager=None):
            self._manager = browser_manager

        def list_documents(self, *_args):
            return SyncOutcome(code=ResultCode.NETWORK_ERROR, message="uspto down")

    class FakeEpo:
        provider_id = "epo_global_dossier"

        def list_documents(self, *_args):
            return SyncOutcome(code=ResultCode.NETWORK_ERROR, message="epo down")

    monkeypatch.setattr("web_dossier.browser.manager.BrowserManager", FakeManager)
    monkeypatch.setattr(
        "web_dossier.providers.uspto_global_dossier.UsptoGlobalDossierProvider",
        FakeUspto,
    )
    monkeypatch.setattr(
        "web_dossier.providers.epo_global_dossier.EpoGlobalDossierProvider", FakeEpo
    )

    outcome = service._get_chain().list_documents(
        APPLICATION_NUMBER, PUBLICATION_NUMBER, threading.Event()
    )

    cnipa_manager = managers["cnipa"]
    assert outcome.code == ResultCode.AUTH_REQUIRED
    assert outcome.provider_used == "cnipa"
    assert [attempt.provider for attempt in outcome.attempts] == [
        "uspto_global_dossier",
        "epo_global_dossier",
        "cnipa",
    ]
    assert cnipa_manager.launch_calls == 0
    assert cnipa_manager.open_page_calls == 0
    assert cnipa_manager.ensure_login_calls == 0
    assert cnipa_manager.state_checks == 1

    login = service._op_login({}, threading.Event())

    assert login["code"] == "OK"
    assert cnipa_manager.ensure_login_calls == 1
    assert cnipa_manager.launch_calls == 1


def test_explicit_login_still_only_uses_cnipa(monkeypatch):
    class FakeCnipa:
        def __init__(self):
            self.login_calls = 0

        def ensure_login(self, _cancel):
            self.login_calls += 1
            return ResultCode.OK

    fake_cnipa = FakeCnipa()
    requested = []

    def fake_get_provider(name):
        requested.append(name)
        if name == "cnipa":
            return fake_cnipa, None
        return None, ResultCode.UNSUPPORTED_JURISDICTION

    monkeypatch.setattr(service, "_get_provider", fake_get_provider)

    ok = service._op_login({}, threading.Event())
    unsupported = service._op_login({"provider": "epo_global_dossier"}, threading.Event())

    assert ok["code"] == "OK"
    assert unsupported["code"] == "UNSUPPORTED_JURISDICTION"
    assert requested == ["cnipa", "epo_global_dossier"]
    assert fake_cnipa.login_calls == 1


def test_sync_outcome_serialization_rejects_malformed_documents_and_attempts():
    class Broken:
        def to_dict(self):
            raise KeyError("broken")

    outcome = SyncOutcome(
        code=ResultCode.OK,
        documents=[None, Broken()],
        provider_used="safe-provider",
        attempts=[
            ProviderAttempt("first", ResultCode.NETWORK_ERROR, "offline"),
            {"provider": "second", "code": ResultCode.OK, "message": ""},
            Broken(),
            None,
        ],
    )

    with pytest.raises(ProtocolSerializationError, match="document"):
        outcome.to_dict(latest_event=None)


def test_sync_outcome_serialization_rejects_non_iterable_containers():
    outcome = SyncOutcome(code=ResultCode.OK)
    outcome.documents = object()
    outcome.attempts = object()

    with pytest.raises(ProtocolSerializationError, match="documents"):
        outcome.to_dict()


@pytest.mark.parametrize(
    "attempt",
    [
        {"provider": "broken", "code": "NOT_A_RESULT_CODE", "message": ""},
        object(),
    ],
)
def test_sync_outcome_serialization_rejects_invalid_attempt(attempt):
    outcome = SyncOutcome(code=ResultCode.OK, attempts=[attempt])

    with pytest.raises(ProtocolSerializationError, match="attempt"):
        outcome.to_dict()


def test_sync_outcome_serialization_rejects_invalid_document_and_latest_event():
    invalid = _document()
    invalid.document_type = "NOT_A_DOCUMENT_TYPE"

    with pytest.raises(ProtocolSerializationError, match="document"):
        SyncOutcome(code=ResultCode.OK, documents=[invalid]).to_dict()
    with pytest.raises(ProtocolSerializationError, match="latest_event"):
        SyncOutcome(code=ResultCode.OK).to_dict(latest_event=invalid)


def test_sync_outcome_serialization_rejects_non_dict_to_dict_results():
    class ReturnsList:
        def to_dict(self):
            return []

    with pytest.raises(ProtocolSerializationError, match="document"):
        SyncOutcome(code=ResultCode.OK, documents=[ReturnsList()]).to_dict()
    with pytest.raises(ProtocolSerializationError, match="attempt"):
        SyncOutcome(code=ResultCode.OK, attempts=[ReturnsList()]).to_dict()


def test_sync_case_converts_serialization_failure_to_stable_error(monkeypatch):
    class BrokenDocument:
        def to_dict(self):
            raise KeyError("broken")

    _install_chain(
        monkeypatch,
        SyncOutcome(
            code=ResultCode.OK,
            documents=[BrokenDocument()],
            provider_used="broken-provider",
        ),
    )

    payload = service._op_sync_case({}, threading.Event())

    assert payload["code"] == "TEMPORARY_ERROR"
    assert payload["ok"] is False
    assert payload["documents"] == []
    assert payload["attempts"] == []


def test_old_positional_to_dict_call_stays_readable_but_latest_oa_is_retired():
    latest = _document(DocumentType.GRANT_NOTICE)
    outcome = SyncOutcome(code=ResultCode.NEW_OFFICE_ACTION, documents=[latest])

    payload = outcome.to_dict(latest)

    assert payload["ok"] is True
    assert payload["code"] == "NEW_OFFICE_ACTION"
    assert payload["latest_event"]["document_type"] == "GRANT_NOTICE"
    assert payload["latest_oa"] is None


def test_sync_case_never_emits_the_legacy_new_office_action_code(monkeypatch):
    _install_chain(
        monkeypatch,
        SyncOutcome(
            code=ResultCode.NEW_OFFICE_ACTION,
            documents=[_document(DocumentType.GRANT_NOTICE)],
            provider_used="legacy-provider",
        ),
    )

    payload = service._op_sync_case({}, threading.Event())

    assert payload["code"] == "NEW_OFFICIAL_EVENT"
    assert payload["ok"] is True


def test_sync_case_exception_keeps_the_full_protocol_shape(monkeypatch):
    _install_chain(monkeypatch, exception=RuntimeError("cookie=secret"))

    payload = service._op_sync_case(
        {"application_number": APPLICATION_NUMBER}, threading.Event()
    )

    assert set(payload) == {
        "ok",
        "code",
        "message",
        "auth_state",
        "provider_used",
        "attempts",
        "resolved_application_number",
        "documents",
        "latest_event",
        "latest_oa",
    }
    assert payload["ok"] is False
    assert payload["code"] == "TEMPORARY_ERROR"
    assert payload["documents"] == []
    assert payload["attempts"] == []
    assert payload["latest_event"] is None
    assert payload["latest_oa"] is None
    assert "secret" not in payload["message"]


def test_sync_case_malformed_args_keeps_the_full_protocol_shape(monkeypatch):
    chain = _install_chain(monkeypatch, SyncOutcome(code=ResultCode.OK))

    payload = service._op_sync_case([], threading.Event())

    assert set(payload) == {
        "ok",
        "code",
        "message",
        "auth_state",
        "provider_used",
        "attempts",
        "resolved_application_number",
        "documents",
        "latest_event",
        "latest_oa",
    }
    assert payload["code"] == "TEMPORARY_ERROR"
    assert chain.calls == []


def test_main_rejects_non_mapping_request_and_continues(monkeypatch):
    stdin = io.StringIO('[]\n{"op":"ping"}\n')
    stdout = io.StringIO()
    monkeypatch.setattr(service.sys, "stdin", stdin)
    monkeypatch.setattr(service.sys, "stdout", stdout)

    assert service.main() == 0

    responses = [json.loads(line) for line in stdout.getvalue().splitlines()]
    assert responses[0]["code"] == "TEMPORARY_ERROR"
    assert responses[1]["op"] == "pong"


def test_main_dispatches_malformed_sync_args_as_stable_outcome(monkeypatch):
    stdin = io.StringIO('{"op":"sync_case","args":[]}\n')
    stdout = io.StringIO()
    monkeypatch.setattr(service.sys, "stdin", stdin)
    monkeypatch.setattr(service.sys, "stdout", stdout)

    assert service.main() == 0

    response = json.loads(stdout.getvalue())
    assert set(response) == {
        "ok",
        "code",
        "message",
        "auth_state",
        "provider_used",
        "attempts",
        "resolved_application_number",
        "documents",
        "latest_event",
        "latest_oa",
    }
    assert response["code"] == "TEMPORARY_ERROR"


class _WaitingOutput(io.StringIO):
    def __init__(self):
        super().__init__()
        self._condition = threading.Condition()
        self._line_count = 0

    def write(self, value):
        written = super().write(value)
        if "\n" in value:
            with self._condition:
                self._line_count += value.count("\n")
                self._condition.notify_all()
        return written

    def wait_for_lines(self, count):
        with self._condition:
            assert self._condition.wait_for(
                lambda: self._line_count >= count, timeout=2
            )


class _SequencedInput:
    def __init__(self, lines, output):
        self._lines = lines
        self._output = output

    def __iter__(self):
        for index, line in enumerate(self._lines):
            if index:
                self._output.wait_for_lines(index)
            yield line


def test_main_uses_one_long_lived_worker_for_provider_lifecycle(monkeypatch):
    thread_ids = []

    class FakeManager:
        def __init__(self, provider, **_kwargs):
            self.provider = provider
            thread_ids.append((f"manager:{provider}", threading.get_ident()))

        def close(self):
            thread_ids.append((f"close:{self.provider}", threading.get_ident()))

    class FakeCnipa:
        provider_id = "cnipa"

        def __init__(self, browser_manager=None):
            self._manager = browser_manager
            thread_ids.append(("provider:cnipa", threading.get_ident()))

        def existing_session_state(self):
            return "AUTHENTICATED"

        def ensure_login(self, _cancel):
            thread_ids.append(("login", threading.get_ident()))
            return ResultCode.OK

        def list_documents(self, *_args):
            thread_ids.append(("cnipa-sync", threading.get_ident()))
            return SyncOutcome(code=ResultCode.OK)

        def download_document(self, *_args):
            thread_ids.append(("download", threading.get_ident()))
            return ResultCode.OK, {}

        def close(self):
            self._manager.close()

    class FakeUspto:
        provider_id = "uspto_global_dossier"

        def __init__(self, browser_manager=None):
            self._manager = browser_manager
            thread_ids.append(("provider:uspto", threading.get_ident()))

        def list_documents(self, *_args):
            thread_ids.append(("sync", threading.get_ident()))
            return SyncOutcome(code=ResultCode.OK)

        def close(self):
            self._manager.close()

    class FakeEpo:
        provider_id = "epo_global_dossier"

        def __init__(self):
            thread_ids.append(("provider:epo", threading.get_ident()))

        def list_documents(self, *_args):
            raise AssertionError("USPTO should satisfy this test")

    monkeypatch.setattr("web_dossier.browser.manager.BrowserManager", FakeManager)
    monkeypatch.setattr(service, "CNIPAWebProvider", FakeCnipa)
    monkeypatch.setattr(
        "web_dossier.providers.uspto_global_dossier.UsptoGlobalDossierProvider",
        FakeUspto,
    )
    monkeypatch.setattr(
        "web_dossier.providers.epo_global_dossier.EpoGlobalDossierProvider", FakeEpo
    )
    output = _WaitingOutput()
    lines = [
        '{"op":"login","args":{}}\n',
        '{"op":"sync_case","args":{}}\n',
        '{"op":"download_document","args":{}}\n',
        '{"op":"sync_case","args":{}}\n',
        '{"op":"shutdown","args":{}}\n',
    ]
    monkeypatch.setattr(service.sys, "stdin", _SequencedInput(lines, output))
    monkeypatch.setattr(service.sys, "stdout", output)
    main_thread = threading.get_ident()

    assert service.main() == 0

    operation_ids = [thread_id for _label, thread_id in thread_ids]
    assert operation_ids
    assert len(set(operation_ids)) == 1
    assert operation_ids[0] != main_thread
    assert [label for label, _thread_id in thread_ids].count("sync") == 2
    assert any(label.startswith("close:") for label, _thread_id in thread_ids)


def test_main_thread_can_cancel_the_long_lived_worker(monkeypatch):
    started = threading.Event()
    finished = threading.Event()
    worker_threads = []

    class BlockingChain:
        def list_documents(self, _application, _publication, cancel):
            worker_threads.append(threading.get_ident())
            started.set()
            assert cancel.wait(timeout=2)
            finished.set()
            return SyncOutcome(
                code=ResultCode.TEMPORARY_ERROR,
                message="cancelled",
            )

    class CancelInput:
        def __iter__(self):
            yield '{"op":"sync_case","args":{}}\n'
            assert started.wait(timeout=2)
            yield '{"op":"cancel","args":{}}\n'
            assert finished.wait(timeout=2)
            yield '{"op":"shutdown","args":{}}\n'

    output = _WaitingOutput()
    monkeypatch.setattr(service, "_get_chain", lambda: BlockingChain())
    monkeypatch.setattr(service.sys, "stdin", CancelInput())
    monkeypatch.setattr(service.sys, "stdout", output)
    main_thread = threading.get_ident()

    assert service.main() == 0

    assert worker_threads and worker_threads[0] != main_thread


def _base64url_json(value):
    raw = json.dumps(value, separators=(",", ":")).encode("utf-8")
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


_JWT_SIGNATURE = base64.urlsafe_b64encode(b"signature").rstrip(b"=").decode("ascii")


def _jwt(exp=None):
    payload = {} if exp is None else {"exp": exp}
    return (
        f"{_base64url_json({'alg': 'none'})}."
        f"{_base64url_json(payload)}.{_JWT_SIGNATURE}"
    )


class _TokenPage:
    def __init__(self, token, url="https://cpquery.cponline.cnipa.gov.cn/"):
        self.url = url
        self._token = token

    def evaluate(self, expression):
        assert expression == "localStorage.getItem('ACCESS_TOKEN')"
        return self._token

    def content(self):
        return ""


@pytest.mark.parametrize(
    ("token", "expected"),
    [
        (None, "AUTH_REQUIRED"),
        (_jwt(exp=1_000), "SESSION_EXPIRED"),
        (_jwt(exp=10_000), "AUTHENTICATED"),
        (_jwt(), "SESSION_EXPIRED"),
        ("not-a-jwt", "SESSION_EXPIRED"),
        ("eyJhbGciOiJub25lIn0.W10.signature", "SESSION_EXPIRED"),
        (_jwt(exp=math.nan), "SESSION_EXPIRED"),
        (_jwt(exp=math.inf), "SESSION_EXPIRED"),
        (_jwt(exp="10000"), "SESSION_EXPIRED"),
    ],
)
def test_cpquery_session_state_validates_token_without_launch(token, expected):
    from web_dossier.browser.manager import BrowserManager

    manager = BrowserManager("cnipa")
    manager._context = object()
    manager._page = _TokenPage(token)
    manager.launch = lambda: pytest.fail("session state must not launch")

    assert manager.cpquery_session_state(
        cnipa_module.BASE_URL,
        now=2_000,
        min_ttl_seconds=60,
    ) == expected


@pytest.mark.parametrize(
    "token",
    [
        f".{_base64url_json({'exp': 10_000})}.{_JWT_SIGNATURE}",
        f"{_base64url_json({'alg': 'none'})}.{_base64url_json({'exp': 10_000})}.",
        (
            f"{_base64url_json({'alg': 'none'})}."
            f"{_base64url_json({'exp': 10_000})}$$$.{_JWT_SIGNATURE}"
        ),
        f"bad$header.{_base64url_json({'exp': 10_000})}.{_JWT_SIGNATURE}",
        (
            f"{_base64url_json({'alg': 'none'})}."
            f"{_base64url_json({'exp': 10_000})}.bad$signature"
        ),
        f"{_base64url_json([])}.{_base64url_json({'exp': 10_000})}.{_JWT_SIGNATURE}",
        f"{_base64url_json({'alg': 'none'})}.{_base64url_json([])}.{_JWT_SIGNATURE}",
    ],
)
def test_cpquery_session_state_rejects_malformed_jwt_structure(token):
    from web_dossier.browser.manager import BrowserManager

    manager = BrowserManager("cnipa")
    manager._context = object()
    manager._page = _TokenPage(token)

    assert manager.cpquery_session_state(
        cnipa_module.BASE_URL, now=2_000
    ) == "SESSION_EXPIRED"


def test_cpquery_session_state_distinguishes_uninitialized_and_wrong_origin():
    from web_dossier.browser.manager import BrowserManager

    manager = BrowserManager("cnipa")
    assert manager.cpquery_session_state(
        cnipa_module.BASE_URL, now=time.time()
    ) == "NOT_INITIALIZED"
    manager._context = object()
    manager._page = _TokenPage(
        _jwt(exp=time.time() + 3_600), url="https://example.com/"
    )
    assert manager.cpquery_session_state(
        cnipa_module.BASE_URL, now=time.time()
    ) == "AUTH_REQUIRED"


def test_cpquery_session_state_accepts_exact_override_origin():
    from web_dossier.browser.manager import BrowserManager

    override = "https://cnipa.internal.example:8443/custom/base"
    manager = BrowserManager("cnipa")
    manager._context = object()
    manager._page = _TokenPage(
        _jwt(exp=10_000),
        url="https://cnipa.internal.example:8443/dossier",
    )

    assert manager.cpquery_session_state(
        override, now=2_000
    ) == "AUTHENTICATED"
    manager._page = _TokenPage(
        _jwt(exp=10_000),
        url="https://cnipa.internal.example.evil:8443/dossier",
    )
    assert manager.cpquery_session_state(
        override, now=2_000
    ) == "AUTH_REQUIRED"


def test_cnipa_existing_session_state_passes_configured_base_url(monkeypatch):
    override = "https://override-cnipa.example:9443/root"
    seen = []

    class FakeManager:
        def cpquery_session_state(self, expected_origin, **_kwargs):
            seen.append(expected_origin)
            return "AUTHENTICATED"

    monkeypatch.setattr(cnipa_module, "BASE_URL", override)
    provider = cnipa_module.CNIPAWebProvider(browser_manager=FakeManager())

    assert provider.existing_session_state() == "AUTHENTICATED"
    assert seen == [override]


def test_cnipa_ensure_login_rejects_expired_token_callback(monkeypatch):
    now = time.time()
    expired_page = _TokenPage(_jwt(exp=now - 1))

    class FakeManager:
        def cpquery_session_state(self, expected_origin, *, page=None, **_kwargs):
            assert expected_origin == cnipa_module.BASE_URL
            assert page is expired_page
            return "SESSION_EXPIRED"

        def ensure_login(self, _url, is_logged_in_fn, _cancel, **_kwargs):
            return (
                ResultCode.OK
                if is_logged_in_fn(expired_page)
                else ResultCode.AUTH_REQUIRED
            )

    provider = cnipa_module.CNIPAWebProvider(browser_manager=FakeManager())

    assert provider.ensure_login(threading.Event()) == ResultCode.AUTH_REQUIRED


@pytest.mark.parametrize(
    ("state", "expected"),
    [
        ("AUTHENTICATED", ResultCode.OK),
        ("SESSION_EXPIRED", ResultCode.SESSION_EXPIRED),
        ("AUTH_REQUIRED", ResultCode.AUTH_REQUIRED),
    ],
)
def test_cnipa_check_auth_uses_shared_session_validation(state, expected):
    page = _TokenPage(_jwt(exp=time.time() + 3_600))

    class FakeManager:
        def open_page(self, url, _cancel):
            assert url == cnipa_module.BASE_URL
            return page

        def cpquery_session_state(self, expected_origin, *, page=None, **_kwargs):
            assert expected_origin == cnipa_module.BASE_URL
            assert page is not None
            return state

    provider = cnipa_module.CNIPAWebProvider(browser_manager=FakeManager())

    assert provider.check_auth(threading.Event()) == expected


def test_background_cnipa_gate_preserves_session_state_and_normalizes_success():
    class FakeProvider:
        def __init__(self, state):
            self.state = state
            self.calls = 0

        def existing_session_state(self):
            return self.state

        def list_documents(self, *_args):
            self.calls += 1
            return SyncOutcome(code=ResultCode.OK)

    expired = FakeProvider("SESSION_EXPIRED")
    expired_outcome = service._BackgroundCnipaProvider(expired).list_documents(
        "", "", threading.Event()
    )
    assert expired_outcome.code == ResultCode.SESSION_EXPIRED
    assert expired_outcome.auth_state == "SESSION_EXPIRED"
    assert expired.calls == 0

    authenticated = FakeProvider("AUTHENTICATED")
    ok = service._BackgroundCnipaProvider(authenticated).list_documents(
        "", "", threading.Event()
    )
    assert ok.code == ResultCode.OK
    assert ok.auth_state == "AUTHENTICATED"
    assert authenticated.calls == 1
