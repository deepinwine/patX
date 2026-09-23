"""Shared data models for the patX web dossier sidecar.

Result codes match the C++ enum in include/web_dossier.hpp - keep in sync.
"""
from __future__ import annotations

import datetime as _dt
import hashlib
import re
from dataclasses import dataclass, field
from enum import Enum
from typing import Optional


class ResultCode(str, Enum):
    OK = "OK"
    NO_CHANGE = "NO_CHANGE"
    NEW_OFFICIAL_EVENT = "NEW_OFFICIAL_EVENT"
    NEW_OFFICE_ACTION = "NEW_OFFICE_ACTION"
    AUTH_REQUIRED = "AUTH_REQUIRED"
    SESSION_EXPIRED = "SESSION_EXPIRED"
    CASE_NOT_FOUND = "CASE_NOT_FOUND"
    PUBLICATION_NOT_AVAILABLE = "PUBLICATION_NOT_AVAILABLE"
    ACCESS_DENIED = "ACCESS_DENIED"
    RESOLVE_FAILED = "RESOLVE_FAILED"
    PAGE_STRUCTURE_CHANGED = "PAGE_STRUCTURE_CHANGED"
    RATE_LIMITED = "RATE_LIMITED"
    TEMPORARY_ERROR = "TEMPORARY_ERROR"
    NETWORK_ERROR = "NETWORK_ERROR"
    DATE_PARSE_FAILED = "DATE_PARSE_FAILED"
    DATE_CONFLICT = "DATE_CONFLICT"
    UNSUPPORTED_JURISDICTION = "UNSUPPORTED_JURISDICTION"
    MANUAL_REVIEW_REQUIRED = "MANUAL_REVIEW_REQUIRED"


class DocumentType(str, Enum):
    OFFICE_ACTION_FIRST = "OFFICE_ACTION_FIRST"
    OFFICE_ACTION_SECOND = "OFFICE_ACTION_SECOND"
    OFFICE_ACTION_NTH = "OFFICE_ACTION_NTH"
    OFFICE_ACTION_UNKNOWN = "OFFICE_ACTION_UNKNOWN"
    RESPONSE_TO_OFFICE_ACTION = "RESPONSE_TO_OFFICE_ACTION"
    CLAIMS_AMENDMENT = "CLAIMS_AMENDMENT"
    SPECIFICATION_AMENDMENT = "SPECIFICATION_AMENDMENT"
    REJECTION_DECISION = "REJECTION_DECISION"
    GRANT_NOTICE = "GRANT_NOTICE"
    CORRECTION_NOTICE = "CORRECTION_NOTICE"
    SEARCH_REPORT = "SEARCH_REPORT"
    OTHER_OFFICIAL = "OTHER_OFFICIAL"
    OTHER_APPLICANT = "OTHER_APPLICANT"
    UNKNOWN = "UNKNOWN"

    @property
    def is_office_action(self) -> bool:
        return self in (
            DocumentType.OFFICE_ACTION_FIRST,
            DocumentType.OFFICE_ACTION_SECOND,
            DocumentType.OFFICE_ACTION_NTH,
            DocumentType.OFFICE_ACTION_UNKNOWN,
        )


class Confidence(str, Enum):
    HIGH = "HIGH"
    MEDIUM = "MEDIUM"
    LOW = "LOW"


# --------------------------------------------------------------------------
# Date handling
# --------------------------------------------------------------------------

_FULLWIDTH = {ord(f): ord(t) for f, t in zip("０１２３４５６７８９．／－　", "0123456789./- ")}

_DATE_PATTERNS = [
    re.compile(r"(\d{4})[.\-/年]\s*(\d{1,2})[.\-/月]\s*(\d{1,2})日?"),
]


def normalize_date(text: str, today: Optional[_dt.date] = None) -> str:
    """2026-09-18 / 2026.09.18 / 2026/09/18 / 2026年09月18日 -> '2026-09-18'.

    Returns '' for unparseable text, empty input, or implausible dates
    (invalid calendar date, more than 1 day in the future).
    """
    if not text:
        return ""
    s = text.translate(_FULLWIDTH).strip()
    for pat in _DATE_PATTERNS:
        m = pat.search(s)
        if m:
            y, mo, d = int(m.group(1)), int(m.group(2)), int(m.group(3))
            try:
                date = _dt.date(y, mo, d)
            except ValueError:
                return ""
            ref = today or _dt.date.today()
            if date > ref + _dt.timedelta(days=1):
                return ""   # future date: do not write it to the database
            return date.isoformat()
    return ""


# --------------------------------------------------------------------------
# Documents
# --------------------------------------------------------------------------

def normalize_title(title: str) -> str:
    """Collapse whitespace/punctuation noise for a stable fingerprint input."""
    return re.sub(r"\s+", "", (title or "").translate(_FULLWIDTH))


