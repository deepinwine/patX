"""Sidecar sync_case protocol: latest official event + provider trace."""
import dataclasses

import web_dossier.service as service
from web_dossier.models import (DocumentType, ProsecutionDocument, ResultCode,
                                pick_latest_official_event)
from web_dossier.providers.base import SyncOutcome

FIRST_OA = ProsecutionDocument(
    jurisdiction="CN", application_number="CN202510469601.5",
    document_type=DocumentType.OFFICE_ACTION_FIRST,
    document_title="第一次审查意见通知书", official_date="2026-05-23",
    source="uspto_global_dossier", oa_ordinal=1,
    document_version="ORIGINAL", direction="official")


class CancelFalse:
    def is_set(self):
        return False


class FakeSuccessfulChain:
    def list_documents(self, application_number, publication_number, cancel):
        return SyncOutcome(code=ResultCode.OK, documents=[FIRST_OA],
                           provider_used="uspto_global_dossier")


class FakeAuthRequiredChain:
    def __init__(self):
        self.ensure_login_called = False

    def ensure_login(self, cancel):
        self.ensure_login_called = True
        return ResultCode.OK

    def list_documents(self, application_number, publication_number, cancel):
        return SyncOutcome(code=ResultCode.AUTH_REQUIRED, provider_used="cnipa")


def test_sync_response_reports_provider_and_latest_official_event(monkeypatch):
    monkeypatch.setattr(service, "_get_chain", lambda: FakeSuccessfulChain())
    response = service._op_sync_case({
        "application_number": "CN202510469601.5",
        "publication_number": "CN120134203A",
    }, CancelFalse())
    assert response["ok"] is True
    assert response["provider_used"] == "uspto_global_dossier"
    assert response["latest_event"]["document_type"] == "OFFICE_ACTION_FIRST"
    assert response["latest_event"]["official_date"] == "2026-05-23"
    assert response["latest_oa"] is None


def test_background_sync_never_opens_cnipa_login(monkeypatch):
    chain = FakeAuthRequiredChain()
    monkeypatch.setattr(service, "_get_chain", lambda: chain)
    response = service._op_sync_case({"application_number": "CN202510469601.5"},
                                     CancelFalse())
    assert response["code"] == "AUTH_REQUIRED"
    assert chain.ensure_login_called is False


def test_pick_latest_official_event_filters_and_orders():
    translated = dataclasses.replace(FIRST_OA, document_version="TRANSLATED")
    applicant = ProsecutionDocument(
        application_number="CN202510469601.5",
        document_type=DocumentType.RESPONSE_TO_OFFICE_ACTION,
        document_title="意见陈述书", official_date="2026-07-01",
        direction="applicant", document_version="ORIGINAL")
    undated_grant = ProsecutionDocument(
        application_number="CN202510469601.5",
        document_type=DocumentType.GRANT_NOTICE,
        document_title="授权通知", official_date="",
        direction="official", document_version="ORIGINAL")
    older_oa = ProsecutionDocument(
        application_number="CN202510469601.5",
        document_type=DocumentType.OFFICE_ACTION_SECOND,
        document_title="第二次审查意见通知书", official_date="2026-03-01",
        direction="official", document_version="ORIGINAL", oa_ordinal=2)
    latest = pick_latest_official_event([translated, applicant, undated_grant,
                                         older_oa, FIRST_OA])
    assert latest is FIRST_OA
    assert pick_latest_official_event([translated, applicant, undated_grant]) is None
