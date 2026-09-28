from threading import Event

import pytest

from web_dossier.models import ProsecutionDocument, ResultCode
from web_dossier.providers.base import SyncOutcome
from web_dossier.providers.chain import ProviderAttempt, ProviderChain
from web_dossier.providers.cnipa import parse_cpquery_list_response


class FakeProvider:
    def __init__(self, provider_id, code=ResultCode.OK, documents=None,
                 message="", exception=None, cancel_after=False):
        self.provider_id = provider_id
        self.code = code
        self.documents = list(documents or [])
        self.message = message
        self.exception = exception
        self.cancel_after = cancel_after
        self.list_calls = 0
        self.login_calls = 0

    def list_documents(self, application_number, publication_number, cancel):
        self.list_calls += 1
        if self.exception is not None:
            raise self.exception
        if self.cancel_after:
            cancel.set()
        return SyncOutcome(
            code=self.code,
            message=self.message,
            documents=self.documents,
            resolved_application_number=application_number,
        )

    def ensure_login(self, cancel):
        self.login_calls += 1
        raise AssertionError("后台降级链不得调用 ensure_login")


FIRST_OA = ProsecutionDocument(
    source="epo_global_dossier",
    application_number="CN202510469601.5",
    document_title="第一次审查意见通知书",
    official_date="2026-05-23",
)


def test_chain_falls_back_uspto_then_epo_without_cnipa_login():
    uspto = FakeProvider("uspto_global_dossier", ResultCode.NETWORK_ERROR)
    epo = FakeProvider("epo_global_dossier", ResultCode.OK, [FIRST_OA])
    cnipa = FakeProvider("cnipa", ResultCode.AUTH_REQUIRED)
    chain = ProviderChain([uspto, epo, cnipa])

    outcome = chain.list_documents(
        "CN202510469601.5", "CN120134203A", Event()
    )

    assert outcome.code == ResultCode.OK
    assert outcome.provider_used == "epo_global_dossier"
    assert [attempt.provider for attempt in outcome.attempts] == [
        "uspto_global_dossier", "epo_global_dossier"
    ]
    assert cnipa.list_calls == 0
    assert all(provider.login_calls == 0 for provider in (uspto, epo, cnipa))


def test_chain_reaches_cnipa_and_surfaces_login_requirement_with_trace():
    providers = [
        FakeProvider("uspto_global_dossier", ResultCode.ACCESS_DENIED, message="blocked"),
        FakeProvider("epo_global_dossier", ResultCode.NETWORK_ERROR, message="offline"),
        FakeProvider("cnipa", ResultCode.AUTH_REQUIRED, message="login needed"),
    ]

    outcome = ProviderChain(providers).list_documents(
        "CN202510469601.5", "", Event()
    )

    assert outcome.code == ResultCode.AUTH_REQUIRED
    assert outcome.provider_used == "cnipa"
    assert [attempt.to_dict() for attempt in outcome.attempts] == [
        {"provider": "uspto_global_dossier", "code": "ACCESS_DENIED", "message": "blocked"},
        {"provider": "epo_global_dossier", "code": "NETWORK_ERROR", "message": "offline"},
        {"provider": "cnipa", "code": "AUTH_REQUIRED", "message": "login needed"},
    ]
    assert all(provider.login_calls == 0 for provider in providers)


def test_chain_stops_on_epo_verified_empty_ok():
    epo = FakeProvider("epo_global_dossier", ResultCode.OK, [])
    cnipa = FakeProvider("cnipa", ResultCode.OK, [FIRST_OA])
    outcome = ProviderChain([epo, cnipa]).list_documents("CN202510469601.5", "", Event())

    assert outcome.code == ResultCode.OK
    assert outcome.documents == []
    assert outcome.provider_used == "epo_global_dossier"
    assert cnipa.list_calls == 0


def test_chain_stops_on_non_fallback_error():
    first = FakeProvider("epo_global_dossier", ResultCode.DATE_PARSE_FAILED)
    second = FakeProvider("cnipa", ResultCode.OK, [FIRST_OA])
    outcome = ProviderChain([first, second]).list_documents("CN202510469601.5", "", Event())

    assert outcome.code == ResultCode.DATE_PARSE_FAILED
    assert outcome.provider_used == "epo_global_dossier"
    assert second.list_calls == 0


