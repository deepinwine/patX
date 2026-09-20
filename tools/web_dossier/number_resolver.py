"""CN patent number normalization.

A publication number (公开号, e.g. CN119870049A, 9 digits + kind code) is NOT
an application number (申请号, e.g. 202410123456.7, 12 digits + check digit).
This module classifies input robustly - it never strips letters and calls
the result an application number.
"""
from __future__ import annotations

import re
from dataclasses import dataclass

_FULLWIDTH = {ord(f): ord(t) for f, t in zip("０１２３４５６７８９．　", "0123456789. ")}

# CN application: 12 digits + optional check digit after a dot (2006+ style)
_APP_RE = re.compile(r"^(?:CN)?(\d{12})(?:\.(\d))?$", re.IGNORECASE)
# CN publication: 9 digits + optional kind code letter(s), e.g. 119870049A
_PUB_RE = re.compile(r"^(?:CN)?(\d{9})\s*([AB]1?|[ABU]?)$", re.IGNORECASE)


@dataclass
class CNIdentifier:
    raw: str
    country_code: str = "CN"
    number: str = ""            # digits (application) or digits (publication)
    kind_code: str = ""
    number_type: str = ""       # "application" | "publication"
    normalized_number: str = ""  # CN+digits(+kind) / CN+digits(.check)

    @property
    def valid(self) -> bool:
        return bool(self.number_type)


def normalize_cn_identifier(raw: str) -> CNIdentifier:
    ident = CNIdentifier(raw=raw or "")
    if not raw:
        return ident
    s = raw.translate(_FULLWIDTH)
    s = re.sub(r"\s+", "", s).upper().replace("，", "")
    if not s.startswith("CN"):
        m_app = _APP_RE.match(s)
        if m_app:
            ident.number = m_app.group(1)
            ident.number_type = "application"
            ident.normalized_number = "CN" + m_app.group(1) + \
                ("." + m_app.group(2) if m_app.group(2) else "")
            return ident
    m_pub = _PUB_RE.match(s)
    if m_pub:
        ident.number = m_pub.group(1)
        ident.kind_code = m_pub.group(2) or ""
        ident.number_type = "publication"
        ident.normalized_number = "CN" + m_pub.group(1) + ident.kind_code
        return ident
    # CN-prefixed application
    m_app2 = _APP_RE.match(s)
    if m_app2:
        ident.number = m_app2.group(1)
        ident.number_type = "application"
        ident.normalized_number = "CN" + m_app2.group(1) + \
            ("." + m_app2.group(2) if m_app2.group(2) else "")
        return ident
    return ident


def application_number_of(raw: str) -> str:
    """Convenience: returns the normalized application number or ''."""
    ident = normalize_cn_identifier(raw)
    return ident.normalized_number if ident.number_type == "application" else ""
