"""Classification of Chinese prosecution document titles.

Rules tolerate spaces, punctuation noise and full-width digits, and are kept
in this module (never in UI code) so page re-designs only touch one place.
"""
from __future__ import annotations

import re
from typing import Tuple

from .models import Confidence, DocumentType

_FULLWIDTH = {ord(f): ord(t) for f, t in zip("０１２３４５６７８９（）《》　", "0123456789()<> ")}

_ZH_NUM = {"一": 1, "1": 1, "二": 2, "两": 2, "2": 2, "三": 3, "3": 3, "四": 4, "4": 4,
           "五": 5, "5": 5, "六": 6, "6": 6, "七": 7, "7": 7, "八": 8, "8": 8,
           "九": 9, "9": 9, "十": 10}

_OA_ORDINAL_RE = re.compile(r"第\s*([0-9０-９一二三四五六七八九十两]+)\s*次")


def _ordinal_of(title: str) -> int:
    m = _OA_ORDINAL_RE.search(title)
    if not m:
        for abbr, v in (("一通", 1), ("二通", 2), ("三通", 3), ("四通", 4)):
            if abbr in title:
                return v
        return 0
    token = m.group(1)
    if token in _ZH_NUM:
        return _ZH_NUM[token]
    if token.isdigit():
        n = int(token)
        return n if 0 < n < 100 else 0
    return 0


def classify_cn_title(raw_title: str) -> Tuple[DocumentType, int, Confidence]:
    """Returns (document_type, oa_ordinal, confidence)."""
    if not raw_title:
        return DocumentType.UNKNOWN, 0, Confidence.LOW
    # strip all whitespace so layout splits like "第二次 审查意见 通知书" match
    title = re.sub(r"\s+", "", raw_title.translate(_FULLWIDTH))

    if "审查意见通知书" in title:
        ordinal = _ordinal_of(title)
        if ordinal == 1:
            return DocumentType.OFFICE_ACTION_FIRST, 1, Confidence.HIGH
        if ordinal == 2:
            return DocumentType.OFFICE_ACTION_SECOND, 2, Confidence.HIGH
        if ordinal >= 3:
            return DocumentType.OFFICE_ACTION_NTH, ordinal, Confidence.HIGH
        return DocumentType.OFFICE_ACTION_UNKNOWN, 0, Confidence.MEDIUM

    # internal abbreviations: 一通/二通/三通/四通
    for abbr, v in (("一通", 1), ("二通", 2), ("三通", 3), ("四通", 4)):
        if abbr in title:
            t = (DocumentType.OFFICE_ACTION_FIRST if v == 1 else
                 DocumentType.OFFICE_ACTION_SECOND if v == 2 else
                 DocumentType.OFFICE_ACTION_NTH)
            return t, v, Confidence.MEDIUM

    if "驳回决定" in title:
        return DocumentType.REJECTION_DECISION, 0, Confidence.HIGH
    if re.search(r"授予.*专利权|授权.*(?:通知|决定|公告)|办理登记.*通知", title):
        return DocumentType.GRANT_NOTICE, 0, Confidence.HIGH
    if "补正" in title and "通知" in title:
        return DocumentType.CORRECTION_NOTICE, 0, Confidence.HIGH
    if "检索报告" in title:
        return DocumentType.SEARCH_REPORT, 0, Confidence.HIGH
    if "意见陈述" in title:
        return DocumentType.RESPONSE_TO_OFFICE_ACTION, 0, Confidence.HIGH
    if "权利要求" in title and re.search(r"修改|替换|全文", title):
        return DocumentType.CLAIMS_AMENDMENT, 0, Confidence.HIGH
    if "说明书" in title and re.search(r"修改|替换", title):
        return DocumentType.SPECIFICATION_AMENDMENT, 0, Confidence.HIGH
    if re.search(r"缴费|费用|手续|补正书|撤回|延长期限|恢复", title):
        return DocumentType.OTHER_OFFICIAL, 0, Confidence.MEDIUM
    if "申请" in title or "陈述" in title:
        return DocumentType.OTHER_APPLICANT, 0, Confidence.MEDIUM
    return DocumentType.UNKNOWN, 0, Confidence.LOW