def fingerprint(jurisdiction: str, application_number: str, document_title: str,
                official_date: str, remote_document_id: str = "") -> str:
    """SHA256 over the stable identity of a discovered document.

    When the site offers no remote id, callers pass a stable combination
    (e.g. row position + title) so the fingerprint still dedups.
    """
    basis = "|".join([
        jurisdiction or "",
        application_number or "",
        normalize_title(document_title),
        official_date or "",
        remote_document_id or "",
    ])
    return hashlib.sha256(basis.encode("utf-8")).hexdigest()


def _event_title_token(
    document_type: DocumentType, oa_ordinal: int, document_title: str
) -> str:
    if document_type.is_office_action:
        return f"OFFICE_ACTION:{document_type.value}:{oa_ordinal}"
    fixed_tokens = {
        DocumentType.REJECTION_DECISION: "REJECTION_DECISION",
        DocumentType.GRANT_NOTICE: "GRANT_NOTICE",
        DocumentType.CORRECTION_NOTICE: "CORRECTION_NOTICE",
    }
    return fixed_tokens.get(document_type, normalize_title(document_title))


def _event_application_number(jurisdiction: str, application_number: str) -> str:
    raw_number = application_number or ""
    if (jurisdiction or "").strip().upper() == "CN":
        from .number_resolver import cn_check_digit, normalize_cn_identifier

        normalized = raw_number.translate(_FULLWIDTH).translate(
            {ord("Ｘ"): "X", ord("ｘ"): "X"}
        )
        normalized = re.sub(r"\s+", "", normalized).upper()
        number = normalized.removeprefix("CN")

        if re.fullmatch(r"\d{12}", number):
            return f"CN{number}.{cn_check_digit(number)}"
        dotted = re.fullmatch(r"(\d{12})\.([0-9X])", number)
        if dotted:
            return f"CN{dotted.group(1)}.{dotted.group(2)}"
        compact = re.fullmatch(r"(\d{12})([0-9X])", number)
        if compact:
            return f"CN{compact.group(1)}.{compact.group(2)}"

        identifier = normalize_cn_identifier(raw_number)
        if identifier.number_type == "application":
            return identifier.normalized_number.upper()
        return normalized
    return re.sub(r"\s+", "", raw_number).upper()


@dataclass
class ProsecutionDocument:
    jurisdiction: str = "CN"
    application_number: str = ""
    publication_number: str = ""
    source: str = "cnipa"
    remote_document_id: str = ""
    document_type: DocumentType = DocumentType.UNKNOWN
    document_title: str = ""
    raw_title: str = ""
    official_date: str = ""
    direction: str = "official"
    source_url: str = ""
    download_url: str = ""
    download_available: bool = False
    confidence: Confidence = Confidence.HIGH
    oa_ordinal: int = 0
    ds: str = ""                         # cpquery list kind (TZS/ZJWJ/SQWJ)
    wenjiandm: str = ""                  # cpquery document code
    document_code: str = ""
    document_version: str = "ORIGINAL"
    source_trace: list[str] = field(default_factory=list)

    def to_dict(self) -> dict:
        return {
            "jurisdiction": self.jurisdiction,
            "application_number": self.application_number,
            "publication_number": self.publication_number,
            "source": self.source,
            "remote_document_id": self.remote_document_id,
            "document_type": self.document_type.value,
            "document_title": self.document_title,
            "raw_title": self.raw_title,
            "official_date": self.official_date,
            "direction": self.direction,
            "source_url": self.source_url,
            "download_url": self.download_url,
            "download_available": self.download_available,
            "confidence": self.confidence.value,
            "oa_ordinal": self.oa_ordinal,
            "ds": self.ds,
            "wenjiandm": self.wenjiandm,
            "document_code": self.document_code,
            "document_version": self.document_version,
            "source_trace": self.source_trace,
            "event_key": self.event_key(),
            "fingerprint": self.fingerprint_value(),
        }

    def event_key(self) -> str:
        """Cross-provider identity for one official prosecution event."""
        basis = "|".join([
            (self.jurisdiction or "").strip().upper(),
            _event_application_number(self.jurisdiction, self.application_number),
            self.document_type.value,
            str(self.oa_ordinal),
            (self.official_date or "").strip(),
            _event_title_token(
                self.document_type, self.oa_ordinal, self.document_title
            ),
        ])
        return hashlib.sha256(basis.encode("utf-8")).hexdigest()

    def fingerprint_value(self) -> str:
        return fingerprint(self.jurisdiction, self.application_number,
                           self.document_title, self.official_date,
                           self.remote_document_id)


def pick_latest_office_action(documents):
    """Latest TRUE Office Action: OFFICE_ACTION_* only, max official_date.

    Documents without a parseable date never win. When the max date ties,
    the higher ordinal wins (e.g. the OA over a search report mailed the
    same day).
    """
    oas = [d for d in documents if d.document_type.is_office_action and d.official_date]
    if not oas:
        return None
    return max(oas, key=lambda d: (d.official_date, d.oa_ordinal))
