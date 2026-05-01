// patX Database Implementation
#include "database.hpp"
#include "undo_manager.hpp"
#include <sstream>
#include <iostream>
#include <set>

static UndoManager* g_undo_manager = nullptr;

UndoManager& GetUndoManager() {
    return *g_undo_manager;
}

Database::Database(const std::string& db_path) {
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "Cannot open database: " << sqlite3_errmsg(db_) << std::endl;
        db_ = nullptr;
        return;
    }

    // Enable WAL mode for better performance
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size=10000;", nullptr, nullptr, nullptr);

    // Initialize undo manager
    g_undo_manager = new UndoManager();
    g_undo_manager->SetDatabase(db_);

    // Create tables
    InitTables();

    // Migrate tables (add missing columns)
    MigrateTables();
}

Database::~Database() {
    delete g_undo_manager;
    g_undo_manager = nullptr;
    if (db_) {
        sqlite3_close(db_);
    }
}

void Database::InitTables() {
    Execute(R"(
        CREATE TABLE IF NOT EXISTS patents (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            geke_code TEXT UNIQUE,
            application_number TEXT,
            title TEXT,
            proposal_name TEXT,
            application_status TEXT,
            patent_type TEXT,
            patent_level TEXT,
            application_date TEXT,
            authorization_date TEXT,
            expiration_date TEXT,
            geke_handler TEXT,
            rd_department TEXT,
            agency_firm TEXT,
            original_applicant TEXT,
            current_applicant TEXT,
            inventor TEXT,
            notes TEXT,
            class_level1 TEXT,
            class_level2 TEXT,
            class_level3 TEXT,
            updated_at INTEGER DEFAULT 0
        )
    )");

    Execute(R"(
        CREATE TABLE IF NOT EXISTS oa_records (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            patent_id INTEGER,
            geke_code TEXT,
            patent_title TEXT,
            oa_type TEXT,
            official_deadline TEXT,
            issue_date TEXT,
            response_date TEXT,
            handler TEXT,
            writer TEXT,
            progress TEXT,
            agency TEXT,
            oa_summary TEXT,
            is_completed INTEGER DEFAULT 0,
            is_extendable INTEGER DEFAULT 0,
            extension_requested INTEGER DEFAULT 0,
            extension_months INTEGER,
            extended_deadline TEXT,
            notes TEXT
        )
    )");

    Execute(R"(
        CREATE TABLE IF NOT EXISTS pct_patents (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            geke_code TEXT UNIQUE,
            domestic_source TEXT,
            application_no TEXT,
            country_app_no TEXT,
            title TEXT,
            application_status TEXT,
            handler TEXT,
            inventor TEXT,
            filing_date TEXT,
            application_date TEXT,
            priority_date TEXT,
            country TEXT,
            notes TEXT
        )
    )");

    Execute(R"(
        CREATE TABLE IF NOT EXISTS software_copyrights (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            case_no TEXT UNIQUE,
            reg_no TEXT,
            title TEXT,
            original_owner TEXT,
            current_owner TEXT,
            application_status TEXT,
            handler TEXT,
            developer TEXT,
            inventor TEXT,
            dev_complete_date TEXT,
            application_date TEXT,
            reg_date TEXT,
            version TEXT,
            notes TEXT
        )
    )");

    Execute(R"(
        CREATE TABLE IF NOT EXISTS ic_layouts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            case_no TEXT UNIQUE,
            reg_no TEXT,
            title TEXT,
            original_owner TEXT,
            current_owner TEXT,
            application_status TEXT,
            handler TEXT,
            designer TEXT,
            inventor TEXT,
            application_date TEXT,
            creation_date TEXT,
            cert_date TEXT,
            notes TEXT
        )
    )");

    Execute(R"(
        CREATE TABLE IF NOT EXISTS foreign_patents (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            case_no TEXT UNIQUE,
            pct_no TEXT,
            country_app_no TEXT,
            title TEXT,
            owner TEXT,
            patent_status TEXT,
            handler TEXT,
            inventor TEXT,
            application_date TEXT,
            authorization_date TEXT,
            country TEXT,
            application_no TEXT,
            notes TEXT
        )
    )");

    Execute(R"(
        CREATE TABLE IF NOT EXISTS annual_fees (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            patent_id INTEGER,
            geke_code TEXT,
            patent_title TEXT,
            patent_type TEXT,
            fee_year INTEGER,
            fee_amount TEXT,
            fee_period_end TEXT,
            grace_period_end TEXT,
            is_paid INTEGER DEFAULT 0,
            payment_date TEXT,
            notes TEXT
        )
    )");
}

