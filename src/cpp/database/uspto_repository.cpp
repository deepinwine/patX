#include "patx/uspto_repository.hpp"
#include "patx/log.hpp"

#include <sqlite3.h>
#include <ctime>

namespace patx {

namespace {

std::string Col(sqlite3_stmt* stmt, int col) {
    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return text ? text : "";
}

std::string Q(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    out += "'";
    return out;
}

bool Exec(sqlite3* db, const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        PATX_LOG_ERROR(std::string("uspto SQL error: ") + (err ? err : "?") + " | " + sql.substr(0, 160));
        sqlite3_free(err);
        return false;
    }
    return true;
}

std::string NowISO() {
    time_t now = time(nullptr);
    struct tm tm_buf;
#ifdef _WIN32
    gmtime_s(&tm_buf, &now);
#else
    gmtime_r(&now, &tm_buf);
#endif
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buf;
}

} // namespace

UsptoRepository::UsptoRepository(sqlite3* db) : db_(db) {}

void UsptoRepository::EnsureTables() {
    if (!db_) return;

    Exec(db_, R"(
        CREATE TABLE IF NOT EXISTS uspto_cases (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            foreign_patent_id INTEGER DEFAULT 0,
            internal_case_no TEXT,
            application_number TEXT UNIQUE NOT NULL,
            publication_number TEXT,
            patent_number TEXT,
            title TEXT,
            filing_date TEXT,
            publication_date TEXT,
            grant_date TEXT,
            priority_date TEXT,
            application_type TEXT,
            application_status TEXT,
            status_date TEXT,
            first_named_inventor TEXT,
            applicant_name TEXT,
            assignee_name TEXT,
            examiner_name TEXT,
            art_unit TEXT,
            technology_center TEXT,
            attorney_docket_number TEXT,
            entity_status TEXT,
            parent_application_number TEXT,
            continuity_json TEXT,
            last_uspto_modified_at TEXT,
            last_synced_at TEXT,
            sync_status TEXT DEFAULT 'idle',
            sync_error TEXT,
            new_document_count INTEGER DEFAULT 0,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP,
            updated_at TEXT
        );
    )");

    Exec(db_, R"(
        CREATE TABLE IF NOT EXISTS uspto_documents (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uspto_case_id INTEGER NOT NULL,
            document_identifier TEXT NOT NULL,
            document_code TEXT,
            document_category TEXT,
            document_description TEXT,
            document_title TEXT,
            filing_date TEXT,
            mail_date TEXT,
            document_size INTEGER DEFAULT 0,
            mime_type TEXT,
            file_download_uri TEXT,
            xml_download_uri TEXT,
            local_file_path TEXT,
            local_text_path TEXT,
            ocr_text TEXT,
            extracted_text TEXT,
            text_source TEXT,
            sha256 TEXT,
            source_last_modified_at TEXT,
            download_status TEXT DEFAULT 'not_downloaded',
            parse_status TEXT DEFAULT 'pending',
            parse_confidence REAL DEFAULT 0,
            raw_document_code TEXT,
            raw_description TEXT,
            party TEXT DEFAULT 'unknown',
            related_document_id INTEGER DEFAULT 0,
            page_count INTEGER DEFAULT 0,
            is_new INTEGER DEFAULT 0,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP,
            updated_at TEXT,
            UNIQUE (uspto_case_id, document_identifier)
        );
    )");

    Exec(db_, R"(
        CREATE TABLE IF NOT EXISTS uspto_rejections (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            document_id INTEGER NOT NULL,
            statute TEXT,
            rejection_type TEXT,
            claim_numbers TEXT,
            reference_text TEXT,
            primary_reference TEXT,
            secondary_references TEXT,
            section_text TEXT,
            parse_confidence REAL DEFAULT 0
        );
    )");

    Exec(db_, R"(
        CREATE TABLE IF NOT EXISTS claim_versions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uspto_case_id INTEGER NOT NULL,
            source_document_id INTEGER DEFAULT 0,
            version_no INTEGER NOT NULL,
            effective_date TEXT,
            version_type TEXT,
            source_type TEXT,
            is_initial INTEGER DEFAULT 0,
            is_current INTEGER DEFAULT 0,
            parse_confidence REAL DEFAULT 0,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP
        );
    )");

    Exec(db_, R"(
        CREATE TABLE IF NOT EXISTS claims (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            claim_version_id INTEGER NOT NULL,
            claim_number INTEGER NOT NULL,
            claim_status TEXT,
            claim_text_clean TEXT,
            claim_text_marked TEXT,
            is_independent INTEGER DEFAULT 0,
            parent_claim_numbers TEXT,
            parse_confidence REAL DEFAULT 0
        );
    )");

    Exec(db_, R"(
        CREATE TABLE IF NOT EXISTS uspto_sync_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uspto_case_id INTEGER,
            event TEXT,
            detail TEXT,
            is_error INTEGER DEFAULT 0,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP
        );
    )");

    Exec(db_, "CREATE INDEX IF NOT EXISTS idx_uspto_docs_case ON uspto_documents(uspto_case_id);");
    Exec(db_, "CREATE INDEX IF NOT EXISTS idx_uspto_docs_identifier ON uspto_documents(document_identifier);");
    Exec(db_, "CREATE UNIQUE INDEX IF NOT EXISTS idx_uspto_docs_unique ON uspto_documents(uspto_case_id, document_identifier);");
    Exec(db_, "CREATE INDEX IF NOT EXISTS idx_uspto_docs_date ON uspto_documents(filing_date);");
    Exec(db_, "CREATE INDEX IF NOT EXISTS idx_uspto_cases_app_no ON uspto_cases(application_number);");
    Exec(db_, "CREATE INDEX IF NOT EXISTS idx_claim_versions_case ON claim_versions(uspto_case_id);");
    Exec(db_, "CREATE INDEX IF NOT EXISTS idx_claims_version ON claims(claim_version_id);");
    Exec(db_, "CREATE INDEX IF NOT EXISTS idx_rejections_doc ON uspto_rejections(document_id);");
    Exec(db_, "CREATE INDEX IF NOT EXISTS idx_sync_log_case ON uspto_sync_log(uspto_case_id);");
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

