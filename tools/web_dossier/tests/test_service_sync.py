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


# ---------------------------------------------------------------------------
# CNIPA 会话状态严格校验（移植自 codex 分支加固设计）
# ---------------------------------------------------------------------------
import base64
import json
import time as _time


def _enc(obj) -> str:
    raw = json.dumps(obj).encode()
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def _make_jwt(exp_offset: float) -> str:
    now = _time.time()
    return f"{_enc({'alg': 'HS256'})}.{_enc({'exp': now + exp_offset})}.{_enc({'sig': 'x'})}"


class _FakePage:
    def __init__(self, url, token):
        self.url = url
        self._token = token

    def evaluate(self, _script):
        return self._token


def _bare_manager(page):
    from web_dossier.browser.manager import BrowserManager
    mgr = BrowserManager.__new__(BrowserManager)   # 不触发 __init__ 路径逻辑
    mgr._page = page
    mgr._context = object()
    return mgr


def test_cpquery_session_state_valid_token():
    mgr = _bare_manager(_FakePage("https://cpquery.cponline.cnipa.gov.cn/",
                                  _make_jwt(3600)))
    assert mgr.cpquery_session_state(
        "https://cpquery.cponline.cnipa.gov.cn/") == "AUTHENTICATED"


def test_cpquery_session_state_expired_and_margin():
    mgr = _bare_manager(_FakePage("https://cpquery.cponline.cnipa.gov.cn/",
                                  _make_jwt(-10)))
    assert mgr.cpquery_session_state(
        "https://cpquery.cponline.cnipa.gov.cn/") == "SESSION_EXPIRED"
    # 剩余不足 TTL 余量（默认60秒）同样按过期
    mgr._page = _FakePage("https://cpquery.cponline.cnipa.gov.cn/",
                          _make_jwt(30))
    assert mgr.cpquery_session_state(
        "https://cpquery.cponline.cnipa.gov.cn/") == "SESSION_EXPIRED"


def test_cpquery_session_state_malformed_tokens():
    mgr = _bare_manager(None)
    url = "https://cpquery.cponline.cnipa.gov.cn/"
    for bad in ("not-a-jwt", "a.b", f"x.{_enc({'alg': 'none'})}",
                f"{_enc({'alg': 'HS256'})}.notbase64@@!.",
                _enc("stringpayload") + "." + _enc({"exp": 9999999999}) + ".sig",
                f"{_enc({'alg': 'HS256'})}.{_enc({'no_exp': True})}.sig"):
        mgr._page = _FakePage(url, bad)
        assert mgr.cpquery_session_state(url) == "SESSION_EXPIRED", bad


def test_cpquery_session_state_origin_and_missing_token():
    mgr = _bare_manager(None)
    # 不在 cpquery origin：需要登录
    mgr._page = _FakePage("https://tysf.cponline.cnipa.gov.cn/", _make_jwt(3600))
    assert mgr.cpquery_session_state(
        "https://cpquery.cponline.cnipa.gov.cn/") == "AUTH_REQUIRED"
    # 无 token
    mgr._page = _FakePage("https://cpquery.cponline.cnipa.gov.cn/", None)
    assert mgr.cpquery_session_state(
        "https://cpquery.cponline.cnipa.gov.cn/") == "AUTH_REQUIRED"
    # 未初始化
    from web_dossier.browser.manager import BrowserManager
    empty = BrowserManager.__new__(BrowserManager)
    empty._page = None
    empty._context = None
    assert empty.cpquery_session_state(
        "https://cpquery.cponline.cnipa.gov.cn/") == "NOT_INITIALIZED"