void Database::MigrateTables() {
    // Helper to get existing columns
    auto getColumns = [this](const std::string& table) -> std::set<std::string> {
        std::set<std::string> columns;
        std::string sql = "PRAGMA table_info(" + table + ");";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* col_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (col_name) columns.insert(col_name);
            }
            sqlite3_finalize(stmt);
        }
        return columns;
    };

    // Helper to add column if missing
    auto addColumn = [this](const std::string& table, const std::string& col, const std::string& type) {
        std::string sql = "ALTER TABLE " + table + " ADD COLUMN " + col + " " + type + ";";
        sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    };

    // Migrate patents table
    auto patent_cols = getColumns("patents");
    if (!patent_cols.count("patent_level")) addColumn("patents", "patent_level", "TEXT");
    if (!patent_cols.count("geke_handler")) addColumn("patents", "geke_handler", "TEXT");
    if (!patent_cols.count("class_level1")) addColumn("patents", "class_level1", "TEXT");
    if (!patent_cols.count("class_level2")) addColumn("patents", "class_level2", "TEXT");
    if (!patent_cols.count("class_level3")) addColumn("patents", "class_level3", "TEXT");
    if (!patent_cols.count("rd_department")) addColumn("patents", "rd_department", "TEXT");
    if (!patent_cols.count("agency_firm")) addColumn("patents", "agency_firm", "TEXT");
    if (!patent_cols.count("original_applicant")) addColumn("patents", "original_applicant", "TEXT");
    if (!patent_cols.count("current_applicant")) addColumn("patents", "current_applicant", "TEXT");
    if (!patent_cols.count("proposal_name")) addColumn("patents", "proposal_name", "TEXT");

    // Migrate oa_records table
    auto oa_cols = getColumns("oa_records");
    if (!oa_cols.count("writer")) addColumn("oa_records", "writer", "TEXT");
    if (!oa_cols.count("progress")) addColumn("oa_records", "progress", "TEXT");
    if (!oa_cols.count("agency")) addColumn("oa_records", "agency", "TEXT");
    if (!oa_cols.count("oa_summary")) addColumn("oa_records", "oa_summary", "TEXT");
    if (!oa_cols.count("is_extendable")) addColumn("oa_records", "is_extendable", "INTEGER DEFAULT 0");
    if (!oa_cols.count("extension_requested")) addColumn("oa_records", "extension_requested", "INTEGER DEFAULT 0");
    if (!oa_cols.count("extension_months")) addColumn("oa_records", "extension_months", "INTEGER");
    if (!oa_cols.count("extended_deadline")) addColumn("oa_records", "extended_deadline", "TEXT");

    // Migrate pct_patents table
    auto pct_cols = getColumns("pct_patents");
    if (!pct_cols.count("domestic_source")) addColumn("pct_patents", "domestic_source", "TEXT");
    if (!pct_cols.count("country_app_no")) addColumn("pct_patents", "country_app_no", "TEXT");
    if (!pct_cols.count("filing_date")) addColumn("pct_patents", "filing_date", "TEXT");
    if (!pct_cols.count("priority_date")) addColumn("pct_patents", "priority_date", "TEXT");
    if (!pct_cols.count("country")) addColumn("pct_patents", "country", "TEXT");

    // Migrate software_copyrights table
    auto sw_cols = getColumns("software_copyrights");
    if (!sw_cols.count("original_owner")) addColumn("software_copyrights", "original_owner", "TEXT");
    if (!sw_cols.count("current_owner")) addColumn("software_copyrights", "current_owner", "TEXT");
    if (!sw_cols.count("developer")) addColumn("software_copyrights", "developer", "TEXT");
    if (!sw_cols.count("dev_complete_date")) addColumn("software_copyrights", "dev_complete_date", "TEXT");
    if (!sw_cols.count("version")) addColumn("software_copyrights", "version", "TEXT");

    // Migrate ic_layouts table
    auto ic_cols = getColumns("ic_layouts");
    if (!ic_cols.count("original_owner")) addColumn("ic_layouts", "original_owner", "TEXT");
    if (!ic_cols.count("current_owner")) addColumn("ic_layouts", "current_owner", "TEXT");
    if (!ic_cols.count("designer")) addColumn("ic_layouts", "designer", "TEXT");
    if (!ic_cols.count("creation_date")) addColumn("ic_layouts", "creation_date", "TEXT");
    if (!ic_cols.count("cert_date")) addColumn("ic_layouts", "cert_date", "TEXT");

    // Migrate foreign_patents table
    auto fp_cols = getColumns("foreign_patents");
    if (!fp_cols.count("pct_no")) addColumn("foreign_patents", "pct_no", "TEXT");
    if (!fp_cols.count("country_app_no")) addColumn("foreign_patents", "country_app_no", "TEXT");
    if (!fp_cols.count("owner")) addColumn("foreign_patents", "owner", "TEXT");
    if (!fp_cols.count("patent_status")) addColumn("foreign_patents", "patent_status", "TEXT");
    if (!fp_cols.count("country")) addColumn("foreign_patents", "country", "TEXT");
    if (!fp_cols.count("application_no")) addColumn("foreign_patents", "application_no", "TEXT");
}

