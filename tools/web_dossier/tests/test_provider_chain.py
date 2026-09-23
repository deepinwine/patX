"""Provider chain fallback order and login surfacing."""
from web_dossier.models import DocumentType, ProsecutionDocument, ResultCode
from web_dossier.providers.chain import ProviderChain

FIRST_OA = ProsecutionDocument(
    jurisdiction="CN", application_number="CN202510469601.5",
    document_type=DocumentType.OFFICE_ACTION_FIRST,
    document_title="第一次审查意见通知书", official_date="2026-05-23",
    source="epo_global_dossier", oa_ordinal=1)


class CancelFalse:
    def is_set(self):
        return False


class FakeProvider:
    def __init__(self, provider_id, code, documents=None):
        self.provider_id = provider_id
        self._code = code
        self._documents = documents or []
        self.calls = 0
        self.ensure_login_called = False

    def list_documents(self, application_number, publication_number, cancel):
        from web_dossier.providers.base import SyncOutcome
        self.calls += 1
        return SyncOutcome(code=self._code, documents=list(self._documents))

    def ensure_login(self, cancel):
        self.ensure_login_called = True
        return ResultCode.OK


def test_chain_falls_back_uspto_then_epo_without_cnipa_login():
    chain = ProviderChain([
        FakeProvider("uspto_global_dossier", ResultCode.NETWORK_ERROR),
        FakeProvider("epo_global_dossier", ResultCode.OK, [FIRST_OA]),
        FakeProvider("cnipa", ResultCode.AUTH_REQUIRED),
    ])
    outcome = chain.list_documents("CN202510469601.5", "CN120134203A", CancelFalse())
    assert outcome.code == ResultCode.OK
    assert outcome.provider_used == "epo_global_dossier"
    assert [a.provider for a in outcome.attempts] == [
        "uspto_global_dossier", "epo_global_dossier"
    ]
    assert chain.providers[2].calls == 0     # CNIPA never touched


def test_chain_reaches_cnipa_and_surfaces_login_requirement():
    chain = ProviderChain([
        FakeProvider("uspto_global_dossier", ResultCode.ACCESS_DENIED),
        FakeProvider("epo_global_dossier", ResultCode.NETWORK_ERROR),
        FakeProvider("cnipa", ResultCode.AUTH_REQUIRED),
    ])
    outcome = chain.list_documents("CN202510469601.5", "", CancelFalse())
    assert outcome.code == ResultCode.AUTH_REQUIRED
    assert outcome.provider_used == "cnipa"
    assert [a.provider for a in outcome.attempts] == [
        "uspto_global_dossier", "epo_global_dossier", "cnipa"
    ]


def test_chain_empty_ok_stops_fallback():
    chain = ProviderChain([
        FakeProvider("uspto_global_dossier", ResultCode.OK),
        FakeProvider("epo_global_dossier", ResultCode.OK, [FIRST_OA]),
    ])
    outcome = chain.list_documents("CN202510469601.5", "", CancelFalse())
    assert outcome.code == ResultCode.OK
    assert outcome.documents == []
    assert chain.providers[1].calls == 0     # verified page, stop descending


def test_chain_all_failing_returns_last_outcome_with_trace():
    chain = ProviderChain([
        FakeProvider("uspto_global_dossier", ResultCode.NETWORK_ERROR),
        FakeProvider("epo_global_dossier", ResultCode.RATE_LIMITED),
        FakeProvider("cnipa", ResultCode.TEMPORARY_ERROR),
    ])
    outcome = chain.list_documents("CN202510469601.5", "", CancelFalse())
    assert outcome.code == ResultCode.TEMPORARY_ERROR
    assert len(outcome.attempts) == 3
    assert outcome.attempts[-1].code == ResultCode.TEMPORARY_ERROR
