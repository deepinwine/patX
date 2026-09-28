"""Ordered, auditable fallback across dossier metadata providers."""
from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Optional

from ..models import ProsecutionDocument, ResultCode
from .base import SyncOutcome


FALLBACK_CODES = frozenset({
    ResultCode.NETWORK_ERROR,
    ResultCode.TEMPORARY_ERROR,
    ResultCode.RATE_LIMITED,
    ResultCode.ACCESS_DENIED,
    ResultCode.PAGE_STRUCTURE_CHANGED,
    ResultCode.CASE_NOT_FOUND,
    ResultCode.RESOLVE_FAILED,
})


@dataclass(frozen=True)
class ProviderAttempt:
    provider: str
    code: ResultCode
    message: str = ""

    def to_dict(self) -> dict:
        return {
            "provider": self.provider,
            "code": self.code.value,
            "message": self.message,
        }


@dataclass
class ChainOutcome(SyncOutcome):
    @classmethod
    def from_outcome(
        cls,
        outcome: SyncOutcome,
        provider_used: str,
        attempts: Iterable[ProviderAttempt],
    ) -> "ChainOutcome":
        if not isinstance(outcome.code, ResultCode):
            raise TypeError("provider returned an invalid result code")
        for value in (
            outcome.message,
            outcome.auth_state,
            outcome.resolved_application_number,
        ):
            if not isinstance(value, str):
                raise TypeError("provider returned invalid outcome metadata")
        documents = list(outcome.documents)
        if any(not isinstance(document, ProsecutionDocument) for document in documents):
            raise TypeError("provider returned an invalid document")
        return cls(
            code=outcome.code,
            message=outcome.message,
            auth_state=outcome.auth_state,
            documents=documents,
            resolved_application_number=outcome.resolved_application_number,
            provider_used=provider_used,
            attempts=list(attempts),
        )

def _cancelled(cancel) -> bool:
    return cancel is not None and cancel.is_set()


class ProviderChain:
    """Call providers exactly in caller-supplied order, without login flows."""

    def __init__(self, providers):
        self.providers = list(providers)

    def list_documents(
        self, application_number: str, publication_number: str, cancel
    ) -> ChainOutcome:
        attempts: list[ProviderAttempt] = []
        if _cancelled(cancel):
            return ChainOutcome(
                code=ResultCode.TEMPORARY_ERROR,
                message="案卷查询已取消",
            )
        if not self.providers:
            return ChainOutcome(
                code=ResultCode.UNSUPPORTED_JURISDICTION,
                message="没有可用的案卷查询 Provider",
            )

        last_outcome: Optional[ChainOutcome] = None
        for provider in self.providers:
            provider_id = type(provider).__name__
            try:
                provider_id = str(
                    getattr(provider, "provider_id", "") or provider_id
                )
                raw_outcome = provider.list_documents(
                    application_number, publication_number, cancel
                )
                if not isinstance(raw_outcome, SyncOutcome):
                    raise TypeError("provider returned a non-SyncOutcome result")
                attempt = ProviderAttempt(
                    provider_id, raw_outcome.code, raw_outcome.message
                )
                current = ChainOutcome.from_outcome(
                    raw_outcome, provider_id, [*attempts, attempt]
                )
            except Exception as exc:  # noqa: BLE001 - sidecar safety boundary
                raw_outcome = SyncOutcome(
                    code=ResultCode.TEMPORARY_ERROR,
                    message=f"Provider 内部错误: {type(exc).__name__}",
                )
                attempt = ProviderAttempt(
                    provider_id, raw_outcome.code, raw_outcome.message
                )
                current = ChainOutcome.from_outcome(
                    raw_outcome, provider_id, [*attempts, attempt]
                )

            attempts.append(attempt)
            last_outcome = current

            if current.ok:
                return current
            if current.code in (ResultCode.AUTH_REQUIRED, ResultCode.SESSION_EXPIRED):
                return current
            if current.code not in FALLBACK_CODES:
                return current
            if _cancelled(cancel):
                return ChainOutcome(
                    code=ResultCode.TEMPORARY_ERROR,
                    message="案卷查询已取消",
                    provider_used=provider_id,
                    attempts=list(attempts),
                )

        return last_outcome or ChainOutcome(
            code=ResultCode.UNSUPPORTED_JURISDICTION,
            message="没有可用的案卷查询 Provider",
        )