namespace {

void ReadCase(sqlite3_stmt* stmt, UsptoCase& c) {
    c.id = sqlite3_column_int(stmt, 0);
    c.foreign_patent_id = sqlite3_column_int(stmt, 1);
    c.internal_case_no = Col(stmt, 2);
    c.application_number = Col(stmt, 3);
    c.publication_number = Col(stmt, 4);
    c.patent_number = Col(stmt, 5);
    c.title = Col(stmt, 6);
    c.filing_date = Col(stmt, 7);
    c.publication_date = Col(stmt, 8);
    c.grant_date = Col(stmt, 9);
    c.priority_date = Col(stmt, 10);
    c.application_type = Col(stmt, 11);
    c.application_status = Col(stmt, 12);
    c.status_date = Col(stmt, 13);
    c.first_named_inventor = Col(stmt, 14);
    c.applicant_name = Col(stmt, 15);
    c.assignee_name = Col(stmt, 16);
    c.examiner_name = Col(stmt, 17);
    c.art_unit = Col(stmt, 18);
    c.technology_center = Col(stmt, 19);
    c.attorney_docket_number = Col(stmt, 20);
    c.entity_status = Col(stmt, 21);
    c.parent_application_number = Col(stmt, 22);
    c.continuity_json = Col(stmt, 23);
    c.last_uspto_modified_at = Col(stmt, 24);
    c.last_synced_at = Col(stmt, 25);
    c.sync_status = Col(stmt, 26);
    c.sync_error = Col(stmt, 27);
    c.new_document_count = sqlite3_column_int(stmt, 28);
}

const char* kCaseColumns =
    "id, foreign_patent_id, internal_case_no, application_number, publication_number, "
    "patent_number, title, filing_date, publication_date, grant_date, priority_date, "
    "application_type, application_status, status_date, first_named_inventor, applicant_name, "
    "assignee_name, examiner_name, art_unit, technology_center, attorney_docket_number, "
    "entity_status, parent_application_number, continuity_json, last_uspto_modified_at, "
    "last_synced_at, sync_status, sync_error, new_document_count";

} // namespace

