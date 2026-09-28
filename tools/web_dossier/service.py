#!/usr/bin/env python3
"""patX web dossier sidecar - stdin/stdout line JSON-RPC.

Protocol (one JSON object per line):
    -> {"op": "ping"}
    <- {"op": "pong", "python_ok": true}

    -> {"op": "login",        "args": {"provider": "cnipa"}}
    <- {"ok": true, "code": "OK"}

    -> {"op": "sync_case",    "args": {"application_number": "...",
                                       "publication_number": "..."}}
    <- {"ok": true, "code": "OK"|"NEW_OFFICIAL_EVENT"|...,
        "message": "...", "auth_state": "...",
        "provider_used": "...", "attempts": [...],
        "resolved_application_number": "...",
        "documents": [...], "latest_event": {...}|null, "latest_oa": null}

    -> {"op": "cancel"}     sets the cancel event for the in-flight call
    -> {"op": "shutdown"}   closes browsers and exits

Long ops run on a single worker thread while the main thread keeps reading
stdin, so "cancel" is always honored. stdout carries ONLY protocol lines;
diagnostics go to stderr. Nothing here ever logs cookies or passwords.
"""
from __future__ import annotations

import json
import os
import sys
import threading
import traceback
from collections.abc import Mapping
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from web_dossier.models import ResultCode, pick_latest_official_event      # noqa: E402
from web_dossier.providers.base import SyncOutcome                         # noqa: E402
from web_dossier.providers.cnipa import CNIPAWebProvider                    # noqa: E402

_PROVIDERS = {}
_CHAIN = None


class _BackgroundCnipaProvider:
    """Use CNIPA only when an authenticated browser session already exists."""

    provider_id = "cnipa"

    def __init__(self, provider):
        self._provider = provider

    def _has_reusable_authenticated_session(self) -> bool:
        manager = getattr(self._provider, "_manager", None)
        if manager is None:
            return False
        if getattr(manager, "_context", None) is None:
            return False
        if getattr(manager, "_page", None) is None:
            return False
        has_token = getattr(manager, "has_cpquery_token", None)
        if not callable(has_token):
            return False
        try:
            return has_token() is True
        except Exception:  # noqa: BLE001 - read-only provider state boundary
            return False

    def list_documents(self, application_number, publication_number, cancel):
        if not self._has_reusable_authenticated_session():
            return SyncOutcome(
                code=ResultCode.AUTH_REQUIRED,
                message="CNIPA 需要已有的已认证浏览器会话，请先显式登录",
                auth_state="AUTH_REQUIRED",
            )
        return self._provider.list_documents(
            application_number, publication_number, cancel
        )


def _get_provider(name: str):
    if name != "cnipa":
        return None, ResultCode.UNSUPPORTED_JURISDICTION
    if name not in _PROVIDERS:
        from web_dossier.browser.manager import BrowserManager
        manager = BrowserManager("cnipa")
        _PROVIDERS[name] = CNIPAWebProvider(browser_manager=manager)
    return _PROVIDERS[name], None


def _get_chain():
    global _CHAIN
    if _CHAIN is not None:
        return _CHAIN

    from web_dossier.browser.manager import BrowserManager
    from web_dossier.providers.chain import ProviderChain
    from web_dossier.providers.epo_global_dossier import EpoGlobalDossierProvider
    from web_dossier.providers.uspto_global_dossier import UsptoGlobalDossierProvider

    if "uspto_global_dossier" not in _PROVIDERS:
        manager = BrowserManager(
            "uspto_global_dossier",
            headless=True,
            prefer_system_browser=False,
        )
        _PROVIDERS["uspto_global_dossier"] = UsptoGlobalDossierProvider(
            browser_manager=manager
        )
    if "epo_global_dossier" not in _PROVIDERS:
        _PROVIDERS["epo_global_dossier"] = EpoGlobalDossierProvider()
    cnipa, err = _get_provider("cnipa")
    if err:
        raise RuntimeError("CNIPA provider unavailable")
    _CHAIN = ProviderChain([
        _PROVIDERS["uspto_global_dossier"],
        _PROVIDERS["epo_global_dossier"],
        _BackgroundCnipaProvider(cnipa),
    ])
    return _CHAIN


def _op_ping(_args):
    try:
        import playwright  # noqa: F401
        return {"op": "pong", "python_ok": True}
    except ImportError as exc:
        return {"op": "pong", "python_ok": False, "missing": str(exc)}


def _op_login(args, cancel):
    provider, err = _get_provider(args.get("provider", "cnipa"))
    if err:
        return {"ok": False, "code": err.value}
    code = provider.ensure_login(cancel)
    return {"ok": code == ResultCode.OK, "code": code.value,
            "auth_state": "AUTHENTICATED" if code == ResultCode.OK else "AUTH_REQUIRED"}


def _op_sync_case(args, cancel):
    try:
        if not isinstance(args, Mapping):
            raise TypeError("sync_case args must be a JSON object")
        app_no = args.get("application_number", "")
        pub_no = args.get("publication_number", "")
        outcome = _get_chain().list_documents(app_no, pub_no, cancel)
        if not isinstance(outcome, SyncOutcome):
            raise TypeError("provider chain returned an invalid outcome")
        latest_event = pick_latest_official_event(outcome.documents)
        # Bypass ChainOutcome's legacy latest_oa override; SyncOutcome owns the
        # protocol shape and safely serializes task-3 ProviderAttempt objects.
        response = SyncOutcome.to_dict(outcome, latest_event)
        if response["code"] == ResultCode.NEW_OFFICE_ACTION.value:
            response["code"] = ResultCode.NEW_OFFICIAL_EVENT.value
        return response
    except Exception as exc:  # noqa: BLE001 - sidecar safety boundary
        return SyncOutcome(
            code=ResultCode.TEMPORARY_ERROR,
            message=f"Provider 链内部错误: {type(exc).__name__}",
        ).to_dict()