bool Database::Execute(const std::string& sql) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL error: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

std::string Database::EscapeString(const std::string& s) {
    std::string result;
    result.reserve(s.size() * 2);
    for (char c : s) {
        if (c == '\'') result += "''";
        else result += c;
    }
    return result;
}

std::string Database::GetCurrentDate() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[11];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    return std::string(buf);
}

std::string Database::PatentToJson(const Patent& p) {
    std::ostringstream json;
    json << "{";
    json << "\"id\":" << p.id << ",";
    json << "\"geke_code\":\"" << EscapeString(p.geke_code) << "\",";
    json << "\"application_number\":\"" << EscapeString(p.application_number) << "\",";
    json << "\"title\":\"" << EscapeString(p.title) << "\",";
    json << "\"proposal_name\":\"" << EscapeString(p.proposal_name) << "\",";
    json << "\"application_status\":\"" << EscapeString(p.application_status) << "\",";
    json << "\"patent_type\":\"" << EscapeString(p.patent_type) << "\",";
    json << "\"patent_level\":\"" << EscapeString(p.patent_level) << "\",";
    json << "\"application_date\":\"" << EscapeString(p.application_date) << "\",";
    json << "\"authorization_date\":\"" << EscapeString(p.authorization_date) << "\",";
    json << "\"expiration_date\":\"" << EscapeString(p.expiration_date) << "\",";
    json << "\"geke_handler\":\"" << EscapeString(p.geke_handler) << "\",";
    json << "\"rd_department\":\"" << EscapeString(p.rd_department) << "\",";
    json << "\"agency_firm\":\"" << EscapeString(p.agency_firm) << "\",";
    json << "\"original_applicant\":\"" << EscapeString(p.original_applicant) << "\",";
    json << "\"current_applicant\":\"" << EscapeString(p.current_applicant) << "\",";
    json << "\"inventor\":\"" << EscapeString(p.inventor) << "\",";
    json << "\"notes\":\"" << EscapeString(p.notes) << "\",";
    json << "\"class_level1\":\"" << EscapeString(p.class_level1) << "\",";
    json << "\"class_level2\":\"" << EscapeString(p.class_level2) << "\",";
    json << "\"class_level3\":\"" << EscapeString(p.class_level3) << "\"";
    json << "}";
    return json.str();
}

std::string Database::OAToJson(const OARecord& oa) {
    std::ostringstream json;
    json << "{";
    json << "\"id\":" << oa.id << ",";
    json << "\"patent_id\":" << oa.patent_id << ",";
    json << "\"geke_code\":\"" << EscapeString(oa.geke_code) << "\",";
    json << "\"patent_title\":\"" << EscapeString(oa.patent_title) << "\",";
    json << "\"oa_type\":\"" << EscapeString(oa.oa_type) << "\",";
    json << "\"official_deadline\":\"" << EscapeString(oa.official_deadline) << "\",";
    json << "\"issue_date\":\"" << EscapeString(oa.issue_date) << "\",";
    json << "\"handler\":\"" << EscapeString(oa.handler) << "\",";
    json << "\"writer\":\"" << EscapeString(oa.writer) << "\",";
    json << "\"progress\":\"" << EscapeString(oa.progress) << "\",";
    json << "\"is_completed\":" << (oa.is_completed ? 1 : 0) << ",";
    json << "\"is_extendable\":" << (oa.is_extendable ? 1 : 0);
    json << "}";
    return json.str();
}

