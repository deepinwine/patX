// Persistence layer for USPTO prosecution data. Owns the uspto_* tables and
// is the only place that writes them.
#pragma once

#include "patx/uspto_models.hpp"

#include <string>
#include <vector>

struct sqlite3;

namespace patx {

class UsptoRepository {
public:
    explicit UsptoRepository(sqlite3* db);

    // Idempotent table creation; also called from the Database constructor.
    void EnsureTables();

    // ----- Cases -----
    int UpsertCase(UsptoCase& c);                       // by application_number; fills c.id
    UsptoCase GetCaseById(int id);
    UsptoCase GetCaseByApplicationNumber(const std::string& app_no);
    std::vector<UsptoCase> GetAllCases();
    bool UpdateCaseSyncState(int case_id, const std::string& status,
                             const std::string& error, int new_document_count);
    bool DeleteCase(int id);
    // Case -> foreign_patents link (0 clears)
    bool LinkCaseToForeignPatent(int case_id, int foreign_patent_id);

    // ----- Documents -----
    // Dedup key: (uspto_case_id, document_identifier). Returns the doc id and
    // sets *created=false when the document already existed.
    int UpsertDocument(UsptoDocument& doc, bool* created = nullptr);
    UsptoDocument GetDocumentById(int id);
    UsptoDocument GetDocumentByIdentifier(int case_id, const std::string& identifier);
    std::vector<UsptoDocument> GetDocumentsForCase(int case_id);
    bool UpdateDocumentDownload(UsptoDocument& doc);    // path/sha/status/text fields
    bool UpdateDocumentParse(UsptoDocument& doc);       // parse fields
    bool LinkDocuments(int response_doc_id, int oa_doc_id);
    // Documents in category groups, ordered by date (for timeline building)
    std::vector<UsptoDocument> GetDocumentsForCaseOrdered(int case_id, bool ascending = true);
    int CountNewDocuments(int case_id);
    bool MarkDocumentsSeen(int case_id);

    // ----- Claim versions / claims -----
    int InsertClaimVersion(ClaimVersion& v);
    bool DeleteClaimVersionsForCase(int case_id);
    std::vector<ClaimVersion> GetClaimVersions(int case_id);   // ordered by version_no
    ClaimVersion GetClaimVersion(int version_id);
    // Flags one version as current and clears the flag on the others.
    bool SetCurrentClaimVersion(int case_id, int version_id);
    int InsertClaim(Claim& c);
    std::vector<Claim> GetClaims(int claim_version_id);        // ordered by claim_number
    // Full text of one claim across a version (helper for diff viewer)
    Claim GetClaim(int claim_version_id, int claim_number);

    // ----- Rejections -----
    int InsertRejection(OaRejection& r);
    std::vector<OaRejection> GetRejectionsForDocument(int document_id);
    bool DeleteRejectionsForDocument(int document_id);

    // ----- Sync log -----
    void LogSyncEvent(int case_id, const std::string& event, const std::string& detail,
                      bool is_error);
    struct SyncLogEntry {
        int id = 0;
        int case_id = 0;
        std::string event;
        std::string detail;
        std::string created_at;
        bool is_error = false;
    };
    std::vector<SyncLogEntry> GetSyncLog(int case_id, int limit = 100);

private:
    sqlite3* db_;
};

} // namespace patx
