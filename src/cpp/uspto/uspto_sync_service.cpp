#include "patx/uspto_sync_service.hpp"
#include "patx/claim_parser.hpp"
#include "patx/document_classifier.hpp"
#include "patx/log.hpp"
#include "patx/oa_parser.hpp"
#include "patx/text_extractor.hpp"
#include "patx/timeline_service.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace patx {

const char* ToString(SyncState s) {
    switch (s) {
        case SyncState::Idle: return "Idle";
        case SyncState::FetchingMetadata: return "Fetching metadata";
        case SyncState::FetchingDocuments: return "Fetching documents";
        case SyncState::Downloading: return "Downloading";
        case SyncState::Parsing: return "Parsing";
        case SyncState::Completed: return "Completed";
        case SyncState::PartialFailure: return "Partial failure";
        case SyncState::Failed: return "Failed";
    }
    return "Idle";
}

namespace {

// Sanitizes a string for use in a file name (task: no illegal characters,
// database stores relative paths).
std::string SanitizeFileName(const std::string& input) {
    std::string out;
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            out += c;
        } else if (c == ' ') {
            out += '_';
        }
        // everything else (\/:*?"<>| and C0 controls) is dropped
    }
    if (out.size() > 80) out = out.substr(0, 80);
    return out;
}

bool IsOaCategory(UsptoDocumentCategory c) {
    return c == UsptoDocumentCategory::NonFinalOfficeAction ||
           c == UsptoDocumentCategory::FinalOfficeAction ||
           c == UsptoDocumentCategory::RestrictionRequirement ||
           c == UsptoDocumentCategory::AdvisoryAction;
}

bool IsAmendmentCategory(UsptoDocumentCategory c) {
    return c == UsptoDocumentCategory::Amendment ||
           c == UsptoDocumentCategory::ClaimsAmendment ||
           c == UsptoDocumentCategory::ResponseToOfficeAction ||
           c == UsptoDocumentCategory::PreliminaryAmendment ||
           c == UsptoDocumentCategory::AfterFinalAmendment ||
           c == UsptoDocumentCategory::SpecificationAmendment ||
           c == UsptoDocumentCategory::DrawingAmendment;
}

// Documents whose text is expected to contain a claim listing.
bool CarriesClaims(UsptoDocumentCategory c) {
    return IsAmendmentCategory(c) || c == UsptoDocumentCategory::ExaminersAmendment ||
           c == UsptoDocumentCategory::NoticeOfAllowance;
}

std::string VersionTypeFor(UsptoDocumentCategory c) {
    switch (c) {
        case UsptoDocumentCategory::PreliminaryAmendment: return "preliminary";
        case UsptoDocumentCategory::AfterFinalAmendment: return "after_final";
        case UsptoDocumentCategory::ExaminersAmendment: return "examiner";
        case UsptoDocumentCategory::RCE: return "rce";
        case UsptoDocumentCategory::ResponseToOfficeAction:
        case UsptoDocumentCategory::Amendment:
        case UsptoDocumentCategory::ClaimsAmendment:
        case UsptoDocumentCategory::SpecificationAmendment:
        case UsptoDocumentCategory::DrawingAmendment:
            return "after_nonfinal";
        default: return "amendment";
    }
}

} // namespace

UsptoSyncService::UsptoSyncService(Database& db, const UsptoConfig& config)
    : db_(db), repo_(db.GetHandle()), client_(config), config_(config) {
    repo_.EnsureTables();
}

