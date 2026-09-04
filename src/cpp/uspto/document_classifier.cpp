#include "patx/document_classifier.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace patx {

const char* ToString(UsptoDocumentCategory c) {
    switch (c) {
        case UsptoDocumentCategory::NonFinalOfficeAction: return "Non-Final Office Action";
        case UsptoDocumentCategory::FinalOfficeAction: return "Final Office Action";
        case UsptoDocumentCategory::RestrictionRequirement: return "Restriction Requirement";
        case UsptoDocumentCategory::AdvisoryAction: return "Advisory Action";
        case UsptoDocumentCategory::NoticeOfAllowance: return "Notice of Allowance";
        case UsptoDocumentCategory::ExaminersAmendment: return "Examiner's Amendment";
        case UsptoDocumentCategory::InterviewSummary: return "Interview Summary";
        case UsptoDocumentCategory::NoticeOfAbandonment: return "Notice of Abandonment";
        case UsptoDocumentCategory::NoticeToFileMissingParts: return "Notice to File Missing Parts";
        case UsptoDocumentCategory::NoticeOfNonCompliantAmendment: return "Notice of Non-Compliant Amendment";
        case UsptoDocumentCategory::IssueNotification: return "Issue Notification";
        case UsptoDocumentCategory::AppealRelated: return "Appeal Related";
        case UsptoDocumentCategory::OtherExaminer: return "Other Examiner Document";
        case UsptoDocumentCategory::Amendment: return "Amendment";
        case UsptoDocumentCategory::ResponseToOfficeAction: return "Response to Office Action";
        case UsptoDocumentCategory::PreliminaryAmendment: return "Preliminary Amendment";
        case UsptoDocumentCategory::AfterFinalAmendment: return "After Final Amendment";
        case UsptoDocumentCategory::ClaimsAmendment: return "Claims Amendment";
        case UsptoDocumentCategory::SpecificationAmendment: return "Specification Amendment";
        case UsptoDocumentCategory::DrawingAmendment: return "Drawing Amendment";
        case UsptoDocumentCategory::IDS: return "Information Disclosure Statement";
        case UsptoDocumentCategory::RCE: return "Request for Continued Examination";
        case UsptoDocumentCategory::NoticeOfAppeal: return "Notice of Appeal";
        case UsptoDocumentCategory::AppealBrief: return "Appeal Brief";
        case UsptoDocumentCategory::PreAppealBriefRequest: return "Pre-Appeal Brief Request";
        case UsptoDocumentCategory::ReplyBrief: return "Reply Brief";
        case UsptoDocumentCategory::Petition: return "Petition";
        case UsptoDocumentCategory::TerminalDisclaimer: return "Terminal Disclaimer";
        case UsptoDocumentCategory::OtherApplicant: return "Other Applicant Document";
        case UsptoDocumentCategory::FilingReceipt: return "Filing Receipt";
        case UsptoDocumentCategory::AcknowledgmentReceipt: return "Acknowledgment Receipt";
        case UsptoDocumentCategory::AssignmentRecord: return "Assignment Record";
        case UsptoDocumentCategory::FeeDocument: return "Fee Document";
        case UsptoDocumentCategory::OtherAdministrative: return "Other Administrative";
        case UsptoDocumentCategory::Unknown: return "Unclassified";
    }
    return "Unclassified";
}

const char* ToString(FilingParty p) {
    switch (p) {
        case FilingParty::Examiner: return "Examiner";
        case FilingParty::Applicant: return "Applicant";
        case FilingParty::Administrative: return "Administrative";
        case FilingParty::Unknown: return "Unknown";
    }
    return "Unknown";
}

const char* FilingPartyKey(FilingParty p) {
    switch (p) {
        case FilingParty::Examiner: return "examiner";
        case FilingParty::Applicant: return "applicant";
        case FilingParty::Administrative: return "administrative";
        case FilingParty::Unknown: return "unknown";
    }
    return "unknown";
}

const char* ToString(ClaimStatus s) {
    switch (s) {
        case ClaimStatus::Original: return "original";
        case ClaimStatus::CurrentlyAmended: return "currently amended";
        case ClaimStatus::PreviouslyPresented: return "previously presented";
        case ClaimStatus::New: return "new";
        case ClaimStatus::Canceled: return "canceled";
        case ClaimStatus::Withdrawn: return "withdrawn";
        case ClaimStatus::NotEntered: return "not entered";
        case ClaimStatus::Allowed: return "allowed";
        case ClaimStatus::Unknown: return "unknown";
    }
    return "unknown";
}

namespace {

std::string Upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string TrimDots(const std::string& code) {
    // ODP codes arrive in variants like "CTNF.", "_ACL.", "CTFR"
    std::string out;
    for (char c : code) {
        if (c == '.' || c == ' ' || c == '_') continue;
        out += static_cast<char>(std::toupper(c));
    }
    return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return Upper(haystack).find(Upper(needle)) != std::string::npos;
}

} // namespace