int UsptoRepository::UpsertCase(UsptoCase& c) {
    if (!db_ || c.application_number.empty()) return 0;

    UsptoCase existing = GetCaseByApplicationNumber(c.application_number);
    std::string now = NowISO();

    if (existing.id > 0) {
        std::string sql = std::string("UPDATE uspto_cases SET ") +
            "foreign_patent_id = " + std::to_string(c.foreign_patent_id ? c.foreign_patent_id : existing.foreign_patent_id) + "," +
            "internal_case_no = " + Q(c.internal_case_no.empty() ? existing.internal_case_no : c.internal_case_no) + "," +
            "publication_number = " + Q(c.publication_number.empty() ? existing.publication_number : c.publication_number) + "," +
            "patent_number = " + Q(c.patent_number.empty() ? existing.patent_number : c.patent_number) + "," +
            "title = " + Q(c.title.empty() ? existing.title : c.title) + "," +
            "filing_date = " + Q(c.filing_date.empty() ? existing.filing_date : c.filing_date) + "," +
            "publication_date = " + Q(c.publication_date.empty() ? existing.publication_date : c.publication_date) + "," +
            "grant_date = " + Q(c.grant_date.empty() ? existing.grant_date : c.grant_date) + "," +
            "priority_date = " + Q(c.priority_date.empty() ? existing.priority_date : c.priority_date) + "," +
            "application_type = " + Q(c.application_type.empty() ? existing.application_type : c.application_type) + "," +
            "application_status = " + Q(c.application_status.empty() ? existing.application_status : c.application_status) + "," +
            "status_date = " + Q(c.status_date.empty() ? existing.status_date : c.status_date) + "," +
            "first_named_inventor = " + Q(c.first_named_inventor.empty() ? existing.first_named_inventor : c.first_named_inventor) + "," +
            "applicant_name = " + Q(c.applicant_name.empty() ? existing.applicant_name : c.applicant_name) + "," +
            "assignee_name = " + Q(c.assignee_name.empty() ? existing.assignee_name : c.assignee_name) + "," +
            "examiner_name = " + Q(c.examiner_name.empty() ? existing.examiner_name : c.examiner_name) + "," +
            "art_unit = " + Q(c.art_unit.empty() ? existing.art_unit : c.art_unit) + "," +
            "technology_center = " + Q(c.technology_center.empty() ? existing.technology_center : c.technology_center) + "," +
            "attorney_docket_number = " + Q(c.attorney_docket_number.empty() ? existing.attorney_docket_number : c.attorney_docket_number) + "," +
            "entity_status = " + Q(c.entity_status.empty() ? existing.entity_status : c.entity_status) + "," +
            "parent_application_number = " + Q(c.parent_application_number.empty() ? existing.parent_application_number : c.parent_application_number) + "," +
            "continuity_json = " + Q(c.continuity_json.empty() ? existing.continuity_json : c.continuity_json) + "," +
            "last_uspto_modified_at = " + Q(c.last_uspto_modified_at) + "," +
            "last_synced_at = " + Q(now) + "," +
            "sync_status = " + Q(c.sync_status.empty() ? "completed" : c.sync_status) + "," +
            "sync_error = " + Q(c.sync_error) + "," +
            "updated_at = " + Q(now) +
            " WHERE id = " + std::to_string(existing.id);
        if (!Exec(db_, sql)) return 0;
        c.id = existing.id;
        return existing.id;
    }

    std::string sql = std::string(
        "INSERT INTO uspto_cases (foreign_patent_id, internal_case_no, application_number, "
        "publication_number, patent_number, title, filing_date, publication_date, grant_date, "
        "priority_date, application_type, application_status, status_date, first_named_inventor, "
        "applicant_name, assignee_name, examiner_name, art_unit, technology_center, "
        "attorney_docket_number, entity_status, parent_application_number, continuity_json, "
        "last_uspto_modified_at, last_synced_at, sync_status, sync_error) VALUES (") +
        std::to_string(c.foreign_patent_id) + "," + Q(c.internal_case_no) + "," +
        Q(c.application_number) + "," + Q(c.publication_number) + "," + Q(c.patent_number) + "," +
        Q(c.title) + "," + Q(c.filing_date) + "," + Q(c.publication_date) + "," + Q(c.grant_date) + "," +
        Q(c.priority_date) + "," + Q(c.application_type) + "," + Q(c.application_status) + "," +
        Q(c.status_date) + "," + Q(c.first_named_inventor) + "," + Q(c.applicant_name) + "," +
        Q(c.assignee_name) + "," + Q(c.examiner_name) + "," + Q(c.art_unit) + "," +
        Q(c.technology_center) + "," + Q(c.attorney_docket_number) + "," + Q(c.entity_status) + "," +
        Q(c.parent_application_number) + "," + Q(c.continuity_json) + "," +
        Q(c.last_uspto_modified_at) + "," + Q(now) + "," +
        Q(c.sync_status.empty() ? "completed" : c.sync_status) + "," + Q(c.sync_error) + ")";
    if (!Exec(db_, sql)) return 0;
    c.id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    return c.id;
}

UsptoCase UsptoRepository::GetCaseById(int id) {
    UsptoCase c;
    if (!db_) return c;
    sqlite3_stmt* stmt;
    std::string sql = std::string("SELECT ") + kCaseColumns + " FROM uspto_cases WHERE id = " + std::to_string(id);
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) ReadCase(stmt, c);
        sqlite3_finalize(stmt);
    }
    return c;
}

UsptoCase UsptoRepository::GetCaseByApplicationNumber(const std::string& app_no) {
    UsptoCase c;
    if (!db_) return c;
    sqlite3_stmt* stmt;
    std::string sql = std::string("SELECT ") + kCaseColumns +
                      " FROM uspto_cases WHERE application_number = " + Q(app_no);
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) ReadCase(stmt, c);
        sqlite3_finalize(stmt);
    }
    return c;
}

