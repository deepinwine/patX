// patX Database Implementation
#include "database.hpp"
#include <sstream>
#include <iostream>

Database::Database(const std::string& db_path) {
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "Cannot open database: " << sqlite3_errmsg(db_) << std::endl;
        db_ = nullptr;
        return;
    }
    
    // Create tables
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
            class_level3 TEXT
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
}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
    }
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
            p.geke_code = (const char*)sqlite3_column_text(stmt, 1);
            p.application_number = (const char*)sqlite3_column_text(stmt, 2);
            p.title = (const char*)sqlite3_column_text(stmt, 3);
            p.proposal_name = (const char*)sqlite3_column_text(stmt, 4);
            p.application_status = (const char*)sqlite3_column_text(stmt, 5);
            p.patent_type = (const char*)sqlite3_column_text(stmt, 6);
            p.patent_level = (const char*)sqlite3_column_text(stmt, 7);
            p.application_date = (const char*)sqlite3_column_text(stmt, 8);
            p.authorization_date = (const char*)sqlite3_column_text(stmt, 9);
            p.expiration_date = (const char*)sqlite3_column_text(stmt, 10);
            p.geke_handler = (const char*)sqlite3_column_text(stmt, 11);
            p.rd_department = (const char*)sqlite3_column_text(stmt, 12);
            p.agency_firm = (const char*)sqlite3_column_text(stmt, 13);
            p.original_applicant = (const char*)sqlite3_column_text(stmt, 14);
            p.current_applicant = (const char*)sqlite3_column_text(stmt, 15);
            p.inventor = (const char*)sqlite3_column_text(stmt, 16);
            p.notes = (const char*)sqlite3_column_text(stmt, 17);
            p.class_level1 = (const char*)sqlite3_column_text(stmt, 18);
            p.class_level2 = (const char*)sqlite3_column_text(stmt, 19);
            p.class_level3 = (const char*)sqlite3_column_text(stmt, 20);
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
            p.geke_code = (const char*)sqlite3_column_text(stmt, 1);
            p.application_number = (const char*)sqlite3_column_text(stmt, 2);
            p.title = (const char*)sqlite3_column_text(stmt, 3);
            p.patent_type = (const char*)sqlite3_column_text(stmt, 6);
            p.patent_level = (const char*)sqlite3_column_text(stmt, 7);
            p.application_status = (const char*)sqlite3_column_text(stmt, 5);
            p.geke_handler = (const char*)sqlite3_column_text(stmt, 11);
            p.inventor = (const char*)sqlite3_column_text(stmt, 16);
            p.application_date = (const char*)sqlite3_column_text(stmt, 8);
            p.authorization_date = (const char*)sqlite3_column_text(stmt, 9);
            p.expiration_date = (const char*)sqlite3_column_text(stmt, 10);
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
            p.geke_code = (const char*)sqlite3_column_text(stmt, 1);
            p.title = (const char*)sqlite3_column_text(stmt, 3);
            p.patent_type = (const char*)sqlite3_column_text(stmt, 6);
            p.patent_level = (const char*)sqlite3_column_text(stmt, 7);
            p.geke_handler = (const char*)sqlite3_column_text(stmt, 11);
        }
        sqlite3_finalize(stmt);
    }
    return p;
}

int Database::InsertPatent(const Patent& p) {
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
        return sqlite3_last_insert_rowid(db_);
    }
    return 0;
}