SyncResult UsptoSyncService::AddCase(const std::string& application_number,
                                     int foreign_patent_id, const ProgressFn& progress) {
    SyncResult result;
    if (progress && !progress(SyncState::FetchingMetadata, 0, "Resolving API key")) return result;

    std::string resolved_key = UsptoConfig::ResolveApiKey(config_.api_key);
    if (resolved_key.empty()) {
        result.api_key_missing = true;
        result.error = "USPTO API key is not configured. Open USPTO Settings to add one.";
        PATX_LOG_WARN("USPTO sync skipped: no API key");
        return result;
    }
    config_.api_key = resolved_key;
    client_.SetApiKey(resolved_key);

    // The user may enter an application number (17248024), a publication
    // number (US 2021/0210819 A1) or a patent number (11,646,472). Letters in
    // the input mean it definitely is not an application number, so resolve
    // it through the official search endpoint first; bare digits go direct
    // and fall back to search only on a 404.
    std::string normalized_pub = UsptoClient::NormalizePublicationNumber(application_number);
    bool has_letters = normalized_pub.find_first_not_of("0123456789") != std::string::npos;

    UsptoCase record;
    record.foreign_patent_id = foreign_patent_id;

    if (has_letters) {
        std::string resolved = ResolveApplicationNumber(application_number, result);
        if (resolved.empty()) return result;
        record.application_number = resolved;
        record.publication_number = normalized_pub;
        SyncResult sync = RunSync(record, progress);
        if (sync.ok) sync.resolved_application_number = resolved;
        return sync;
    }

    record.application_number = UsptoClient::NormalizeApplicationNumber(application_number);
    SyncResult sync = RunSync(record, progress);

    // Bare digits that 404'd may be a patent number - resolve and retry once
    if (!sync.ok && sync.case_not_found) {
        std::string resolved = ResolveApplicationNumber(application_number, sync);
        if (!resolved.empty()) {
            UsptoCase retry;
            retry.foreign_patent_id = foreign_patent_id;
            retry.application_number = resolved;
            sync = RunSync(retry, progress);
            if (sync.ok) sync.resolved_application_number = resolved;
        }
    }
    return sync;
}

std::string UsptoSyncService::ResolveApplicationNumber(const std::string& input,
                                                       SyncResult& out_result) {
    for (const auto& q : UsptoClient::BuildSearchQueries(input)) {
        std::vector<UsptoCase> hits;
        ApiResult api = client_.SearchApplications(q, hits);
        if (!api.ok) {
            if (api.http_status == 404) continue;   // this query found nothing, try next
            out_result.error = api.error;
            out_result.http_status = api.http_status;
            out_result.http_status_line = api.status_line;
            return "";
        }
        int idx = UsptoClient::PickSearchHit(input, hits);
        if (idx >= 0) {
            PATX_LOG_INFO("Resolved identifier '" + input + "' to application " +
                          hits[idx].application_number + " (query: " + q + ")");
            return hits[idx].application_number;
        }
        if (idx == -2) {
            out_result.error = "identifier '" + input + "' matches " +
                               std::to_string(hits.size()) +
                               " applications; enter the application number (e.g. 17248024) "
                               "to pick the exact case";
            return "";
        }
        // -1: no exact match among this query's hits - try the next query
    }
    out_result.error = "no US application found for '" + input +
                       "'. Check the number, or use the application number directly.";
    return "";
}

SyncResult UsptoSyncService::SyncCase(int uspto_case_id, const ProgressFn& progress) {
    SyncResult result;
    UsptoCase record = repo_.GetCaseById(uspto_case_id);
    if (record.id == 0) {
        result.error = "case not found";
        return result;
    }
    return RunSync(record, progress);
}

std::vector<SyncResult> UsptoSyncService::SyncAllCases(const ProgressFn& progress) {
    std::vector<SyncResult> results;
    auto cases = repo_.GetAllCases();
    int index = 0;
    for (const auto& c : cases) {
        int percent = static_cast<int>(index * 100 / std::max<size_t>(cases.size(), 1));
        if (progress && !progress(SyncState::Idle, percent, "Case " + c.application_number)) break;
        results.push_back(SyncCase(c.id, progress));
        index++;
    }
    return results;
}