// ============== Patents ==============
std::vector<Patent> Database::GetPatents(const std::string& status_filter,
                                          const std::string& level_filter,
                                          const std::string& handler_filter) {
    std::vector<Patent> results;
    std::string sql = "SELECT * FROM patents WHERE 1=1";

    if (!status_filter.empty()) {
        sql += " AND application_status = '" + EscapeString(status_filter) + "'";
    }
    if (!level_filter.empty()) {
        sql += " AND patent_level = '" + EscapeString(level_filter) + "'";
    }
    if (!handler_filter.empty()) {
        sql += " AND geke_handler = '" + EscapeString(handler_filter) + "'";
    }
    sql += " ORDER BY id DESC";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Patent p;
            p.id = sqlite3_column_int(stmt, 0);
            p.geke_code = (const char*)sqlite3_column_text(stmt, 1) ? (const char*)sqlite3_column_text(stmt, 1) : "";
            p.application_number = (const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
            p.title = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
            p.proposal_name = (const char*)sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "";
            p.application_status = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
            p.patent_type = (const char*)sqlite3_column_text(stmt, 6) ? (const char*)sqlite3_column_text(stmt, 6) : "";
            p.patent_level = (const char*)sqlite3_column_text(stmt, 7) ? (const char*)sqlite3_column_text(stmt, 7) : "";
            p.application_date = (const char*)sqlite3_column_text(stmt, 8) ? (const char*)sqlite3_column_text(stmt, 8) : "";
            p.authorization_date = (const char*)sqlite3_column_text(stmt, 9) ? (const char*)sqlite3_column_text(stmt, 9) : "";
            p.expiration_date = (const char*)sqlite3_column_text(stmt, 10) ? (const char*)sqlite3_column_text(stmt, 10) : "";
            p.geke_handler = (const char*)sqlite3_column_text(stmt, 11) ? (const char*)sqlite3_column_text(stmt, 11) : "";
            p.rd_department = (const char*)sqlite3_column_text(stmt, 12) ? (const char*)sqlite3_column_text(stmt, 12) : "";
            p.agency_firm = (const char*)sqlite3_column_text(stmt, 13) ? (const char*)sqlite3_column_text(stmt, 13) : "";
            p.original_applicant = (const char*)sqlite3_column_text(stmt, 14) ? (const char*)sqlite3_column_text(stmt, 14) : "";
            p.current_applicant = (const char*)sqlite3_column_text(stmt, 15) ? (const char*)sqlite3_column_text(stmt, 15) : "";
            p.inventor = (const char*)sqlite3_column_text(stmt, 16) ? (const char*)sqlite3_column_text(stmt, 16) : "";
            p.notes = (const char*)sqlite3_column_text(stmt, 17) ? (const char*)sqlite3_column_text(stmt, 17) : "";
            p.class_level1 = (const char*)sqlite3_column_text(stmt, 18) ? (const char*)sqlite3_column_text(stmt, 18) : "";
            p.class_level2 = (const char*)sqlite3_column_text(stmt, 19) ? (const char*)sqlite3_column_text(stmt, 19) : "";
            p.class_level3 = (const char*)sqlite3_column_text(stmt, 20) ? (const char*)sqlite3_column_text(stmt, 20) : "";
            results.push_back(p);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

Patent Database::GetPatentById(int id) {
    Patent p;
    std::string sql = "SELECT * FROM patents WHERE id = " + std::to_string(id);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            p.id = sqlite3_column_int(stmt, 0);
            p.geke_code = (const char*)sqlite3_column_text(stmt, 1) ? (const char*)sqlite3_column_text(stmt, 1) : "";
            p.application_number = (const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
            p.title = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
            p.patent_type = (const char*)sqlite3_column_text(stmt, 6) ? (const char*)sqlite3_column_text(stmt, 6) : "";
            p.patent_level = (const char*)sqlite3_column_text(stmt, 7) ? (const char*)sqlite3_column_text(stmt, 7) : "";
            p.application_status = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
            p.geke_handler = (const char*)sqlite3_column_text(stmt, 11) ? (const char*)sqlite3_column_text(stmt, 11) : "";
            p.inventor = (const char*)sqlite3_column_text(stmt, 16) ? (const char*)sqlite3_column_text(stmt, 16) : "";
            p.application_date = (const char*)sqlite3_column_text(stmt, 8) ? (const char*)sqlite3_column_text(stmt, 8) : "";
            p.authorization_date = (const char*)sqlite3_column_text(stmt, 9) ? (const char*)sqlite3_column_text(stmt, 9) : "";
            p.expiration_date = (const char*)sqlite3_column_text(stmt, 10) ? (const char*)sqlite3_column_text(stmt, 10) : "";
        }
        sqlite3_finalize(stmt);
    }
    return p;
}

Patent Database::GetPatentByCode(const std::string& geke_code) {
    Patent p;
    std::string sql = "SELECT * FROM patents WHERE geke_code = '" + EscapeString(geke_code) + "'";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            p.id = sqlite3_column_int(stmt, 0);
            p.geke_code = (const char*)sqlite3_column_text(stmt, 1) ? (const char*)sqlite3_column_text(stmt, 1) : "";
            p.title = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
            p.patent_type = (const char*)sqlite3_column_text(stmt, 6) ? (const char*)sqlite3_column_text(stmt, 6) : "";
            p.patent_level = (const char*)sqlite3_column_text(stmt, 7) ? (const char*)sqlite3_column_text(stmt, 7) : "";
            p.geke_handler = (const char*)sqlite3_column_text(stmt, 11) ? (const char*)sqlite3_column_text(stmt, 11) : "";
        }
        sqlite3_finalize(stmt);
    }
    return p;
}