std::vector<UsptoCase> UsptoRepository::GetAllCases() {
    std::vector<UsptoCase> results;
    if (!db_) return results;
    sqlite3_stmt* stmt;
    std::string sql = std::string("SELECT ") + kCaseColumns +
                      " FROM uspto_cases ORDER BY application_number";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            UsptoCase c;
            ReadCase(stmt, c);
            results.push_back(std::move(c));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

bool UsptoRepository::UpdateCaseSyncState(int case_id, const std::string& status,
                                          const std::string& error, int new_document_count) {
    if (!db_) return false;
    return Exec(db_, "UPDATE uspto_cases SET sync_status = " + Q(status) +
                     ", sync_error = " + Q(error) +
                     ", new_document_count = " + std::to_string(new_document_count) +
                     ", last_synced_at = " + Q(NowISO()) +
                     " WHERE id = " + std::to_string(case_id));
}

bool UsptoRepository::DeleteCase(int id) {
    if (!db_) return false;
    Exec(db_, "DELETE FROM claims WHERE claim_version_id IN "
              "(SELECT id FROM claim_versions WHERE uspto_case_id = " + std::to_string(id) + ")");
    Exec(db_, "DELETE FROM claim_versions WHERE uspto_case_id = " + std::to_string(id));
    Exec(db_, "DELETE FROM uspto_rejections WHERE document_id IN "
              "(SELECT id FROM uspto_documents WHERE uspto_case_id = " + std::to_string(id) + ")");
    Exec(db_, "DELETE FROM uspto_documents WHERE uspto_case_id = " + std::to_string(id));
    Exec(db_, "DELETE FROM uspto_sync_log WHERE uspto_case_id = " + std::to_string(id));
    return Exec(db_, "DELETE FROM uspto_cases WHERE id = " + std::to_string(id));
}

bool UsptoRepository::LinkCaseToForeignPatent(int case_id, int foreign_patent_id) {
    if (!db_) return false;
    return Exec(db_, "UPDATE uspto_cases SET foreign_patent_id = " +
                     std::to_string(foreign_patent_id) + ", updated_at = " + Q(NowISO()) +
                     " WHERE id = " + std::to_string(case_id));
}

// ---------------------------------------------------------------------------
// Documents
// ---------------------------------------------------------------------------

namespace {

void ReadDocument(sqlite3_stmt* stmt, UsptoDocument& d) {
    d.id = sqlite3_column_int(stmt, 0);
    d.uspto_case_id = sqlite3_column_int(stmt, 1);
    d.document_identifier = Col(stmt, 2);
    d.document_code = Col(stmt, 3);
    d.document_category = Col(stmt, 4);
    d.document_description = Col(stmt, 5);
    d.document_title = Col(stmt, 6);
    d.filing_date = Col(stmt, 7);
    d.mail_date = Col(stmt, 8);
    d.document_size = sqlite3_column_int64(stmt, 9);
    d.mime_type = Col(stmt, 10);
    d.file_download_uri = Col(stmt, 11);
    d.xml_download_uri = Col(stmt, 12);
    d.local_file_path = Col(stmt, 13);
    d.local_text_path = Col(stmt, 14);
    d.ocr_text = Col(stmt, 15);
    d.extracted_text = Col(stmt, 16);
    d.text_source = Col(stmt, 17);
    d.sha256 = Col(stmt, 18);
    d.source_last_modified_at = Col(stmt, 19);
    d.download_status = Col(stmt, 20);
    d.parse_status = Col(stmt, 21);
    d.parse_confidence = sqlite3_column_double(stmt, 22);
    d.raw_document_code = Col(stmt, 23);
    d.raw_description = Col(stmt, 24);
    d.party = FilingParty::Unknown; // mapped from string below
    std::string party_key = Col(stmt, 25);
    if (party_key == "examiner") d.party = FilingParty::Examiner;
    else if (party_key == "applicant") d.party = FilingParty::Applicant;
    else if (party_key == "administrative") d.party = FilingParty::Administrative;
    d.related_document_id = sqlite3_column_int(stmt, 26);
    d.page_count = sqlite3_column_int(stmt, 27);
}

const char* kDocColumns =
    "id, uspto_case_id, document_identifier, document_code, document_category, "
    "document_description, document_title, filing_date, mail_date, document_size, mime_type, "
    "file_download_uri, xml_download_uri, local_file_path, local_text_path, ocr_text, "
    "extracted_text, text_source, sha256, source_last_modified_at, download_status, "
    "parse_status, parse_confidence, raw_document_code, raw_description, party, "
    "related_document_id, page_count";

} // namespace

int UsptoRepository::UpsertDocument(UsptoDocument& doc, bool* created) {
    if (!db_ || doc.uspto_case_id <= 0 || doc.document_identifier.empty()) return 0;
    if (created) *created = false;

    sqlite3_stmt* stmt;
    std::string select = std::string("SELECT id, source_last_modified_at FROM uspto_documents "
                                     "WHERE uspto_case_id = ") +
                         std::to_string(doc.uspto_case_id) +
                         " AND document_identifier = " + Q(doc.document_identifier);
    int existing_id = 0;
    std::string existing_modified;
    if (sqlite3_prepare_v2(db_, select.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            existing_id = sqlite3_column_int(stmt, 0);
            existing_modified = Col(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }

    std::string now = NowISO();
    if (existing_id > 0) {
        // Incremental sync: only refresh metadata when USPTO reports a change.
        if (!doc.source_last_modified_at.empty() &&
            doc.source_last_modified_at == existing_modified) {
            doc.id = existing_id;
            return existing_id;
        }
        std::string sql = std::string("UPDATE uspto_documents SET ") +
            "document_code = " + Q(doc.document_code) + "," +
            "document_category = " + Q(doc.document_category) + "," +
            "document_description = " + Q(doc.document_description) + "," +
            "document_title = " + Q(doc.document_title) + "," +
            "filing_date = " + Q(doc.filing_date) + "," +
            "mail_date = " + Q(doc.mail_date) + "," +
            "mime_type = " + Q(doc.mime_type) + "," +
            "file_download_uri = " + Q(doc.file_download_uri) + "," +
            "xml_download_uri = " + Q(doc.xml_download_uri) + "," +
            "source_last_modified_at = " + Q(doc.source_last_modified_at) + "," +
            "raw_document_code = " + Q(doc.raw_document_code) + "," +
            "raw_description = " + Q(doc.raw_description) + "," +
            "party = " + Q(FilingPartyKey(doc.party)) + "," +
            "page_count = " + std::to_string(doc.page_count) + "," +
            "updated_at = " + Q(now) +
            " WHERE id = " + std::to_string(existing_id);
        if (!Exec(db_, sql)) return 0;
        doc.id = existing_id;
        return existing_id;
    }

    std::string sql = std::string(
        "INSERT INTO uspto_documents (uspto_case_id, document_identifier, document_code, "
        "document_category, document_description, document_title, filing_date, mail_date, "
        "document_size, mime_type, file_download_uri, xml_download_uri, source_last_modified_at, "
        "download_status, parse_status, raw_document_code, raw_description, party, page_count, "
        "is_new, updated_at) VALUES (") +
        std::to_string(doc.uspto_case_id) + "," + Q(doc.document_identifier) + "," +
        Q(doc.document_code) + "," + Q(doc.document_category) + "," +
        Q(doc.document_description) + "," + Q(doc.document_title) + "," +
        Q(doc.filing_date) + "," + Q(doc.mail_date) + "," +
        std::to_string(doc.document_size) + "," + Q(doc.mime_type) + "," +
        Q(doc.file_download_uri) + "," + Q(doc.xml_download_uri) + "," +
        Q(doc.source_last_modified_at) + "," +
        Q(doc.download_status.empty() ? "not_downloaded" : doc.download_status) + "," +
        Q(doc.parse_status.empty() ? "pending" : doc.parse_status) + "," +
        Q(doc.raw_document_code) + "," + Q(doc.raw_description) + "," +
        Q(FilingPartyKey(doc.party)) + "," + std::to_string(doc.page_count) + ",1," + Q(now) + ")";
    if (!Exec(db_, sql)) return 0;
    doc.id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    if (created) *created = true;
    return doc.id;
}

UsptoDocument UsptoRepository::GetDocumentById(int id) {
    UsptoDocument d;
    if (!db_) return d;
    sqlite3_stmt* stmt;
    std::string sql = std::string("SELECT ") + kDocColumns +
                      " FROM uspto_documents WHERE id = " + std::to_string(id);
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) ReadDocument(stmt, d);
        sqlite3_finalize(stmt);
    }
    return d;
}

UsptoDocument UsptoRepository::GetDocumentByIdentifier(int case_id, const std::string& identifier) {
    UsptoDocument d;
    if (!db_) return d;
    sqlite3_stmt* stmt;
    std::string sql = std::string("SELECT ") + kDocColumns +
                      " FROM uspto_documents WHERE uspto_case_id = " + std::to_string(case_id) +
                      " AND document_identifier = " + Q(identifier);
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) ReadDocument(stmt, d);
        sqlite3_finalize(stmt);
    }
    return d;
}