def _op_epo(args, cancel):
    """epo_resolve / epo_family. Credentials come per-call from the C++ side
    (local git-ignored config); they are used once and never logged."""
    from web_dossier.providers.epo_ops import EpoOpsProvider
    provider = EpoOpsProvider(args.get("consumer_key", ""), args.get("consumer_secret", ""))
    op = args.get("epo_op", "")
    if not provider.health_check():
        return {"ok": False, "code": "RESOLVE_FAILED",
                "message": "未配置 EPO consumer key/secret（developers.epo.org 注册应用获取）"}
    if op == "resolve":
        out = provider.resolve_publication(args.get("publication_number", ""))
        return {"ok": out.ok, "code": out.code.value, "message": out.message,
                "resolved_application_number": out.resolved_application_number}
    if op == "family":
        out = provider.family(args.get("publication_number", ""))
        return {"ok": out.ok, "code": out.code.value, "message": out.message,
                "members": [m.to_dict() for m in provider.last_family]}
    if op == "legal":
        out = provider.legal_status(args.get("application_number", ""),
                                    args.get("publication_number", ""))
        return {"ok": out.ok, "code": out.code.value, "message": out.message,
                "events": provider.last_events}
    if op == "citations":
        out = provider.citations(args.get("application_number", ""),
                                 args.get("publication_number", ""))
        return {"ok": out.ok, "code": out.code.value, "message": out.message,
                "citations": provider.last_citations}
    return {"ok": False, "code": "TEMPORARY_ERROR", "message": f"unknown epo_op {op!r}"}


def _op_download_document(args, cancel):
    provider, err = _get_provider(args.get("provider", "cnipa"))
    if err:
        return {"ok": False, "code": err.value}
    from web_dossier.models import ProsecutionDocument as _Doc
    doc = _Doc(
        application_number=args.get("application_number", ""),
        remote_document_id=args.get("rid", ""),
        document_title=args.get("title", ""),
        official_date=args.get("official_date", ""),
        ds=args.get("ds", "TZS"),
        wenjiandm=args.get("wenjiandm", "100000"),
    )
    code, info = provider.download_document(doc, args.get("dest_dir", "data/dossiers/CN"), cancel)
    return {"ok": code == ResultCode.OK, "code": code.value,
            "message": info.get("message", ""),
            "saved_path": info.get("saved_path", ""),
            "page_count": info.get("page_count", 0)}


def main() -> int:
    busy = threading.Event()          # one long op at a time; the main thread
    cancel_event = threading.Event()  # stays free to read "cancel"/"shutdown"
    out_lock = threading.Lock()

    def send(obj: dict):
        with out_lock:
            sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
            sys.stdout.flush()

    def run_op(handler, args, *, sync_protocol=False):
        if busy.is_set():
            if sync_protocol:
                send(SyncOutcome(
                    code=ResultCode.TEMPORARY_ERROR,
                    message="上一件案件的查询仍在进行，请稍候",
                ).to_dict())
            else:
                send({"ok": False, "code": ResultCode.TEMPORARY_ERROR.value,
                      "message": "上一件案件的查询仍在进行，请稍候"})
            return

        def target():
            try:
                send(handler(args, cancel_event))
            except Exception as exc:
                if sync_protocol:
                    send(SyncOutcome(
                        code=ResultCode.TEMPORARY_ERROR,
                        message=f"sidecar 内部错误: {type(exc).__name__}",
                    ).to_dict())
                else:
                    send({"ok": False, "code": ResultCode.TEMPORARY_ERROR.value,
                          "message": "sidecar 内部错误（详见 stderr）"})
                traceback.print_exc(file=sys.stderr)
            finally:
                busy.clear()

        busy.set()
        cancel_event.clear()
        threading.Thread(target=target, daemon=False).start()

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError:
            send({"ok": False, "code": ResultCode.TEMPORARY_ERROR.value,
                  "message": "malformed JSON request"})
            continue
        if not isinstance(request, Mapping):
            send({"ok": False, "code": ResultCode.TEMPORARY_ERROR.value,
                  "message": "JSON request must be an object"})
            continue
        op = request.get("op", "")
        args = request.get("args", {})
        if not isinstance(args, Mapping):
            if op == "sync_case":
                run_op(_op_sync_case, args, sync_protocol=True)
            else:
                send({"ok": False, "code": ResultCode.TEMPORARY_ERROR.value,
                      "message": "request args must be an object"})
            continue
        if op == "ping":
            send(_op_ping(args))
        elif op == "cancel":
            cancel_event.set()
        elif op == "shutdown":
            for provider in _PROVIDERS.values():
                try:
                    provider._manager.close()
                except Exception:
                    pass
            send({"ok": True, "code": "OK"})
            return 0
        elif op == "login":
            run_op(_op_login, args)
        elif op == "sync_case":
            run_op(_op_sync_case, args, sync_protocol=True)
        elif op == "download_document":
            run_op(_op_download_document, args)
        elif op == "epo":
            run_op(_op_epo, args)
        else:
            send({"ok": False, "code": ResultCode.TEMPORARY_ERROR.value,
                  "message": f"unknown op: {op}"})
    return 0


if __name__ == "__main__":
    sys.exit(main())