int Database::InsertPatent(const Patent& p, bool log_undo) {
    std::string sql = "INSERT INTO patents (geke_code, application_number, title, application_status, patent_type, patent_level, application_date, authorization_date, expiration_date, geke_handler, inventor, class_level1, class_level2, class_level3) VALUES ('" +
        EscapeString(p.geke_code) + "','" +
        EscapeString(p.application_number) + "','" +
        EscapeString(p.title) + "','" +
        EscapeString(p.application_status) + "','" +
        EscapeString(p.patent_type) + "','" +
        EscapeString(p.patent_level) + "','" +
        EscapeString(p.application_date) + "','" +
        EscapeString(p.authorization_date) + "','" +
        EscapeString(p.expiration_date) + "','" +
        EscapeString(p.geke_handler) + "','" +
        EscapeString(p.inventor) + "','" +
        EscapeString(p.class_level1) + "','" +
        EscapeString(p.class_level2) + "','" +
        EscapeString(p.class_level3) + "')";

    if (Execute(sql)) {
        int id = sqlite3_last_insert_rowid(db_);
        if (log_undo && g_undo_manager) {
            g_undo_manager->LogOperation("delete", "patents", id, PatentToJson(p), "");
        }
        return id;
    }
    return 0;
}

bool Database::UpdatePatent(int id, const Patent& p, bool log_undo) {
    if (log_undo && g_undo_manager) {
        // Get old data for undo
        Patent old = GetPatentById(id);
        g_undo_manager->LogOperation("update", "patents", id, PatentToJson(old), PatentToJson(p));
    }

    std::string sql = "UPDATE patents SET " +
        std::string("application_number = '") + EscapeString(p.application_number) + "'," +
        "title = '" + EscapeString(p.title) + "'," +
        "application_status = '" + EscapeString(p.application_status) + "'," +
        "patent_type = '" + EscapeString(p.patent_type) + "'," +
        "patent_level = '" + EscapeString(p.patent_level) + "'," +
        "application_date = '" + EscapeString(p.application_date) + "'," +
        "authorization_date = '" + EscapeString(p.authorization_date) + "'," +
        "expiration_date = '" + EscapeString(p.expiration_date) + "'," +
        "geke_handler = '" + EscapeString(p.geke_handler) + "'," +
        "inventor = '" + EscapeString(p.inventor) + "'" +
        " WHERE id = " + std::to_string(id);
    return Execute(sql);
}

bool Database::DeletePatent(int id, bool log_undo) {
    if (log_undo && g_undo_manager) {
        Patent old = GetPatentById(id);
        g_undo_manager->LogOperation("delete", "patents", id, PatentToJson(old), "");
    }

    std::string sql = "DELETE FROM patents WHERE id = " + std::to_string(id);
    return Execute(sql);
}