@pytest.mark.parametrize(
    "code",
    [
        ResultCode.NETWORK_ERROR,
        ResultCode.TEMPORARY_ERROR,
        ResultCode.RATE_LIMITED,
        ResultCode.ACCESS_DENIED,
        ResultCode.PAGE_STRUCTURE_CHANGED,
        ResultCode.CASE_NOT_FOUND,
        ResultCode.RESOLVE_FAILED,
    ],
)
def test_chain_falls_back_only_for_the_explicit_retryable_codes(code):
    first = FakeProvider("public", code)
    second = FakeProvider("fallback", ResultCode.OK, [FIRST_OA])

    outcome = ProviderChain([first, second]).list_documents(
        "CN202510469601.5", "", Event()
    )

    assert outcome.code == ResultCode.OK
    assert outcome.provider_used == "fallback"
    assert [attempt.code for attempt in outcome.attempts] == [code, ResultCode.OK]


@pytest.mark.parametrize(
    "code", [ResultCode.AUTH_REQUIRED, ResultCode.SESSION_EXPIRED]
)
def test_chain_stops_on_auth_state_without_login_or_later_provider(code):
    auth = FakeProvider("cnipa", code)
    later = FakeProvider("later", ResultCode.OK, [FIRST_OA])

    outcome = ProviderChain([auth, later]).list_documents(
        "CN202510469601.5", "", Event()
    )

    assert outcome.code == code
    assert outcome.provider_used == "cnipa"
    assert [attempt.code for attempt in outcome.attempts] == [code]
    assert auth.login_calls == 0
    assert later.list_calls == 0


def test_chain_returns_last_failure_with_complete_attempt_trace():
    providers = [
        FakeProvider("uspto", ResultCode.NETWORK_ERROR, message="offline"),
        FakeProvider("epo", ResultCode.RATE_LIMITED, message="busy"),
    ]

    outcome = ProviderChain(providers).list_documents(
        "CN202510469601.5", "", Event()
    )

    assert outcome.code == ResultCode.RATE_LIMITED
    assert outcome.provider_used == "epo"
    assert [attempt.to_dict() for attempt in outcome.attempts] == [
        {"provider": "uspto", "code": "NETWORK_ERROR", "message": "offline"},
        {"provider": "epo", "code": "RATE_LIMITED", "message": "busy"},
    ]


def test_provider_attempt_and_chain_outcome_serialization_are_stable():
    attempt = ProviderAttempt("epo_global_dossier", ResultCode.NETWORK_ERROR, "offline")
    assert attempt.to_dict() == {
        "provider": "epo_global_dossier",
        "code": "NETWORK_ERROR",
        "message": "offline",
    }

    outcome = ProviderChain([
        FakeProvider("epo_global_dossier", ResultCode.OK, [FIRST_OA])
    ]).list_documents("CN202510469601.5", "", Event())
    payload = outcome.to_dict(FIRST_OA)
    assert payload["provider_used"] == "epo_global_dossier"
    assert payload["attempts"] == [
        {"provider": "epo_global_dossier", "code": "OK", "message": ""}
    ]
    assert payload["latest_event"]["remote_document_id"] == FIRST_OA.remote_document_id
    assert payload["latest_oa"] is None

    keyword_payload = outcome.to_dict(latest_event=FIRST_OA)
    assert keyword_payload == payload


def test_chain_honours_cancel_before_and_between_providers():
    cancel = Event()
    cancel.set()
    never = FakeProvider("uspto_global_dossier", ResultCode.OK)
    before = ProviderChain([never]).list_documents("CN202510469601.5", "", cancel)
    assert before.code == ResultCode.TEMPORARY_ERROR
    assert before.provider_used == ""
    assert before.attempts == []
    assert never.list_calls == 0

    cancel = Event()
    first = FakeProvider(
        "uspto_global_dossier", ResultCode.NETWORK_ERROR, cancel_after=True
    )
    second = FakeProvider("epo_global_dossier", ResultCode.OK)
    between = ProviderChain([first, second]).list_documents(
        "CN202510469601.5", "", cancel
    )
    assert between.code == ResultCode.TEMPORARY_ERROR
    assert between.provider_used == "uspto_global_dossier"
    assert [attempt.provider for attempt in between.attempts] == [
        "uspto_global_dossier"
    ]
    assert second.list_calls == 0