bool Database::UpdatePatent(int id, const Patent& p) {
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

bool Database::DeletePatent(int id) {
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
            p.geke_code = (const char*)sqlite3_column_text(stmt, 1);
            p.application_number = (const char*)sqlite3_column_text(stmt, 2);
            p.title = (const char*)sqlite3_column_text(stmt, 3);
            p.patent_type = (const char*)sqlite3_column_text(stmt, 6);
            p.patent_level = (const char*)sqlite3_column_text(stmt, 7);
            p.application_status = (const char*)sqlite3_column_text(stmt, 5);
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
    
    if (filter_type == "All incomplete") {
        sql += " AND is_completed = 0";
    } else if (filter_type == "Completed") {
        sql += " AND is_completed = 1";
    }
    
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
            oa.geke_code = (const char*)sqlite3_column_text(stmt, 2);
            oa.patent_title = (const char*)sqlite3_column_text(stmt, 3);
            oa.oa_type = (const char*)sqlite3_column_text(stmt, 4);
            oa.official_deadline = (const char*)sqlite3_column_text(stmt, 5);
            oa.issue_date = (const char*)sqlite3_column_text(stmt, 6);
            oa.handler = (const char*)sqlite3_column_text(stmt, 8);
            oa.writer = (const char*)sqlite3_column_text(stmt, 9);
            oa.progress = (const char*)sqlite3_column_text(stmt, 10);
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
            oa.geke_code = (const char*)sqlite3_column_text(stmt, 2);
            oa.patent_title = (const char*)sqlite3_column_text(stmt, 3);
            oa.oa_type = (const char*)sqlite3_column_text(stmt, 4);
            oa.official_deadline = (const char*)sqlite3_column_text(stmt, 5);
            oa.handler = (const char*)sqlite3_column_text(stmt, 8);
            oa.writer = (const char*)sqlite3_column_text(stmt, 9);
            oa.progress = (const char*)sqlite3_column_text(stmt, 10);
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
            oa.oa_type = (const char*)sqlite3_column_text(stmt, 4);
            oa.official_deadline = (const char*)sqlite3_column_text(stmt, 5);
            oa.handler = (const char*)sqlite3_column_text(stmt, 8);
            oa.writer = (const char*)sqlite3_column_text(stmt, 9);
            oa.progress = (const char*)sqlite3_column_text(stmt, 10);
            oa.is_completed = sqlite3_column_int(stmt, 13) != 0;
            results.push_back(oa);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

int Database::InsertOA(const OARecord& oa) {
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
        return sqlite3_last_insert_rowid(db_);
    }
    return 0;
}

bool Database::UpdateOA(int id, const OARecord& oa) {
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

bool Database::DeleteOA(int id) {
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
            p.geke_code = (const char*)sqlite3_column_text(stmt, 1);
            p.domestic_source = (const char*)sqlite3_column_text(stmt, 2);
            p.application_no = (const char*)sqlite3_column_text(stmt, 3);
            p.country_app_no = (const char*)sqlite3_column_text(stmt, 4);
            p.title = (const char*)sqlite3_column_text(stmt, 5);
            p.application_status = (const char*)sqlite3_column_text(stmt, 6);
            p.handler = (const char*)sqlite3_column_text(stmt, 7);
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
            s.case_no = (const char*)sqlite3_column_text(stmt, 1);
            s.reg_no = (const char*)sqlite3_column_text(stmt, 2);
            s.title = (const char*)sqlite3_column_text(stmt, 3);
            s.original_owner = (const char*)sqlite3_column_text(stmt, 4);
            s.current_owner = (const char*)sqlite3_column_text(stmt, 5);
            s.application_status = (const char*)sqlite3_column_text(stmt, 6);
            s.handler = (const char*)sqlite3_column_text(stmt, 7);
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
            ic.case_no = (const char*)sqlite3_column_text(stmt, 1);
            ic.reg_no = (const char*)sqlite3_column_text(stmt, 2);
            ic.title = (const char*)sqlite3_column_text(stmt, 3);
            ic.original_owner = (const char*)sqlite3_column_text(stmt, 4);
            ic.current_owner = (const char*)sqlite3_column_text(stmt, 5);
            ic.application_status = (const char*)sqlite3_column_text(stmt, 6);
            ic.handler = (const char*)sqlite3_column_text(stmt, 7);
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
            f.case_no = (const char*)sqlite3_column_text(stmt, 1);
            f.pct_no = (const char*)sqlite3_column_text(stmt, 2);
            f.country_app_no = (const char*)sqlite3_column_text(stmt, 3);
            f.title = (const char*)sqlite3_column_text(stmt, 4);
            f.owner = (const char*)sqlite3_column_text(stmt, 5);
            f.patent_status = (const char*)sqlite3_column_text(stmt, 6);
            f.handler = (const char*)sqlite3_column_text(stmt, 7);
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
            results.push_back((const char*)sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}