std::vector<Patent> Database::SearchPatents(const std::string& keyword) {
    std::vector<Patent> results;
    std::string sql = "SELECT * FROM patents WHERE geke_code LIKE '%" + EscapeString(keyword) +
        "%' OR title LIKE '%" + EscapeString(keyword) +
        "%' OR application_number LIKE '%" + EscapeString(keyword) + "%'";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Patent p;
            p.id = sqlite3_column_int(stmt, 0);
            p.geke_code = (const char*)sqlite3_column_text(stmt, 1) ? (const char*)sqlite3_column_text(stmt, 1) : "";
            p.application_number = (const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
            p.title = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
            p.patent_type = (const char*)sqlite3_column_text(stmt, 6) ? (const char*)sqlite3_column_text(stmt, 6) : "";
            p.patent_level = (const char*)sqlite3_column_text(stmt, 7) ? (const char*)sqlite3_column_text(stmt, 7) : "";
            p.application_status = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
            results.push_back(p);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

// ============== OA Records ==============
std::vector<OARecord> Database::GetOARecords(const std::string& filter_type,
                                              const std::string& handler_filter,
                                              const std::string& writer_filter) {
    std::vector<OARecord> results;
    std::string sql = "SELECT * FROM oa_records WHERE 1=1";

    // 日期过滤
    std::string date_filter;
    if (filter_type.find("5") != std::string::npos || filter_type.find("五") != std::string::npos) {
        date_filter = " AND is_completed = 0 AND date(official_deadline) BETWEEN date('now') AND date('now', '+5 days')";
    } else if (filter_type.find("30") != std::string::npos || filter_type.find("三十") != std::string::npos) {
        date_filter = " AND is_completed = 0 AND date(official_deadline) BETWEEN date('now') AND date('now', '+30 days')";
    } else if (filter_type == "All incomplete" || filter_type == "全部未完成" || filter_type.find("incomplete") != std::string::npos) {
        sql += " AND is_completed = 0";
    } else if (filter_type == "Completed" || filter_type == "已完成") {
        sql += " AND is_completed = 1";
    } else if (filter_type == "All" || filter_type == "全部" || filter_type.empty()) {
        // 无过滤
    }
    sql += date_filter;

    if (!handler_filter.empty()) {
        sql += " AND handler = '" + EscapeString(handler_filter) + "'";
    }
    if (!writer_filter.empty()) {
        sql += " AND writer = '" + EscapeString(writer_filter) + "'";
    }
    sql += " ORDER BY official_deadline ASC";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            OARecord oa;
            oa.id = sqlite3_column_int(stmt, 0);
            oa.patent_id = sqlite3_column_int(stmt, 1);
            oa.geke_code = (const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
            oa.patent_title = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
            oa.oa_type = (const char*)sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "";
            oa.official_deadline = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
            oa.issue_date = (const char*)sqlite3_column_text(stmt, 6) ? (const char*)sqlite3_column_text(stmt, 6) : "";
            oa.handler = (const char*)sqlite3_column_text(stmt, 8) ? (const char*)sqlite3_column_text(stmt, 8) : "";
            oa.writer = (const char*)sqlite3_column_text(stmt, 9) ? (const char*)sqlite3_column_text(stmt, 9) : "";
            oa.progress = (const char*)sqlite3_column_text(stmt, 10) ? (const char*)sqlite3_column_text(stmt, 10) : "";
            oa.is_completed = sqlite3_column_int(stmt, 13) != 0;
            oa.is_extendable = sqlite3_column_int(stmt, 14) != 0;
            results.push_back(oa);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

OARecord Database::GetOAById(int id) {
    OARecord oa;
    std::string sql = "SELECT * FROM oa_records WHERE id = " + std::to_string(id);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            oa.id = sqlite3_column_int(stmt, 0);
            oa.geke_code = (const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
            oa.patent_title = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
            oa.oa_type = (const char*)sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "";
            oa.official_deadline = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
            oa.handler = (const char*)sqlite3_column_text(stmt, 8) ? (const char*)sqlite3_column_text(stmt, 8) : "";
            oa.writer = (const char*)sqlite3_column_text(stmt, 9) ? (const char*)sqlite3_column_text(stmt, 9) : "";
            oa.progress = (const char*)sqlite3_column_text(stmt, 10) ? (const char*)sqlite3_column_text(stmt, 10) : "";
            oa.is_completed = sqlite3_column_int(stmt, 13) != 0;
        }
        sqlite3_finalize(stmt);
    }
    return oa;
}

std::vector<OARecord> Database::GetOAByPatent(const std::string& geke_code) {
    std::vector<OARecord> results;
    std::string sql = "SELECT * FROM oa_records WHERE geke_code = '" + EscapeString(geke_code) + "'";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            OARecord oa;
            oa.id = sqlite3_column_int(stmt, 0);
            oa.oa_type = (const char*)sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "";
            oa.official_deadline = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
            oa.handler = (const char*)sqlite3_column_text(stmt, 8) ? (const char*)sqlite3_column_text(stmt, 8) : "";
            oa.writer = (const char*)sqlite3_column_text(stmt, 9) ? (const char*)sqlite3_column_text(stmt, 9) : "";
            oa.progress = (const char*)sqlite3_column_text(stmt, 10) ? (const char*)sqlite3_column_text(stmt, 10) : "";
            oa.is_completed = sqlite3_column_int(stmt, 13) != 0;
            results.push_back(oa);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

int Database::InsertOA(const OARecord& oa, bool log_undo) {
    std::string sql = "INSERT INTO oa_records (patent_id, geke_code, patent_title, oa_type, official_deadline, issue_date, handler, writer, progress, is_completed, is_extendable) VALUES (" +
        std::to_string(oa.patent_id) + ",'" +
        EscapeString(oa.geke_code) + "','" +
        EscapeString(oa.patent_title) + "','" +
        EscapeString(oa.oa_type) + "','" +
        EscapeString(oa.official_deadline) + "','" +
        EscapeString(oa.issue_date) + "','" +
        EscapeString(oa.handler) + "','" +
        EscapeString(oa.writer) + "','" +
        EscapeString(oa.progress) + "'," +
        std::to_string(oa.is_completed ? 1 : 0) + "," +
        std::to_string(oa.is_extendable ? 1 : 0) + ")";

    if (Execute(sql)) {
        int id = sqlite3_last_insert_rowid(db_);
        if (log_undo && g_undo_manager) {
            g_undo_manager->LogOperation("delete", "oa_records", id, OAToJson(oa), "");
        }
        return id;
    }
    return 0;
}

bool Database::UpdateOA(int id, const OARecord& oa, bool log_undo) {
    if (log_undo && g_undo_manager) {
        OARecord old = GetOAById(id);
        g_undo_manager->LogOperation("update", "oa_records", id, OAToJson(old), OAToJson(oa));
    }

    std::string sql = "UPDATE oa_records SET " +
        std::string("oa_type = '") + EscapeString(oa.oa_type) + "'," +
        "official_deadline = '" + EscapeString(oa.official_deadline) + "'," +
        "handler = '" + EscapeString(oa.handler) + "'," +
        "writer = '" + EscapeString(oa.writer) + "'," +
        "progress = '" + EscapeString(oa.progress) + "'," +
        "is_completed = " + std::to_string(oa.is_completed ? 1 : 0) +
        " WHERE id = " + std::to_string(id);
    return Execute(sql);
}

bool Database::DeleteOA(int id, bool log_undo) {
    if (log_undo && g_undo_manager) {
        OARecord old = GetOAById(id);
        g_undo_manager->LogOperation("delete", "oa_records", id, OAToJson(old), "");
    }

    std::string sql = "DELETE FROM oa_records WHERE id = " + std::to_string(id);
    return Execute(sql);
}

bool Database::MarkOACompleted(int id) {
    std::string sql = "UPDATE oa_records SET is_completed = 1, progress = 'completed', response_date = '" + GetCurrentDate() + "' WHERE id = " + std::to_string(id);
    return Execute(sql);
}

// ============== PCT ==============
std::vector<PCTPatent> Database::GetPCTPatents(const std::string& handler_filter) {
    std::vector<PCTPatent> results;
    std::string sql = "SELECT * FROM pct_patents";
    if (!handler_filter.empty()) {
        sql += " WHERE handler = '" + EscapeString(handler_filter) + "'";
    }

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            PCTPatent p;
            p.id = sqlite3_column_int(stmt, 0);
            p.geke_code = (const char*)sqlite3_column_text(stmt, 1) ? (const char*)sqlite3_column_text(stmt, 1) : "";
            p.domestic_source = (const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
            p.application_no = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
            p.country_app_no = (const char*)sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "";
            p.title = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
            p.application_status = (const char*)sqlite3_column_text(stmt, 6) ? (const char*)sqlite3_column_text(stmt, 6) : "";
            p.handler = (const char*)sqlite3_column_text(stmt, 7) ? (const char*)sqlite3_column_text(stmt, 7) : "";
            results.push_back(p);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

int Database::InsertPCT(const PCTPatent& p) {
    std::string sql = "INSERT INTO pct_patents (geke_code, domestic_source, application_no, country_app_no, title, application_status, handler) VALUES ('" +
        EscapeString(p.geke_code) + "','" +
        EscapeString(p.domestic_source) + "','" +
        EscapeString(p.application_no) + "','" +
        EscapeString(p.country_app_no) + "','" +
        EscapeString(p.title) + "','" +
        EscapeString(p.application_status) + "','" +
        EscapeString(p.handler) + "')";

    if (Execute(sql)) {
        return sqlite3_last_insert_rowid(db_);
    }
    return 0;
}

bool Database::UpdatePCT(int id, const PCTPatent& p) {
    std::string sql = "UPDATE pct_patents SET title = '" + EscapeString(p.title) + "', application_status = '" + EscapeString(p.application_status) + "' WHERE id = " + std::to_string(id);
    return Execute(sql);
}

bool Database::DeletePCT(int id) {
    return Execute("DELETE FROM pct_patents WHERE id = " + std::to_string(id));
}

// ============== Software ==============
std::vector<SoftwareCopyright> Database::GetSoftwareCopyrights(const std::string& handler_filter) {
    std::vector<SoftwareCopyright> results;
    std::string sql = "SELECT * FROM software_copyrights";
    if (!handler_filter.empty()) {
        sql += " WHERE handler = '" + EscapeString(handler_filter) + "'";
    }

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SoftwareCopyright s;
            s.id = sqlite3_column_int(stmt, 0);
            s.case_no = (const char*)sqlite3_column_text(stmt, 1) ? (const char*)sqlite3_column_text(stmt, 1) : "";
            s.reg_no = (const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
            s.title = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
            s.original_owner = (const char*)sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "";
            s.current_owner = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
            s.application_status = (const char*)sqlite3_column_text(stmt, 6) ? (const char*)sqlite3_column_text(stmt, 6) : "";
            s.handler = (const char*)sqlite3_column_text(stmt, 7) ? (const char*)sqlite3_column_text(stmt, 7) : "";
            results.push_back(s);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

int Database::InsertSoftware(const SoftwareCopyright& s) {
    std::string sql = "INSERT INTO software_copyrights (case_no, reg_no, title, original_owner, current_owner, application_status, handler) VALUES ('" +
        EscapeString(s.case_no) + "','" +
        EscapeString(s.reg_no) + "','" +
        EscapeString(s.title) + "','" +
        EscapeString(s.original_owner) + "','" +
        EscapeString(s.current_owner) + "','" +
        EscapeString(s.application_status) + "','" +
        EscapeString(s.handler) + "')";

    if (Execute(sql)) {
        return sqlite3_last_insert_rowid(db_);
    }
    return 0;
}

bool Database::DeleteSoftware(int id) {
    return Execute("DELETE FROM software_copyrights WHERE id = " + std::to_string(id));
}

// ============== IC Layouts ==============
std::vector<ICLayout> Database::GetICLayouts() {
    std::vector<ICLayout> results;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, "SELECT * FROM ic_layouts", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ICLayout ic;
            ic.id = sqlite3_column_int(stmt, 0);
            ic.case_no = (const char*)sqlite3_column_text(stmt, 1) ? (const char*)sqlite3_column_text(stmt, 1) : "";
            ic.reg_no = (const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
            ic.title = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
            ic.original_owner = (const char*)sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "";
            ic.current_owner = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
            ic.application_status = (const char*)sqlite3_column_text(stmt, 6) ? (const char*)sqlite3_column_text(stmt, 6) : "";
            ic.handler = (const char*)sqlite3_column_text(stmt, 7) ? (const char*)sqlite3_column_text(stmt, 7) : "";
            results.push_back(ic);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

int Database::InsertIC(const ICLayout& ic) {
    std::string sql = "INSERT INTO ic_layouts (case_no, reg_no, title, original_owner, current_owner, application_status, handler) VALUES ('" +
        EscapeString(ic.case_no) + "','" +
        EscapeString(ic.reg_no) + "','" +
        EscapeString(ic.title) + "','" +
        EscapeString(ic.original_owner) + "','" +
        EscapeString(ic.current_owner) + "','" +
        EscapeString(ic.application_status) + "','" +
        EscapeString(ic.handler) + "')";

    if (Execute(sql)) {
        return sqlite3_last_insert_rowid(db_);
    }
    return 0;
}

bool Database::DeleteIC(int id) {
    return Execute("DELETE FROM ic_layouts WHERE id = " + std::to_string(id));
}

// ============== Foreign Patents ==============
std::vector<ForeignPatent> Database::GetForeignPatents() {
    std::vector<ForeignPatent> results;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, "SELECT * FROM foreign_patents", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ForeignPatent f;
            f.id = sqlite3_column_int(stmt, 0);
            f.case_no = (const char*)sqlite3_column_text(stmt, 1) ? (const char*)sqlite3_column_text(stmt, 1) : "";
            f.pct_no = (const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
            f.country_app_no = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
            f.title = (const char*)sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "";
            f.owner = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
            f.patent_status = (const char*)sqlite3_column_text(stmt, 6) ? (const char*)sqlite3_column_text(stmt, 6) : "";
            f.handler = (const char*)sqlite3_column_text(stmt, 7) ? (const char*)sqlite3_column_text(stmt, 7) : "";
            results.push_back(f);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

int Database::InsertForeign(const ForeignPatent& f) {
    std::string sql = "INSERT INTO foreign_patents (case_no, pct_no, country_app_no, title, owner, patent_status, handler) VALUES ('" +
        EscapeString(f.case_no) + "','" +
        EscapeString(f.pct_no) + "','" +
        EscapeString(f.country_app_no) + "','" +
        EscapeString(f.title) + "','" +
        EscapeString(f.owner) + "','" +
        EscapeString(f.patent_status) + "','" +
        EscapeString(f.handler) + "')";

    if (Execute(sql)) {
        return sqlite3_last_insert_rowid(db_);
    }
    return 0;
}

bool Database::DeleteForeign(int id) {
    return Execute("DELETE FROM foreign_patents WHERE id = " + std::to_string(id));
}

// ============== Utility ==============
std::vector<std::string> Database::GetDistinctValues(const std::string& table, const std::string& column) {
    std::vector<std::string> results;
    std::string sql = "SELECT DISTINCT " + column + " FROM " + table + " WHERE " + column + " IS NOT NULL AND " + column + " != '' ORDER BY " + column;

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back((const char*)sqlite3_column_text(stmt, 0) ? (const char*)sqlite3_column_text(stmt, 0) : "");
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

void Database::SetConfig(const std::string& key, const std::string& value) {
    Execute(R"(CREATE TABLE IF NOT EXISTS config (key TEXT PRIMARY KEY, value TEXT))");
    std::string sql = "INSERT OR REPLACE INTO config (key, value) VALUES ('" + EscapeString(key) + "','" + EscapeString(value) + "')";
    Execute(sql);
}

std::string Database::GetConfig(const std::string& key) {
    Execute(R"(CREATE TABLE IF NOT EXISTS config (key TEXT PRIMARY KEY, value TEXT))");
    std::string sql = "SELECT value FROM config WHERE key = '" + EscapeString(key) + "'";
    sqlite3_stmt* stmt;
    std::string result;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = (const char*)sqlite3_column_text(stmt, 0) ? (const char*)sqlite3_column_text(stmt, 0) : "";
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

void Database::BeginBatch() {
    if (g_undo_manager) g_undo_manager->BeginBatch();
}

int Database::Undo() {
    if (g_undo_manager) return g_undo_manager->Undo();
    return 0;
}

bool Database::CanUndo() const {
    if (g_undo_manager) return g_undo_manager->CanUndo();
    return false;
}