def test_provider_exception_becomes_safe_attempt_and_can_fall_back():
    broken = FakeProvider(
        "uspto_global_dossier", exception=RuntimeError("cookie=secret")
    )
    epo = FakeProvider("epo_global_dossier", ResultCode.OK, [FIRST_OA])
    outcome = ProviderChain([broken, epo]).list_documents(
        "CN202510469601.5", "", Event()
    )

    assert outcome.code == ResultCode.OK
    assert outcome.provider_used == "epo_global_dossier"
    assert outcome.attempts[0].code == ResultCode.TEMPORARY_ERROR
    assert "secret" not in outcome.attempts[0].message
    assert "RuntimeError" in outcome.attempts[0].message


def test_invalid_provider_result_becomes_safe_attempt_and_can_fall_back():
    class InvalidProvider:
        provider_id = "invalid"

        def list_documents(self, application_number, publication_number, cancel):
            return object()

    fallback = FakeProvider("epo_global_dossier", ResultCode.OK, [FIRST_OA])
    outcome = ProviderChain([InvalidProvider(), fallback]).list_documents(
        "CN202510469601.5", "", Event()
    )

    assert outcome.code == ResultCode.OK
    assert outcome.provider_used == "epo_global_dossier"
    assert outcome.attempts[0].code == ResultCode.TEMPORARY_ERROR
    assert "TypeError" in outcome.attempts[0].message


def test_malformed_sync_outcome_becomes_safe_attempt_and_can_fall_back():
    class MalformedProvider:
        provider_id = "malformed"

        def list_documents(self, application_number, publication_number, cancel):
            return SyncOutcome(
                code=ResultCode.NETWORK_ERROR,
                documents=None,
            )

    fallback = FakeProvider("epo_global_dossier", ResultCode.OK, [FIRST_OA])
    outcome = ProviderChain([MalformedProvider(), fallback]).list_documents(
        "CN202510469601.5", "", Event()
    )

    assert outcome.code == ResultCode.OK
    assert outcome.provider_used == "epo_global_dossier"
    assert [attempt.code for attempt in outcome.attempts] == [
        ResultCode.TEMPORARY_ERROR,
        ResultCode.OK,
    ]
    assert "TypeError" in outcome.attempts[0].message


def test_sync_outcome_with_invalid_code_cannot_short_circuit_the_chain():
    class MalformedProvider:
        provider_id = "malformed"

        def list_documents(self, application_number, publication_number, cancel):
            return SyncOutcome(code="OK")

    fallback = FakeProvider("epo_global_dossier", ResultCode.OK, [FIRST_OA])
    outcome = ProviderChain([MalformedProvider(), fallback]).list_documents(
        "CN202510469601.5", "", Event()
    )

    assert outcome.code == ResultCode.OK
    assert outcome.provider_used == "epo_global_dossier"
    assert [attempt.code for attempt in outcome.attempts] == [
        ResultCode.TEMPORARY_ERROR,
        ResultCode.OK,
    ]


def test_empty_provider_chain_has_explicit_safe_result():
    outcome = ProviderChain([]).list_documents("CN202510469601.5", "", Event())
    assert outcome.code == ResultCode.UNSUPPORTED_JURISDICTION
    assert outcome.provider_used == ""
    assert outcome.attempts == []
    assert outcome.message


def test_cnipa_documents_set_unified_source_version_and_document_code():
    body = """{
      "code": 200,
      "data": [{
        "name": "2026-05-23  第一次审查意见通知书",
        "ds": "TZS",
        "additionalData": {"rid": "rid-1", "wenjiandm": "210401"}
      }]
    }"""
    outcome = parse_cpquery_list_response(
        [("/api/view/gn/scxx/tzs", body)], "CN202510469601.5", "CN120134203A"
    )
    document = outcome.documents[0]
    assert document.source == "cnipa"
    assert document.document_version == "ORIGINAL"
    assert document.document_code == "210401"