SyncResult UsptoSyncService::RunSync(UsptoCase& record, const ProgressFn& progress) {
    SyncResult result;
    auto report = [&](SyncState state, int pct, const std::string& msg) -> bool {
        return !progress || progress(state, pct, msg);
    };

    repo_.UpdateCaseSyncState(record.id ? record.id : 0, "fetching", "", 0);

    // ---- 1. Application metadata ----
    if (!report(SyncState::FetchingMetadata, 10, "Fetching application data")) {
        result.error = "cancelled";
        return result;
    }
    ApiResult api = client_.FetchApplication(record.application_number, record);
    if (!api.ok) {
        result.error = api.error;
        result.http_status = api.http_status;
        result.http_status_line = api.status_line;
        result.case_not_found = api.http_status == 404;
        repo_.UpdateCaseSyncState(record.id, "failed", api.error, 0);
        repo_.LogSyncEvent(record.id, "sync_failed", api.error, true);
        PATX_LOG_WARN("USPTO sync failed for " + record.application_number + ": " + api.error);
        return result;
    }

    record.sync_status = "completed";
    int case_id = repo_.UpsertCase(record);
    if (case_id <= 0) {
        result.error = "failed to store case";
        return result;
    }
    record.id = case_id;

    // ---- 2. Documents (incremental) ----
    if (!report(SyncState::FetchingDocuments, 40, "Fetching document list")) {
        result.error = "cancelled";
        return result;
    }
    std::vector<UsptoDocument> docs;
    api = client_.FetchDocuments(record.application_number, docs);
    if (!api.ok) {
        repo_.UpdateCaseSyncState(case_id, "partial", api.error, 0);
        repo_.LogSyncEvent(case_id, "documents_failed", api.error, true);
        result.ok = false;
        result.error = api.error;
        result.http_status = api.http_status;
        result.http_status_line = api.status_line;
        return result;
    }
    result.documents_total = static_cast<int>(docs.size());

    int downloaded = 0;
    for (auto& doc : docs) {
        doc.uspto_case_id = case_id;
        bool created = false;
        repo_.UpsertDocument(doc, &created);
        if (created) result.documents_new++;

        // Lazy download policy: only OA/amendment categories when enabled
        bool want_download = !doc.file_download_uri.empty() &&
            ((config_.auto_download_oa && IsOaCategory(doc.category)) ||
             (config_.auto_download_amendments && IsAmendmentCategory(doc.category)));
        if (want_download && doc.download_status != "downloaded") {
            if (!report(SyncState::Downloading, 60, "Downloading " + doc.document_description)) {
                result.error = "cancelled";
                return result;
            }
            std::string name = SanitizeFileName(doc.filing_date + "_" + doc.document_code + "_" +
                                                doc.document_identifier) + ".pdf";
            std::string dest = (std::filesystem::path(config_.document_folder) /
                                record.application_number / name).string();
            std::string sha;
            long long size = 0;
            ApiResult dl = client_.DownloadDocument(doc.file_download_uri, dest, sha, &size);
            if (dl.ok) {
                doc.local_file_path = (std::filesystem::path(record.application_number) / name).string();
                doc.sha256 = sha;
                doc.document_size = size;
                doc.download_status = "downloaded";
                downloaded++;
                repo_.UpdateDocumentDownload(doc);

                // Extract text right away so parsing below has material
                auto text = DocumentTextExtractor::Extract(dest, doc.mime_type);
                if (text.success) {
                    doc.extracted_text = text.text;
                    doc.text_source = text.source;
                    doc.parse_confidence = text.confidence;
                    repo_.UpdateDocumentParse(doc);
                }
            } else {
                doc.download_status = "download_failed";
                repo_.UpdateDocumentDownload(doc);
                repo_.LogSyncEvent(case_id, "download_failed",
                                   doc.document_description + ": " + dl.error, true);
            }
        }
    }
    result.documents_downloaded = downloaded;

    // ---- 3. Parse what we have (OA rejections, claim listings) ----
    if (!report(SyncState::Parsing, 80, "Parsing documents")) {
        result.error = "cancelled";
        return result;
    }
    auto stored = repo_.GetDocumentsForCaseOrdered(case_id, true);
    for (auto& doc : stored) {
        if (doc.extracted_text.empty()) continue;

        if (IsOaCategory(doc.category) && doc.parse_status != "parsed") {
            auto parsed = OaParser::Parse(doc.extracted_text);
            if (parsed.is_office_action) {
                repo_.DeleteRejectionsForDocument(doc.id);
                for (const auto& rejection : parsed.rejections) {
                    OaRejection r = rejection;
                    r.document_id = doc.id;
                    repo_.InsertRejection(r);
                }
                doc.parse_status = parsed.confidence >= 0.75 ? "parsed" : "needs_review";
                doc.parse_confidence = parsed.confidence;
                repo_.UpdateDocumentParse(doc);
            }
        }
    }

    // ---- 4. Claim history ----
    result.claim_versions_built = BuildClaimHistory(case_id);

    // ---- 5. OA tracker + new-document bookkeeping ----
    result.oa_records_linked = SyncOaTracker(case_id);
    TimelineService timeline(repo_);
    timeline.LinkResponsesToOfficeActions(case_id);
    int new_count = repo_.CountNewDocuments(case_id);
    repo_.UpdateCaseSyncState(case_id, "completed", "", new_count);

    if (new_count > 0) {
        repo_.LogSyncEvent(case_id, "sync_completed",
                           std::to_string(new_count) + " new document(s)", false);
        PATX_LOG_INFO("USPTO sync for " + record.application_number + ": " +
                      std::to_string(new_count) + " new documents");
    }

    result.ok = true;
    if (!report(SyncState::Completed, 100, "Sync complete")) {
        // cancellation after completion is fine
    }
    return result;
}

