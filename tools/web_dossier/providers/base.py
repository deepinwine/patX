"""DossierProvider interface - one abstraction over every data source.

Implementations exist for browser-assisted sites now (CNIPAWebProvider) and
REST APIs later (USPTOODPProvider, EPOOPSProvider) without any caller change.
"""
from __future__ import annotations

import abc
import json
from collections.abc import Mapping
from dataclasses import dataclass, field
from typing import List, Optional

from ..models import DocumentType, ProsecutionDocument, ResultCode


class ProtocolSerializationError(ValueError):
    """A sidecar outcome contains data that cannot be serialized safely."""


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
    def _serialize_document(document, label: str = "document") -> dict:
        try:
            payload = document.to_dict()
        except Exception as exc:  # noqa: BLE001 - protocol serialization boundary
            raise ProtocolSerializationError(
                f"{label} to_dict failed: {type(exc).__name__}"
            ) from exc
        if not isinstance(payload, dict):
            raise ProtocolSerializationError(f"{label} to_dict must return dict")
        valid_document_types = {item.value for item in DocumentType}
        if payload.get("document_type") not in valid_document_types:
            raise ProtocolSerializationError(f"{label} has invalid document_type")
        try:
            json.dumps(payload, ensure_ascii=False, allow_nan=False)
        except (TypeError, ValueError) as exc:
            raise ProtocolSerializationError(
                f"{label} contains non-JSON data"
            ) from exc
        return payload

    @staticmethod
    def _serialize_attempt(attempt) -> dict:
        try:
            if isinstance(attempt, Mapping):
                payload = dict(attempt)
            else:
                payload = attempt.to_dict()
        except Exception as exc:  # noqa: BLE001 - protocol serialization boundary
            raise ProtocolSerializationError(
                f"attempt to_dict failed: {type(exc).__name__}"
            ) from exc
        if not isinstance(payload, dict):
            raise ProtocolSerializationError("attempt to_dict must return dict")
        provider = payload.get("provider", "")
        code = payload.get("code", "")
        message = payload.get("message", "")
        if isinstance(code, ResultCode):
            code = code.value
        valid_codes = {item.value for item in ResultCode}
        if not isinstance(provider, str) or not provider:
            raise ProtocolSerializationError("attempt has invalid provider")
        if code not in valid_codes:
            raise ProtocolSerializationError("attempt has invalid code")
        if not isinstance(message, str):
            raise ProtocolSerializationError("attempt has invalid message")
        return {"provider": provider, "code": code, "message": message}

    @staticmethod
    def _strict_items(value, label: str) -> list:
        try:
            return list(value)
        except Exception as exc:  # noqa: BLE001 - protocol serialization boundary
            raise ProtocolSerializationError(f"{label} must be iterable") from exc

    def to_dict(self, latest_event: Optional[ProsecutionDocument] = None,
                **legacy_kwargs) -> dict:
        if latest_event is None and "latest_oa" in legacy_kwargs:
            latest_event = legacy_kwargs["latest_oa"]
        if not isinstance(self.code, ResultCode):
            raise ProtocolSerializationError("outcome has invalid code")
        for label, value in (
            ("message", self.message),
            ("auth_state", self.auth_state),
            ("provider_used", self.provider_used),
            ("resolved_application_number", self.resolved_application_number),
        ):
            if not isinstance(value, str):
                raise ProtocolSerializationError(f"outcome has invalid {label}")
        documents = [
            self._serialize_document(document)
            for document in self._strict_items(self.documents, "documents")
        ]
        attempts = [
            self._serialize_attempt(attempt)
            for attempt in self._strict_items(self.attempts, "attempts")
        ]
        latest_payload = (
            self._serialize_document(latest_event, "latest_event")
            if latest_event is not None else None
        )
        return {
            "ok": self.ok,
            "code": self.code.value,
            "message": self.message,
            "auth_state": self.auth_state,
            "provider_used": self.provider_used,
            "attempts": attempts,
            "resolved_application_number": self.resolved_application_number,
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

    def close(self) -> None:
        """Close provider-owned browser resources, when present."""
        manager = getattr(self, "_manager", None)
        if manager is not None:
            manager.close()