std::vector<UsptoDocument> UsptoRepository::GetDocumentsForCase(int case_id) {
    return GetDocumentsForCaseOrdered(case_id, true);
}

std::vector<UsptoDocument> UsptoRepository::GetDocumentsForCaseOrdered(int case_id, bool ascending) {
    std::vector<UsptoDocument> results;
    if (!db_) return results;
    sqlite3_stmt* stmt;
    std::string sql = std::string("SELECT ") + kDocColumns +
                      " FROM uspto_documents WHERE uspto_case_id = " + std::to_string(case_id) +
                      " ORDER BY filing_date " + std::string(ascending ? "ASC" : "DESC") +
                      ", id ASC";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            UsptoDocument d;
            ReadDocument(stmt, d);
            results.push_back(std::move(d));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

bool UsptoRepository::UpdateDocumentDownload(UsptoDocument& doc) {
    if (!db_ || doc.id <= 0) return false;
    std::string sql = std::string("UPDATE uspto_documents SET ") +
        "local_file_path = " + Q(doc.local_file_path) + "," +
        "local_text_path = " + Q(doc.local_text_path) + "," +
        "extracted_text = " + Q(doc.extracted_text) + "," +
        "ocr_text = " + Q(doc.ocr_text) + "," +
        "text_source = " + Q(doc.text_source) + "," +
        "sha256 = " + Q(doc.sha256) + "," +
        "document_size = " + std::to_string(doc.document_size) + "," +
        "download_status = " + Q(doc.download_status) + "," +
        "updated_at = " + Q(NowISO()) +
        " WHERE id = " + std::to_string(doc.id);
    return Exec(db_, sql);
}

bool UsptoRepository::UpdateDocumentParse(UsptoDocument& doc) {
    if (!db_ || doc.id <= 0) return false;
    std::string sql = std::string("UPDATE uspto_documents SET ") +
        "parse_status = " + Q(doc.parse_status) + "," +
        "parse_confidence = " + std::to_string(doc.parse_confidence) + "," +
        "text_source = " + Q(doc.text_source) + "," +
        "extracted_text = " + Q(doc.extracted_text) + "," +
        "updated_at = " + Q(NowISO()) +
        " WHERE id = " + std::to_string(doc.id);
    return Exec(db_, sql);
}

bool UsptoRepository::LinkDocuments(int response_doc_id, int oa_doc_id) {
    if (!db_ || response_doc_id <= 0 || oa_doc_id <= 0) return false;
    return Exec(db_, "UPDATE uspto_documents SET related_document_id = " +
                     std::to_string(oa_doc_id) + " WHERE id = " + std::to_string(response_doc_id));
}

int UsptoRepository::CountNewDocuments(int case_id) {
    if (!db_) return 0;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM uspto_documents WHERE uspto_case_id = ? AND is_new = 1",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, case_id);
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return count;
    }
    return 0;
}

