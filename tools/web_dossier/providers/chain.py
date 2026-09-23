"""Fixed-order provider chain: USPTO Global Dossier -> EPO -> CNIPA.

Fall-through happens ONLY on the transient/structural codes below; any
outcome that proves the page was read (OK / NO_CHANGE / NEW_* / manual
review) stops the chain. A CNIPA login requirement is surfaced immediately -
background callers must never auto-open the login browser.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import List

from ..models import ResultCode
from .base import SyncOutcome

# codes that justify trying the next source
_FALLBACK_CODES = {
    ResultCode.NETWORK_ERROR,
    ResultCode.TEMPORARY_ERROR,
    ResultCode.RATE_LIMITED,
    ResultCode.ACCESS_DENIED,
    ResultCode.PAGE_STRUCTURE_CHANGED,
    ResultCode.CASE_NOT_FOUND,
    ResultCode.RESOLVE_FAILED,
}

# codes that stop the chain without falling through
_TERMINAL_CODES = {
    ResultCode.AUTH_REQUIRED,
    ResultCode.SESSION_EXPIRED,
}


@dataclass
class ProviderAttempt:
    provider: str
    code: ResultCode
    message: str = ""

    def to_dict(self) -> dict:
        return {"provider": self.provider, "code": self.code.value,
                "message": self.message}


class ProviderChain:
    def __init__(self, providers):
        self.providers = list(providers)

    def list_documents(self, application_number: str, publication_number: str,
                       cancel):
        attempts: List[ProviderAttempt] = []
        outcome = None
        for provider in self.providers:
            if cancel is not None and cancel.is_set():
                break
            outcome = provider.list_documents(application_number,
                                              publication_number, cancel)
            attempts.append(ProviderAttempt(provider.provider_id, outcome.code,
                                            outcome.message))
            outcome.provider_used = provider.provider_id
            outcome.attempts = list(attempts)
            if outcome.code in _TERMINAL_CODES:
                return outcome
            if outcome.code not in _FALLBACK_CODES:
                return outcome      # page read succeeded (OK/NO_CHANGE/...)
        if outcome is None:
            outcome = SyncOutcome(code=ResultCode.TEMPORARY_ERROR,
                                  message="没有可用的数据源")
        outcome.attempts = list(attempts)
        return outcome
