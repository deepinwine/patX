// Orchestrates a full case sync against the USPTO Open Data Portal:
// metadata -> documents (incremental, dedup by documentIdentifier) ->
// lazy/optional downloads -> text extraction -> OA parsing -> claim history
// reconstruction -> OA tracker integration.
//
// The service is synchronous and owns no Database instance; callers run it on
// a worker thread with their own connection (WAL allows concurrent readers).
#pragma once

#include "database.hpp"
#include "patx/uspto_client.hpp"
#include "patx/uspto_models.hpp"
#include "patx/uspto_repository.hpp"

#include <functional>
#include <string>
#include <vector>

namespace patx {

enum class SyncState {
    Idle,
    FetchingMetadata,
    FetchingDocuments,
    Downloading,
    Parsing,
    Completed,
    PartialFailure,
    Failed,
};

const char* ToString(SyncState s);

class UsptoSyncService {
public:
    // progress(state, percent [0..100], message); return false to cancel.
    using ProgressFn = std::function<bool(SyncState, int, const std::string&)>;

    UsptoSyncService(Database& db, const UsptoConfig& config);

    // Adds a case by application number (first sync). Optionally links to an
    // existing foreign_patents row.
    SyncResult AddCase(const std::string& application_number, int foreign_patent_id = 0,
                       const ProgressFn& progress = nullptr);

    // Incremental sync of one case (by uspto_cases.id).
    SyncResult SyncCase(int uspto_case_id, const ProgressFn& progress = nullptr);

    // Syncs every known case.
    std::vector<SyncResult> SyncAllCases(const ProgressFn& progress = nullptr);

    // Lazy download of one document + text extraction + parsing.
    SyncResult DownloadAndParseDocument(int document_id, const ProgressFn& progress = nullptr);

    // Rebuilds the claim version history from stored, parsed documents.
    // Returns the number of versions built.
    int BuildClaimHistory(int uspto_case_id);

    // Creates/updates OA tracker records (oa_records) for examiner OAs found
    // in the document set. Never touches handler/writer/progress/completed of
    // existing records - those belong to the user.
    int SyncOaTracker(int uspto_case_id);

    UsptoClient& client() { return client_; }

private:
    SyncResult RunSync(UsptoCase& record, const ProgressFn& progress);

    Database& db_;
    UsptoRepository repo_;
    UsptoClient client_;
    UsptoConfig config_;
};

} // namespace patx