DocumentClassifier::Result DocumentClassifier::Classify(const std::string& document_code,
                                                        const std::string& description,
                                                        const std::string& title) {
    Result result;
    const std::string code = TrimDots(document_code);
    const std::string text = description + " " + title;

    // --- Office actions and examiner communications (official codes) ---
    if (code == "CTNF" || Contains(text, "NON-FINAL REJECTION") ||
        Contains(text, "NON FINAL OFFICE ACTION") ||
        Contains(text, "NONFINAL OFFICE ACTION") ||
        Contains(text, "NON-FINAL OFFICE ACTION")) {
        return {UsptoDocumentCategory::NonFinalOfficeAction, FilingParty::Examiner, 0.95};
    }
    // "Mail Non-Final Office Action" contains the substring "Final Office
    // Action" - exclude the non-final forms explicitly.
    if (code == "CTFR" || (Contains(text, "FINAL OFFICE ACTION") &&
                           !Contains(text, "NON-FINAL") && !Contains(text, "NONFINAL") &&
                           !Contains(text, "NON FINAL"))) {
        return {UsptoDocumentCategory::FinalOfficeAction, FilingParty::Examiner, 0.95};
    }
    if (code == "CTC" || Contains(text, "RESTRICTION REQUIREMENT") ||
        Contains(text, "ELECTION")) {
        return {UsptoDocumentCategory::RestrictionRequirement, FilingParty::Examiner, 0.85};
    }
    if (code == "CTAV" || code == "ADVA" || Contains(text, "ADVISORY ACTION")) {
        return {UsptoDocumentCategory::AdvisoryAction, FilingParty::Examiner, 0.9};
    }
    if (code == "NOAL" || code == "NOAM" || Contains(text, "NOTICE OF ALLOWANCE") ||
        Contains(text, "NOTICE OF ALLOWABILITY")) {
        return {UsptoDocumentCategory::NoticeOfAllowance, FilingParty::Examiner, 0.95};
    }
    if (code == "EXAM" || code == "CTAE" || Contains(text, "EXAMINER'S AMENDMENT") ||
        Contains(text, "EXAMINER AMENDMENT")) {
        return {UsptoDocumentCategory::ExaminersAmendment, FilingParty::Examiner, 0.9};
    }
    if (code == "INTVSUM" || code == "INTSUM" || Contains(text, "INTERVIEW SUMMARY") ||
        Contains(text, "INTERVIEW RECORD")) {
        return {UsptoDocumentCategory::InterviewSummary, FilingParty::Examiner, 0.85};
    }
    if (code == "NOAB" || Contains(text, "NOTICE OF ABANDONMENT")) {
        return {UsptoDocumentCategory::NoticeOfAbandonment, FilingParty::Examiner, 0.9};
    }
    if (code == "MPGE" || Contains(text, "MISSING PARTS")) {
        return {UsptoDocumentCategory::NoticeToFileMissingParts, FilingParty::Examiner, 0.9};
    }
    if (Contains(text, "NON-COMPLIANT AMENDMENT") || Contains(text, "NONCOMPLIANT AMENDMENT")) {
        return {UsptoDocumentCategory::NoticeOfNonCompliantAmendment, FilingParty::Examiner, 0.85};
    }
    if (code == "ISSNOT" || code == "PTOL" || Contains(text, "ISSUE NOTIFICATION") ||
        Contains(text, "NOTIFICATION OF ISSUANCE")) {
        return {UsptoDocumentCategory::IssueNotification, FilingParty::Examiner, 0.9};
    }
    // Notice of appeal / appeal brief family (either side may file)
    if (code == "NOA" || Contains(text, "NOTICE OF APPEAL")) {
        return {UsptoDocumentCategory::NoticeOfAppeal, FilingParty::Applicant, 0.85};
    }
    if (code == "APBR" || Contains(text, "APPEAL BRIEF") || Contains(text, "APPELLANT BRIEF")) {
        if (Contains(text, "REPLY")) {
            return {UsptoDocumentCategory::ReplyBrief, FilingParty::Applicant, 0.85};
        }
        return {UsptoDocumentCategory::AppealBrief, FilingParty::Applicant, 0.85};
    }
    if (Contains(text, "PRE-APPEAL BRIEF") || Contains(text, "PREAPPEAL")) {
        return {UsptoDocumentCategory::PreAppealBriefRequest, FilingParty::Applicant, 0.85};
    }
    if (code == "REBR" || Contains(text, "REPLY BRIEF")) {
        return {UsptoDocumentCategory::ReplyBrief, FilingParty::Applicant, 0.85};
    }
    if (code == "EXAAB" || Contains(text, "EXAMINER'S ANSWER") || Contains(text, "APPEAL DISMISSED")) {
        return {UsptoDocumentCategory::AppealRelated, FilingParty::Examiner, 0.8};
    }

    // --- Applicant documents ---
    if (code == "IDS" || code == "SIDS" || Contains(text, "INFORMATION DISCLOSURE")) {
        return {UsptoDocumentCategory::IDS, FilingParty::Applicant, 0.95};
    }
    if (code == "RCE" || Contains(text, "CONTINUED EXAMINATION")) {
        return {UsptoDocumentCategory::RCE, FilingParty::Applicant, 0.95};
    }
    if (code == "TD" || Contains(text, "TERMINAL DISCLAIMER")) {
        return {UsptoDocumentCategory::TerminalDisclaimer, FilingParty::Applicant, 0.9};
    }
    if (code == "PET" || code == "PETDEC" || code == "PETOP" ||
        (Contains(text, "PETITION") && !Contains(text, "DECISION"))) {
        return {UsptoDocumentCategory::Petition, FilingParty::Applicant, 0.8};
    }
    if (code == "PREV" || Contains(text, "PRELIMINARY AMENDMENT")) {
        return {UsptoDocumentCategory::PreliminaryAmendment, FilingParty::Applicant, 0.9};
    }
    if (code == "AFA" || Contains(text, "AFTER-FINAL") || Contains(text, "AFTER FINAL")) {
        return {UsptoDocumentCategory::AfterFinalAmendment, FilingParty::Applicant, 0.9};
    }
    // Amendment / response family. ODP codes: _ACL, ACL, AMDT, ADDR, EF,
    // and "Amendment" descriptions vary - match by keyword too.
    if (code == "ACL" || code == "AMDT" || code == "AMND" || Contains(text, "AMENDMENT")) {
        if (Contains(text, "SPECIFICATION")) {
            return {UsptoDocumentCategory::SpecificationAmendment, FilingParty::Applicant, 0.8};
        }
        if (Contains(text, "DRAWING")) {
            return {UsptoDocumentCategory::DrawingAmendment, FilingParty::Applicant, 0.8};
        }
        if (Contains(text, "CLAIMS") || Contains(text, "CLAIM")) {
            return {UsptoDocumentCategory::ClaimsAmendment, FilingParty::Applicant, 0.8};
        }
        return {UsptoDocumentCategory::Amendment, FilingParty::Applicant, 0.85};
    }
    if (code == "ADDR" || code == "AOA" || Contains(text, "RESPONSE TO OFFICE ACTION") ||
        Contains(text, "RESPONSE TO NON-FINAL") || Contains(text, "RESPONSE TO FINAL") ||
        Contains(text, "ARGUMENT")) {
        return {UsptoDocumentCategory::ResponseToOfficeAction, FilingParty::Applicant, 0.85};
    }
    if (code == "EF" || Contains(text, "ELECTRONIC FILING") || Contains(text, "FILING RECEIPT")) {
        return {UsptoDocumentCategory::FilingReceipt, FilingParty::Administrative, 0.8};
    }
    if (Contains(text, "ACKNOWLEDGMENT RECEIPT")) {
        return {UsptoDocumentCategory::AcknowledgmentReceipt, FilingParty::Administrative, 0.85};
    }
    if (Contains(text, "ASSIGNMENT") || Contains(text, "RECORDATION")) {
        return {UsptoDocumentCategory::AssignmentRecord, FilingParty::Administrative, 0.8};
    }
    if (code == "WFEE" || Contains(text, "FEE WORKSHEET") || Contains(text, "PAYMENT")) {
        return {UsptoDocumentCategory::FeeDocument, FilingParty::Administrative, 0.8};
    }
    if (code == "N417" || Contains(text, "ACKNOWLEDGMENT")) {
        return {UsptoDocumentCategory::AcknowledgmentReceipt, FilingParty::Administrative, 0.8};
    }

    // --- Fallbacks by textual hints ---
    if (Contains(text, "OFFICE ACTION")) {
        return {UsptoDocumentCategory::NonFinalOfficeAction, FilingParty::Examiner, 0.6};
    }
    if (Contains(text, "MAIL") || Contains(text, "NOTICE") || Contains(text, "DECISION")) {
        return {UsptoDocumentCategory::OtherExaminer, FilingParty::Examiner, 0.5};
    }
    if (Contains(text, "RESPONSE") || Contains(text, "REMARKS") || Contains(text, "STATEMENT")) {
        return {UsptoDocumentCategory::OtherApplicant, FilingParty::Applicant, 0.5};
    }

    return {UsptoDocumentCategory::Unknown, FilingParty::Unknown, 0.2};
}

} // namespace patx