bool UsptoRepository::MarkDocumentsSeen(int case_id) {
    if (!db_) return false;
    return Exec(db_, "UPDATE uspto_documents SET is_new = 0 WHERE uspto_case_id = " +
                     std::to_string(case_id));
}

// ---------------------------------------------------------------------------
// Claim versions / claims
// ---------------------------------------------------------------------------

int UsptoRepository::InsertClaimVersion(ClaimVersion& v) {
    if (!db_) return 0;
    std::string sql = std::string(
        "INSERT INTO claim_versions (uspto_case_id, source_document_id, version_no, "
        "effective_date, version_type, source_type, is_initial, is_current, parse_confidence) "
        "VALUES (") +
        std::to_string(v.uspto_case_id) + "," + std::to_string(v.source_document_id) + "," +
        std::to_string(v.version_no) + "," + Q(v.effective_date) + "," + Q(v.version_type) + "," +
        Q(v.source_type) + "," + std::to_string(v.is_initial ? 1 : 0) + "," +
        std::to_string(v.is_current ? 1 : 0) + "," + std::to_string(v.parse_confidence) + ")";
    if (!Exec(db_, sql)) return 0;
    v.id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    return v.id;
}

bool UsptoRepository::DeleteClaimVersionsForCase(int case_id) {
    if (!db_) return false;
    Exec(db_, "DELETE FROM claims WHERE claim_version_id IN "
              "(SELECT id FROM claim_versions WHERE uspto_case_id = " + std::to_string(case_id) + ")");
    return Exec(db_, "DELETE FROM claim_versions WHERE uspto_case_id = " + std::to_string(case_id));
}