int UsptoSyncService::BuildClaimHistory(int uspto_case_id) {
    // Reconstruction principle (37 CFR 1.121): trust the complete claim
    // listing inside each amendment, in date order; no PDF-to-PDF patching.
    auto docs = repo_.GetDocumentsForCaseOrdered(uspto_case_id, true);

    struct Candidate {
        UsptoDocument doc;
        std::string type;
    };
    std::vector<Candidate> candidates;
    for (auto& doc : docs) {
        if (!doc.extracted_text.empty() && CarriesClaims(doc.category)) {
            candidates.push_back({doc, VersionTypeFor(doc.category)});
        }
    }
    if (candidates.empty()) return 0;

    repo_.DeleteClaimVersionsForCase(uspto_case_id);

    int version_no = 0;
    int built = 0;
    bool first = true;
    for (auto& candidate : candidates) {
        auto parsed = ClaimParser::Parse(candidate.doc.extracted_text);
        if (!parsed.found_listing || parsed.claims.empty()) continue;

        version_no++;
        ClaimVersion version;
        version.uspto_case_id = uspto_case_id;
        version.source_document_id = candidate.doc.id;
        version.version_no = version_no;
        version.effective_date = candidate.doc.filing_date;
        version.version_type = first ? "initial" : candidate.type;
        version.source_type = candidate.doc.text_source;
        version.is_initial = first;
        version.parse_confidence = parsed.confidence;
        repo_.InsertClaimVersion(version);

        for (const auto& claim : parsed.claims) {
            Claim stored = claim;
            stored.id = 0;
            stored.claim_version_id = version.id;
            repo_.InsertClaim(stored);
        }

        // Low confidence listings stay flagged for human review
        candidate.doc.parse_status = parsed.confidence >= 0.8 ? "parsed" : "needs_review";
        candidate.doc.parse_confidence = parsed.confidence;
        repo_.UpdateDocumentParse(candidate.doc);

        first = false;
        built++;
    }

    // Mark the latest version as current
    auto versions = repo_.GetClaimVersions(uspto_case_id);
    if (!versions.empty()) {
        // is_current lives on the last version row
        repo_.SetCurrentClaimVersion(uspto_case_id, versions.back().id);
    }
    PATX_LOG_INFO("Claim history for case " + std::to_string(uspto_case_id) + ": " +
                  std::to_string(built) + " versions");
    return built;
}

