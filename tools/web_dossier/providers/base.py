"""DossierProvider interface - one abstraction over every data source.

Implementations exist for browser-assisted sites now (CNIPAWebProvider) and
REST APIs later (USPTOODPProvider, EPOOPSProvider) without any caller change.
"""
from __future__ import annotations

import abc
from dataclasses import dataclass, field
from collections.abc import Mapping
from typing import List, Optional

from ..models import ProsecutionDocument, ResultCode


@dataclass
class SyncOutcome:
    code: ResultCode = ResultCode.OK
    message: str = ""
    auth_state: str = "NOT_INITIALIZED"
    documents: List[ProsecutionDocument] = field(default_factory=list)
    resolved_application_number: str = ""
    provider_used: str = ""
    attempts: list = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.code in (ResultCode.OK, ResultCode.NO_CHANGE,
                             ResultCode.NEW_OFFICIAL_EVENT,
                             ResultCode.NEW_OFFICE_ACTION)

    @staticmethod
    def _serialize_document(document) -> Optional[dict]:
        try:
            payload = document.to_dict()
        except Exception:  # noqa: BLE001 - protocol serialization boundary
            return None
        return payload if isinstance(payload, dict) else None

    @staticmethod
    def _serialize_attempt(attempt) -> Optional[dict]:
        try:
            if isinstance(attempt, Mapping):
                payload = dict(attempt)
            else:
                payload = attempt.to_dict()
            if not isinstance(payload, Mapping):
                return None
            provider = payload.get("provider", "")
            code = payload.get("code", "")
            message = payload.get("message", "")
            if isinstance(code, ResultCode):
                code = code.value
            if not all(isinstance(value, str) for value in (provider, code, message)):
                return None
            return {"provider": provider, "code": code, "message": message}
        except Exception:  # noqa: BLE001 - protocol serialization boundary
            return None

    @staticmethod
    def _safe_items(value) -> list:
        try:
            return list(value or [])
        except Exception:  # noqa: BLE001 - protocol serialization boundary
            return []

    def to_dict(self, latest_event: Optional[ProsecutionDocument] = None,
                **legacy_kwargs) -> dict:
        if latest_event is None and "latest_oa" in legacy_kwargs:
            latest_event = legacy_kwargs["latest_oa"]
        code = self.code.value if isinstance(self.code, ResultCode) else str(self.code or "")
        documents = [
            payload
            for document in self._safe_items(self.documents)
            if (payload := self._serialize_document(document)) is not None
        ]
        attempts = [
            payload
            for attempt in self._safe_items(self.attempts)
            if (payload := self._serialize_attempt(attempt)) is not None
        ]
        latest_payload = self._serialize_document(latest_event) if latest_event else None
        return {
            "ok": self.ok,
            "code": code,
            "message": self.message if isinstance(self.message, str) else "",
            "auth_state": self.auth_state if isinstance(self.auth_state, str) else "",
            "provider_used": self.provider_used if isinstance(self.provider_used, str) else "",
            "attempts": attempts,
            "resolved_application_number": (
                self.resolved_application_number
                if isinstance(self.resolved_application_number, str) else ""
            ),
            "documents": documents,
            "latest_event": latest_payload,
            "latest_oa": None,
        }


class DossierProvider(abc.ABC):
    provider_id: str = ""
    jurisdiction: str = "CN"

    @abc.abstractmethod
    def health_check(self) -> bool:
        """Cheap check that the provider can run (deps present, not banned)."""

    @abc.abstractmethod
    def check_auth(self, cancel) -> ResultCode:
        """OK / AUTH_REQUIRED / SESSION_EXPIRED / NETWORK_ERROR."""

    @abc.abstractmethod
    def ensure_login(self, cancel) -> ResultCode:
        """Opens a visible browser for the user's own login; never reads
        passwords or automates captchas. Returns OK or AUTH_REQUIRED."""

    @abc.abstractmethod
    def resolve_case(self, application_number: str, publication_number: str,
                     cancel) -> SyncOutcome:
        """Resolves the case identifier the site needs (publication ->
        application when required)."""

    @abc.abstractmethod
    def list_documents(self, application_number: str, publication_number: str,
                       cancel) -> SyncOutcome:
        """Lists the public/authorized prosecution documents for the case."""

    @abc.abstractmethod
    def download_document(self, document: ProsecutionDocument, dest_path: str,
                          cancel) -> ResultCode:
        """Optional Phase-2 capability; may return UNSUPPORTED_JURISDICTION."""

    def get_latest_office_action(self, outcome: SyncOutcome) -> Optional[ProsecutionDocument]:
        from ..models import pick_latest_office_action
        return pick_latest_office_action(outcome.documents)