std::vector<ClaimVersion> UsptoRepository::GetClaimVersions(int case_id) {
    std::vector<ClaimVersion> results;
    if (!db_) return results;
    sqlite3_stmt* stmt;
    std::string sql =
        "SELECT id, uspto_case_id, source_document_id, version_no, effective_date, version_type, "
        "source_type, is_initial, is_current, parse_confidence FROM claim_versions "
        "WHERE uspto_case_id = " + std::to_string(case_id) + " ORDER BY version_no ASC";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ClaimVersion v;
            v.id = sqlite3_column_int(stmt, 0);
            v.uspto_case_id = sqlite3_column_int(stmt, 1);
            v.source_document_id = sqlite3_column_int(stmt, 2);
            v.version_no = sqlite3_column_int(stmt, 3);
            v.effective_date = Col(stmt, 4);
            v.version_type = Col(stmt, 5);
            v.source_type = Col(stmt, 6);
            v.is_initial = sqlite3_column_int(stmt, 7) != 0;
            v.is_current = sqlite3_column_int(stmt, 8) != 0;
            v.parse_confidence = sqlite3_column_double(stmt, 9);
            results.push_back(std::move(v));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

bool UsptoRepository::SetCurrentClaimVersion(int case_id, int version_id) {
    if (!db_ || case_id <= 0 || version_id <= 0) return false;
    if (!Exec(db_, "UPDATE claim_versions SET is_current = 0 WHERE uspto_case_id = " +
                   std::to_string(case_id))) {
        return false;
    }
    return Exec(db_, "UPDATE claim_versions SET is_current = 1 WHERE id = " +
                     std::to_string(version_id));
}

ClaimVersion UsptoRepository::GetClaimVersion(int version_id) {
    ClaimVersion v;
    if (!db_) return v;
    sqlite3_stmt* stmt;
    std::string sql =
        "SELECT id, uspto_case_id, source_document_id, version_no, effective_date, version_type, "
        "source_type, is_initial, is_current, parse_confidence FROM claim_versions "
        "WHERE id = " + std::to_string(version_id);
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            v.id = sqlite3_column_int(stmt, 0);
            v.uspto_case_id = sqlite3_column_int(stmt, 1);
            v.source_document_id = sqlite3_column_int(stmt, 2);
            v.version_no = sqlite3_column_int(stmt, 3);
            v.effective_date = Col(stmt, 4);
            v.version_type = Col(stmt, 5);
            v.source_type = Col(stmt, 6);
            v.is_initial = sqlite3_column_int(stmt, 7) != 0;
            v.is_current = sqlite3_column_int(stmt, 8) != 0;
            v.parse_confidence = sqlite3_column_double(stmt, 9);
        }
        sqlite3_finalize(stmt);
    }
    return v;
}

int UsptoRepository::InsertClaim(Claim& c) {
    if (!db_ || c.claim_version_id <= 0) return 0;
    std::string parents;
    for (size_t i = 0; i < c.parent_claim_numbers.size(); i++) {
        if (i > 0) parents += ",";
        parents += std::to_string(c.parent_claim_numbers[i]);
    }
    std::string sql = std::string(
        "INSERT INTO claims (claim_version_id, claim_number, claim_status, claim_text_clean, "
        "claim_text_marked, is_independent, parent_claim_numbers, parse_confidence) VALUES (") +
        std::to_string(c.claim_version_id) + "," + std::to_string(c.claim_number) + "," +
        Q(ToString(c.status)) + "," + Q(c.claim_text_clean) + "," + Q(c.claim_text_marked) + "," +
        std::to_string(c.is_independent ? 1 : 0) + "," + Q(parents) + "," +
        std::to_string(c.parse_confidence) + ")";
    if (!Exec(db_, sql)) return 0;
    c.id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    return c.id;
}

