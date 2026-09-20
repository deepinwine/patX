#!/usr/bin/env python3
"""patX web dossier sidecar - stdin/stdout line JSON-RPC.

Protocol (one JSON object per line):
    -> {"op": "ping"}
    <- {"op": "pong", "python_ok": true}

    -> {"op": "login",        "args": {"provider": "cnipa"}}
    <- {"ok": true, "code": "OK"}

    -> {"op": "sync_case",    "args": {"provider": "cnipa",
                                       "application_number": "...",
                                       "publication_number": "..."}}
    <- {"ok": true, "code": "NO_CHANGE"|"NEW_OFFICE_ACTION"|...,
        "message": "...", "auth_state": "...",
        "resolved_application_number": "...",
        "documents": [...], "latest_oa": {...}|null}

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

from web_dossier.models import ResultCode, pick_latest_office_action       # noqa: E402
from web_dossier.providers.cnipa import CNIPAWebProvider                    # noqa: E402

_PROVIDERS = {}


def _get_provider(name: str):
    if name != "cnipa":
        return None, ResultCode.UNSUPPORTED_JURISDICTION
    if name not in _PROVIDERS:
        from web_dossier.browser.manager import BrowserManager
        manager = BrowserManager("cnipa")
        _PROVIDERS[name] = CNIPAWebProvider(browser_manager=manager)
    return _PROVIDERS[name], None


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
    provider, err = _get_provider(args.get("provider", "cnipa"))
    if err:
        return {"ok": False, "code": err.value}
    app_no = args.get("application_number", "")
    pub_no = args.get("publication_number", "")

    auth = provider.check_auth(cancel)
    if auth in (ResultCode.AUTH_REQUIRED, ResultCode.SESSION_EXPIRED):
        return {"ok": False, "code": auth.value, "auth_state": auth.value,
                "message": "CNIPA 登录状态已失效，请重新登录"}
    if auth == ResultCode.RATE_LIMITED:
        return {"ok": False, "code": auth.value,
                "message": "站点限流提示，本批已停止，请稍后再试"}
    if auth != ResultCode.OK:
        return {"ok": False, "code": auth.value, "message": f"连通性检查失败: {auth.value}"}

    outcome = provider.list_documents(app_no, pub_no, cancel)
    if outcome.auth_state in ("", "NOT_INITIALIZED") and outcome.ok:
        outcome.auth_state = "AUTHENTICATED"
    latest_oa = provider.get_latest_office_action(outcome)
    response = outcome.to_dict(latest_oa)
    if latest_oa is not None:
        from web_dossier.document_classifier import oa_title_cn
        latest_oa.document_title = oa_title_cn(latest_oa.document_type,
                                               latest_oa.oa_ordinal) or latest_oa.document_title
        # rebuild fingerprint with the canonical title
        response["latest_oa"]["document_title"] = latest_oa.document_title
        response["latest_oa"]["fingerprint"] = latest_oa.fingerprint_value()
    return response


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
            send({"ok": True, "code": "OK"})
            return 0
        elif op == "login":
            run_op(_op_login, args)
        elif op == "sync_case":
            run_op(_op_sync_case, args)
        else:
            send({"ok": False, "code": ResultCode.TEMPORARY_ERROR.value,
                  "message": f"unknown op: {op}"})
    return 0


if __name__ == "__main__":
    sys.exit(main())
