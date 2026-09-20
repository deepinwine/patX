"""DossierProvider interface - one abstraction over every data source.

Implementations exist for browser-assisted sites now (CNIPAWebProvider) and
REST APIs later (USPTOODPProvider, EPOOPSProvider) without any caller change.
"""
from __future__ import annotations

import abc
from dataclasses import dataclass, field
from typing import List, Optional

from ..models import ProsecutionDocument, ResultCode


@dataclass
class SyncOutcome:
    code: ResultCode = ResultCode.OK
    message: str = ""
    auth_state: str = "NOT_INITIALIZED"
    documents: List[ProsecutionDocument] = field(default_factory=list)
    resolved_application_number: str = ""

    @property
    def ok(self) -> bool:
        return self.code in (ResultCode.OK, ResultCode.NO_CHANGE,
                             ResultCode.NEW_OFFICE_ACTION)

    def to_dict(self, latest_oa: Optional[ProsecutionDocument]) -> dict:
        d = {
            "ok": self.ok,
            "code": self.code.value,
            "message": self.message,
            "auth_state": self.auth_state,
            "resolved_application_number": self.resolved_application_number,
            "documents": [doc.to_dict() for doc in self.documents],
            "latest_oa": latest_oa.to_dict() if latest_oa else None,
        }
        return d


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