namespace {

void ReadClaim(sqlite3_stmt* stmt, Claim& c) {
    c.id = sqlite3_column_int(stmt, 0);
    c.claim_version_id = sqlite3_column_int(stmt, 1);
    c.claim_number = sqlite3_column_int(stmt, 2);
    std::string status = Col(stmt, 3);
    if (status == "original") c.status = ClaimStatus::Original;
    else if (status == "currently amended") c.status = ClaimStatus::CurrentlyAmended;
    else if (status == "previously presented") c.status = ClaimStatus::PreviouslyPresented;
    else if (status == "new") c.status = ClaimStatus::New;
    else if (status == "canceled") c.status = ClaimStatus::Canceled;
    else if (status == "withdrawn") c.status = ClaimStatus::Withdrawn;
    else if (status == "not entered") c.status = ClaimStatus::NotEntered;
    else if (status == "allowed") c.status = ClaimStatus::Allowed;
    else c.status = ClaimStatus::Unknown;
    c.claim_text_clean = Col(stmt, 4);
    c.claim_text_marked = Col(stmt, 5);
    c.is_independent = sqlite3_column_int(stmt, 6) != 0;
    std::string parents = Col(stmt, 7);
    if (!parents.empty()) {
        size_t start = 0;
        while (start <= parents.size()) {
            size_t comma = parents.find(',', start);
            std::string num = parents.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            try {
                if (!num.empty()) c.parent_claim_numbers.push_back(std::stoi(num));
            } catch (...) {}
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    c.parse_confidence = sqlite3_column_double(stmt, 8);
}

const char* kClaimColumns =
    "id, claim_version_id, claim_number, claim_status, claim_text_clean, claim_text_marked, "
    "is_independent, parent_claim_numbers, parse_confidence";

} // namespace

std::vector<Claim> UsptoRepository::GetClaims(int claim_version_id) {
    std::vector<Claim> results;
    if (!db_) return results;
    sqlite3_stmt* stmt;
    std::string sql = std::string("SELECT ") + kClaimColumns +
                      " FROM claims WHERE claim_version_id = " + std::to_string(claim_version_id) +
                      " ORDER BY claim_number ASC";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Claim c;
            ReadClaim(stmt, c);
            results.push_back(std::move(c));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

Claim UsptoRepository::GetClaim(int claim_version_id, int claim_number) {
    Claim c;
    if (!db_) return c;
    sqlite3_stmt* stmt;
    std::string sql = std::string("SELECT ") + kClaimColumns +
                      " FROM claims WHERE claim_version_id = " + std::to_string(claim_version_id) +
                      " AND claim_number = " + std::to_string(claim_number);
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) ReadClaim(stmt, c);
        sqlite3_finalize(stmt);
    }
    return c;
}

// ---------------------------------------------------------------------------
// Rejections
// ---------------------------------------------------------------------------

int UsptoRepository::InsertRejection(OaRejection& r) {
    if (!db_ || r.document_id <= 0) return 0;
    std::string sql = std::string(
        "INSERT INTO uspto_rejections (document_id, statute, rejection_type, claim_numbers, "
        "reference_text, primary_reference, secondary_references, section_text, parse_confidence) "
        "VALUES (") +
        std::to_string(r.document_id) + "," + Q(r.statute) + "," + Q(r.rejection_type) + "," +
        Q(r.claim_numbers) + "," + Q(r.reference_text) + "," + Q(r.primary_reference) + "," +
        Q(r.secondary_references) + "," + Q(r.section_text) + "," +
        std::to_string(r.parse_confidence) + ")";
    if (!Exec(db_, sql)) return 0;
    r.id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    return r.id;
}

std::vector<OaRejection> UsptoRepository::GetRejectionsForDocument(int document_id) {
    std::vector<OaRejection> results;
    if (!db_) return results;
    sqlite3_stmt* stmt;
    std::string sql =
        "SELECT id, document_id, statute, rejection_type, claim_numbers, reference_text, "
        "primary_reference, secondary_references, section_text, parse_confidence "
        "FROM uspto_rejections WHERE document_id = " + std::to_string(document_id);
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            OaRejection r;
            r.id = sqlite3_column_int(stmt, 0);
            r.document_id = sqlite3_column_int(stmt, 1);
            r.statute = Col(stmt, 2);
            r.rejection_type = Col(stmt, 3);
            r.claim_numbers = Col(stmt, 4);
            r.reference_text = Col(stmt, 5);
            r.primary_reference = Col(stmt, 6);
            r.secondary_references = Col(stmt, 7);
            r.section_text = Col(stmt, 8);
            r.parse_confidence = sqlite3_column_double(stmt, 9);
            results.push_back(std::move(r));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

bool UsptoRepository::DeleteRejectionsForDocument(int document_id) {
    if (!db_) return false;
    return Exec(db_, "DELETE FROM uspto_rejections WHERE document_id = " + std::to_string(document_id));
}

// ---------------------------------------------------------------------------
// Sync log
// ---------------------------------------------------------------------------

void UsptoRepository::LogSyncEvent(int case_id, const std::string& event,
                                   const std::string& detail, bool is_error) {
    if (!db_) return;
    Exec(db_, "INSERT INTO uspto_sync_log (uspto_case_id, event, detail, is_error) VALUES (" +
                  std::to_string(case_id) + "," + Q(event) + "," + Q(detail) + "," +
                  std::to_string(is_error ? 1 : 0) + ")");
}

std::vector<UsptoRepository::SyncLogEntry> UsptoRepository::GetSyncLog(int case_id, int limit) {
    std::vector<SyncLogEntry> results;
    if (!db_) return results;
    sqlite3_stmt* stmt;
    std::string sql =
        "SELECT id, uspto_case_id, event, detail, created_at, is_error FROM uspto_sync_log "
        "WHERE uspto_case_id = " + std::to_string(case_id) +
        " ORDER BY id DESC LIMIT " + std::to_string(limit);
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SyncLogEntry e;
            e.id = sqlite3_column_int(stmt, 0);
            e.case_id = sqlite3_column_int(stmt, 1);
            e.event = Col(stmt, 2);
            e.detail = Col(stmt, 3);
            e.created_at = Col(stmt, 4);
            e.is_error = sqlite3_column_int(stmt, 5) != 0;
            results.push_back(std::move(e));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

} // namespace patx