int UsptoSyncService::SyncOaTracker(int uspto_case_id) {
    UsptoCase record = repo_.GetCaseById(uspto_case_id);
    if (record.id == 0) return 0;

    auto docs = repo_.GetDocumentsForCaseOrdered(uspto_case_id, true);
    int created = 0;
    for (const auto& doc : docs) {
        if (!IsOaCategory(doc.category)) continue;

        // Existing link? Never touch user-owned fields on re-sync.
        if (db_.FindOAByExternalDocument(doc.document_identifier) > 0) continue;

        OARecord oa;
        oa.patent_title = record.title;
        oa.oa_type = doc.document_category;   // "Non-Final Office Action" ...
        oa.issue_date = doc.mail_date.empty() ? doc.filing_date : doc.mail_date;
        oa.jurisdiction = "US";
        oa.source = "USPTO";
        oa.external_case_id = std::to_string(record.id);
        oa.external_document_id = doc.document_identifier;
        oa.progress = "pending";
        oa.is_completed = false;

        // Suggested deadline from the rule table; marked calculated so the
        // user can override it (manual deadlines survive re-syncs).
        std::string deadline = db_.CalculateDeadline("US", "oa_response", oa.issue_date);
        if (!deadline.empty()) {
            oa.official_deadline = deadline;
            oa.deadline_source = "calculated";
        } else {
            oa.deadline_source = "needs_confirmation";
        }

        if (db_.InsertOA(oa, /*log_undo=*/false) > 0) {
            created++;
        }
    }
    return created;
}

SyncResult UsptoSyncService::DownloadAndParseDocument(int document_id, const ProgressFn& progress) {
    SyncResult result;
    auto doc = repo_.GetDocumentById(document_id);
    if (doc.id == 0) {
        result.error = "document not found";
        return result;
    }
    UsptoCase record = repo_.GetCaseById(doc.uspto_case_id);

    if (doc.download_status != "downloaded" || doc.local_file_path.empty()) {
        if (!doc.file_download_uri.empty()) {
            if (progress) progress(SyncState::Downloading, 20, "Downloading document");
            std::string name = SanitizeFileName(doc.filing_date + "_" + doc.document_code + "_" +
                                                doc.document_identifier) + ".pdf";
            std::string dest = (std::filesystem::path(config_.document_folder) /
                                record.application_number / name).string();
            std::string sha;
            long long size = 0;
            ApiResult dl = client_.DownloadDocument(doc.file_download_uri, dest, sha, &size);
            if (!dl.ok) {
                result.error = dl.error;
                result.http_status = dl.http_status;
                result.http_status_line = dl.status_line;
                doc.download_status = "download_failed";
                repo_.UpdateDocumentDownload(doc);
                return result;
            }
            doc.local_file_path = (std::filesystem::path(record.application_number) / name).string();
            doc.sha256 = sha;
            doc.document_size = size;
            doc.download_status = "downloaded";
            repo_.UpdateDocumentDownload(doc);
        } else {
            result.error = "document has no download URL";
            return result;
        }
    }

    if (progress) progress(SyncState::Parsing, 60, "Extracting text");
    if (doc.extracted_text.empty()) {
        std::string abs = (std::filesystem::path(config_.document_folder) / doc.local_file_path).string();
        auto text = DocumentTextExtractor::Extract(abs, doc.mime_type);
        if (text.success) {
            doc.extracted_text = text.text;
            doc.text_source = text.source;
            doc.parse_confidence = text.confidence;
        } else {
            doc.parse_status = "needs_review";
            repo_.UpdateDocumentParse(doc);
            result.ok = true;
            result.error = "downloaded, but text extraction failed: " + text.error;
            return result;
        }
    }

    // Re-run structured parsing now that text exists
    if (IsOaCategory(doc.category)) {
        auto parsed = OaParser::Parse(doc.extracted_text);
        if (parsed.is_office_action) {
            repo_.DeleteRejectionsForDocument(doc.id);
            for (const auto& rejection : parsed.rejections) {
                OaRejection r = rejection;
                r.document_id = doc.id;
                repo_.InsertRejection(r);
            }
            doc.parse_status = parsed.confidence >= 0.75 ? "parsed" : "needs_review";
        }
    } else {
        doc.parse_status = doc.parse_confidence >= 0.8 ? "parsed" : "needs_review";
    }
    repo_.UpdateDocumentParse(doc);

    // Claim versions may now include this document
    result.claim_versions_built = BuildClaimHistory(doc.uspto_case_id);

    result.ok = true;
    result.documents_downloaded = 1;
    if (progress) progress(SyncState::Completed, 100, "Done");
    return result;
}

} // namespace patx
