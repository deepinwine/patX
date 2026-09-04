// Data models for the USPTO prosecution module. Field names follow the
// USPTO Open Data Portal (Patent File Wrapper) JSON responses; see
// uspto_client.cpp for the mapping.
#pragma once

#include <string>
#include <vector>

namespace patx {

// ---------------------------------------------------------------------------
// USPTO Open Data Portal connection settings. The API key never goes into
// source control: it is read from the environment (USPTO_API_KEY) or the
// git-ignored local config file / DB config table.
// ---------------------------------------------------------------------------
struct UsptoConfig {
    std::string api_base_url = "https://api.uspto.gov";
    std::string api_key;                 // never logged, never committed
    long timeout_seconds = 30;
    int max_retries = 3;
    // auto sync interval: "manual", "12h", "24h"
    std::string auto_sync_interval = "manual";
    bool auto_download_oa = true;        // auto-download OA PDFs during sync
    bool auto_download_amendments = false;
    std::string document_folder = "data/documents/uspto";  // relative to db

    bool HasApiKey() const { return !api_key.empty(); }
    // Resolution order: explicit value (settings dialog) > env var USPTO_API_KEY.
    static std::string ResolveApiKey(const std::string& configured);
};

struct UsptoCase {
    int id = 0;                       // uspto_cases.id
    int foreign_patent_id = 0;        // link into foreign_patents (0 = unlinked)
    std::string internal_case_no;
    std::string application_number;   // e.g. 17248024
    std::string publication_number;
    std::string patent_number;
    std::string title;
    std::string filing_date;
    std::string publication_date;
    std::string grant_date;
    std::string priority_date;        // effective filing date
    std::string application_type;     // UTL / DES / PLT ...
    std::string application_status;
    std::string status_date;
    std::string first_named_inventor;
    std::string applicant_name;
    std::string assignee_name;
    std::string examiner_name;
    std::string art_unit;             // groupArtUnitNumber
    std::string technology_center;    // derived: first two digits of art unit
    std::string attorney_docket_number;
    std::string entity_status;
    std::string parent_application_number;
    std::string continuity_json;      // raw continuity payload
    std::string last_uspto_modified_at;
    std::string last_synced_at;
    std::string sync_status;          // idle/fetching/completed/partial/failed
    std::string sync_error;
    int new_document_count = 0;       // unread documents found by last sync
};

enum class UsptoDocumentCategory {
    Unknown = 0,
    // Examiner / USPTO issued
    NonFinalOfficeAction,
    FinalOfficeAction,
    RestrictionRequirement,
    AdvisoryAction,
    NoticeOfAllowance,
    ExaminersAmendment,
    InterviewSummary,
    NoticeOfAbandonment,
    NoticeToFileMissingParts,
    NoticeOfNonCompliantAmendment,
    IssueNotification,
    AppealRelated,
    OtherExaminer,
    // Applicant filed
    Amendment,
    ResponseToOfficeAction,
    PreliminaryAmendment,
    AfterFinalAmendment,
    ClaimsAmendment,
    SpecificationAmendment,
    DrawingAmendment,
    IDS,
    RCE,
    NoticeOfAppeal,
    AppealBrief,
    PreAppealBriefRequest,
    ReplyBrief,
    Petition,
    TerminalDisclaimer,
    OtherApplicant,
    // Administrative
    FilingReceipt,
    AcknowledgmentReceipt,
    AssignmentRecord,
    FeeDocument,
    OtherAdministrative,
};

enum class FilingParty { Unknown = 0, Examiner, Applicant, Administrative };

const char* ToString(UsptoDocumentCategory c);
const char* ToString(FilingParty p);
// "examiner" / "applicant" / "administrative" / "unknown"
const char* FilingPartyKey(FilingParty p);

struct UsptoDocument {
    int id = 0;                          // uspto_documents.id
    int uspto_case_id = 0;
    std::string document_identifier;     // ODP documentIdentifier (dedup key)
    std::string document_code;           // e.g. CTNF, CTFR,_acl
    std::string document_category;       // canonical name (from classifier)
    std::string document_description;    // ODP documentCodeDescriptionText
    std::string document_title;
    std::string filing_date;             // officialDate
    std::string mail_date;
    long long document_size = 0;
    std::string mime_type;
    std::string file_download_uri;
    std::string xml_download_uri;        // xmlarchive option when present
    std::string local_file_path;         // relative to document folder
    std::string local_text_path;
    std::string ocr_text;
    std::string extracted_text;
    std::string text_source;             // official_xml/official_ocr/docx/pdf_text/pdftotext/ocr
    std::string sha256;
    std::string source_last_modified_at;
    // not_downloaded / downloaded / download_failed
    std::string download_status;
    // pending / parsed / failed / needs_review
    std::string parse_status;
    double parse_confidence = 0.0;
    std::string raw_document_code;       // preserved even when classifier changes
    std::string raw_description;
    FilingParty party = FilingParty::Unknown;
    UsptoDocumentCategory category = UsptoDocumentCategory::Unknown;
    int related_document_id = 0;         // OA this response answers, etc.
    int page_count = 0;
};

// One version of the claim set (initial filing, each amendment, current).
struct ClaimVersion {
    int id = 0;
    int uspto_case_id = 0;
    int source_document_id = 0;      // amendment document that produced it
    int version_no = 0;
    std::string effective_date;
    std::string version_type;        // initial/preliminary/after_nonfinal/after_final/rce/examiner/current
    std::string source_type;         // official_xml/extracted_text/...
    bool is_initial = false;
    bool is_current = false;
    double parse_confidence = 0.0;
};

// Claim statuses per 37 CFR 1.121 status identifiers.
enum class ClaimStatus {
    Unknown = 0,
    Original,
    CurrentlyAmended,
    PreviouslyPresented,
    New,
    Canceled,
    Withdrawn,
    NotEntered,
    Allowed,
};

const char* ToString(ClaimStatus s);

struct Claim {
    int id = 0;
    int claim_version_id = 0;
    int claim_number = 0;
    ClaimStatus status = ClaimStatus::Unknown;
    std::string claim_text_clean;     // without the status identifier bracket
    std::string claim_text_marked;    // original with [..] markers if present
    bool is_independent = false;
    std::vector<int> parent_claim_numbers;
    double parse_confidence = 0.0;
};

// Structured rejection extracted from an Office Action.
struct OaRejection {
    int id = 0;
    int document_id = 0;              // uspto_documents.id
    std::string statute;              // "35 U.S.C. 103"
    std::string rejection_type;       // anticipation / obviousness / subject matter / written description / ...
    std::string claim_numbers;        // "1, 3-5" (raw text)
    std::string reference_text;
    std::string primary_reference;
    std::string secondary_references;
    std::string section_text;
    double parse_confidence = 0.0;
};

// One row in the prosecution timeline.
struct TimelineEvent {
    std::string date;                 // YYYY-MM-DD
    std::string event_category;       // canonical category name
    std::string document_description;
    FilingParty party = FilingParty::Unknown;
    std::string oa_type;
    std::string claims_affected;      // e.g. "1, 3-5"
    std::string rejection_summary;    // statutes joined, when parsed
    int document_id = 0;              // uspto_documents.id (0 for pure events)
    bool is_new = false;              // found by the latest sync
};

// Result of a case sync run.
struct SyncResult {
    bool ok = false;
    bool api_key_missing = false;
    bool case_not_found = false;
    int documents_total = 0;
    int documents_new = 0;
    int documents_updated = 0;
    int documents_downloaded = 0;
    int claim_versions_built = 0;
    int oa_records_linked = 0;
    std::string error;
    long http_status = 0;            // 0 = no HTTP response (config/network issue)
    std::string http_status_line;    // e.g. "HTTP 429 Too Many Requests"
};

} // namespace patx
