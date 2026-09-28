from __future__ import annotations

import io
import json
import threading

import pytest

from tools.web_dossier import service
from web_dossier import models
from web_dossier.models import DocumentType, ProsecutionDocument, ResultCode
from web_dossier.providers.base import SyncOutcome
from web_dossier.providers.chain import ProviderAttempt


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
            self.token_checks = 0
            managers[provider] = self

        def has_cpquery_token(self):
            self.token_checks += 1
            return False

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
    assert cnipa_manager.token_checks == 0

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


def test_sync_outcome_serialization_tolerates_malformed_documents_and_attempts():
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

    payload = outcome.to_dict(latest_event=None)

    assert payload["documents"] == []
    assert payload["attempts"] == [
        {"provider": "first", "code": "NETWORK_ERROR", "message": "offline"},
        {"provider": "second", "code": "OK", "message": ""},
    ]
    assert payload["latest_event"] is None
    assert payload["latest_oa"] is None


def test_sync_outcome_serialization_tolerates_non_iterable_containers():
    outcome = SyncOutcome(code=ResultCode.OK)
    outcome.documents = object()
    outcome.attempts = object()

    payload = outcome.to_dict()

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
    class ImmediateThread:
        def __init__(self, target, daemon=False):
            self.target = target
            self.daemon = daemon

        def start(self):
            self.target()

    stdin = io.StringIO('{"op":"sync_case","args":[]}\n')
    stdout = io.StringIO()
    monkeypatch.setattr(service.threading, "Thread", ImmediateThread)
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
