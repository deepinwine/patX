#!/usr/bin/env python3
"""patX web dossier sidecar - stdin/stdout line JSON-RPC.

Protocol (one JSON object per line):
    -> {"op": "ping"}
    <- {"op": "pong", "python_ok": true}

    -> {"op": "login",        "args": {"provider": "cnipa"}}
    <- {"ok": true, "code": "OK"}

    -> {"op": "sync_case",    "args": {"application_number": "...",
                                       "publication_number": "..."}}
    <- {"ok": true, "code": "NO_CHANGE"|"NEW_OFFICIAL_EVENT"|...,
        "message": "...", "auth_state": "...",
        "provider_used": "uspto_global_dossier"|"epo_global_dossier"|"cnipa",
        "attempts": [{"provider": ..., "code": ..., "message": ...}],
        "resolved_application_number": "...",
        "documents": [...], "latest_event": {...}|null,
        "latest_oa": null}

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
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from web_dossier.models import ResultCode, pick_latest_official_event     # noqa: E402
from web_dossier.providers.cnipa import CNIPAWebProvider                    # noqa: E402

_PROVIDERS = {}
_CHAIN = None


def _get_provider(name: str):
    if name != "cnipa":
        return None, ResultCode.UNSUPPORTED_JURISDICTION
    if name not in _PROVIDERS:
        from web_dossier.browser.manager import BrowserManager
        manager = BrowserManager("cnipa")
        _PROVIDERS[name] = CNIPAWebProvider(browser_manager=manager)
    return _PROVIDERS[name], None


def _get_chain():
    """Lazily build the USPTO -> EPO -> CNIPA chain. Constructing a provider
    must never open a browser; the USPTO headless context only launches on
    first use."""
    global _CHAIN
    if _CHAIN is None:
        from web_dossier.browser.manager import BrowserManager
        from web_dossier.providers.chain import ProviderChain
        from web_dossier.providers.epo_global_dossier import EpoGlobalDossierProvider
        from web_dossier.providers.uspto_global_dossier import UsptoGlobalDossierProvider
        uspto = UsptoGlobalDossierProvider(
            BrowserManager("uspto_global_dossier", headless=True,
                           prefer_system_browser=False))
        epo = EpoGlobalDossierProvider()
        cnipa = CNIPAWebProvider(browser_manager=BrowserManager("cnipa"))
        _CHAIN = ProviderChain([uspto, epo, cnipa])
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
    chain = _get_chain()
    app_no = args.get("application_number", "")
    pub_no = args.get("publication_number", "")

    outcome = chain.list_documents(app_no, pub_no, cancel)
    if outcome.auth_state in ("", "NOT_INITIALIZED") and outcome.ok:
        outcome.auth_state = "AUTHENTICATED"
    latest_event = pick_latest_official_event(outcome.documents)
    response = outcome.to_dict(latest_event)
    if (outcome.code == ResultCode.AUTH_REQUIRED
            and outcome.provider_used == "cnipa"):
        response["message"] = outcome.message or "CNIPA 登录状态已失效，请重新登录"
    return response


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

    def run_op(handler, args):
        if busy.is_set():
            send({"ok": False, "code": ResultCode.TEMPORARY_ERROR.value,
                  "message": "上一件案件的查询仍在进行，请稍候"})
            return

        def target():
            try:
                send(handler(args, cancel_event))
            except Exception:
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
        op = request.get("op", "")
        args = request.get("args", {}) or {}
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
            if _CHAIN is not None:
                for provider in _CHAIN.providers:
                    manager = getattr(provider, "_manager", None)
                    try:
                        if manager is not None:
                            manager.close()
                    except Exception:
                        pass
            send({"ok": True, "code": "OK"})
            return 0
        elif op == "login":
            run_op(_op_login, args)
        elif op == "sync_case":
            run_op(_op_sync_case, args)
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