def oa_title_cn(document_type: DocumentType, ordinal: int) -> str:
    """Canonical Chinese OA title for storing into OARecord.oa_type."""
    zh = {1: "一", 2: "二", 3: "三", 4: "四", 5: "五", 6: "六", 7: "七", 8: "八", 9: "九"}
    if document_type == DocumentType.OFFICE_ACTION_FIRST or ordinal == 1:
        return "第一次审查意见通知书"
    if document_type == DocumentType.OFFICE_ACTION_SECOND or ordinal == 2:
        return "第二次审查意见通知书"
    if document_type.is_office_action and ordinal >= 3:
        num = zh.get(ordinal, str(ordinal))
        return f"第{num}次审查意见通知书"
    if document_type.is_office_action:
        return "审查意见通知书"
    return ""


def direction_for(document_type: DocumentType) -> str:
    if document_type in (DocumentType.RESPONSE_TO_OFFICE_ACTION,
                         DocumentType.CLAIMS_AMENDMENT,
                         DocumentType.SPECIFICATION_AMENDMENT,
                         DocumentType.OTHER_APPLICANT):
        return "applicant"
    return "official"


# --------------------------------------------------------------------------
# Global Dossier (USPTO/EPO) English titles
# --------------------------------------------------------------------------

_CODE_TYPES = {
    "210401-CN": (DocumentType.OFFICE_ACTION_FIRST, 1),
}

_EN_FINAL_RE = re.compile(r"final rejection|decision to reject")
_EN_APPLICANT_RE = re.compile(
    r"response to|remarks|observations|amendment|amended claims|substitution of"
    r"|power of attorney|assignment|withdrawal|request for")
_EN_GRANT_RE = re.compile(r"grant patent right|notification to grant|registration formalities")
_EN_CORRECTION_RE = re.compile(r"rectification|correction notice")
_EN_OA_RE = re.compile(r"examination opinions|office action")
_EN_ORDINALS = {"first": 1, "second": 2, "third": 3, "fourth": 4, "fifth": 5,
                "sixth": 6, "seventh": 7, "eighth": 8, "ninth": 9, "tenth": 10}


def _classify_en(raw_title: str):
    """English title -> (document_type, ordinal) or None when not official."""
    compact = re.sub(r"\s+", " ", (raw_title or "").strip()).lower()
    if not compact:
        return None
    if _EN_APPLICANT_RE.search(compact):
        return DocumentType.RESPONSE_TO_OFFICE_ACTION, 0
    if _EN_FINAL_RE.search(compact):
        return DocumentType.REJECTION_DECISION, 0
    for word, ordinal in _EN_ORDINALS.items():
        if re.search(rf"\b{word}\b.*{_EN_OA_RE.pattern}", compact):
            doc_type = (DocumentType.OFFICE_ACTION_FIRST if ordinal == 1 else
                        DocumentType.OFFICE_ACTION_SECOND if ordinal == 2 else
                        DocumentType.OFFICE_ACTION_NTH)
            return doc_type, ordinal
    if _EN_GRANT_RE.search(compact):
        return DocumentType.GRANT_NOTICE, 0
    if _EN_CORRECTION_RE.search(compact):
        return DocumentType.CORRECTION_NOTICE, 0
    return None


def classify_official_document(raw_title: str, document_code: str = "") -> Tuple[DocumentType, int, Confidence]:
    """Classify from an English Global Dossier title and/or a stable document
    code, falling back to the Chinese rules for CNIPA titles.

    When both inputs are present and disagree the answer is UNKNOWN/LOW so the
    caller raises MANUAL_REVIEW_REQUIRED instead of guessing.
    """
    code = (document_code or "").strip().upper()
    code_result = _CODE_TYPES.get(code)
    title_result = _classify_en(raw_title)

    if code_result is None:
        if title_result is not None:
            return title_result[0], title_result[1], Confidence.HIGH
        return classify_cn_title(raw_title)
    if title_result is None or title_result == code_result:
        return code_result[0], code_result[1], Confidence.HIGH
    return DocumentType.UNKNOWN, 0, Confidence.LOW


def event_title_cn(document_type: DocumentType, ordinal: int,
                   fallback_title: str = "") -> str:
    """Canonical Chinese display name for any official event."""
    if document_type.is_office_action:
        cn = oa_title_cn(document_type, ordinal)
        if cn:
            return cn
    return {
        DocumentType.REJECTION_DECISION: "驳回决定",
        DocumentType.GRANT_NOTICE: "授权通知",
        DocumentType.CORRECTION_NOTICE: "补正通知",
    }.get(document_type, fallback_title or "其他官方通知")
