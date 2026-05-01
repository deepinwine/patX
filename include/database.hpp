// patX Database - SQLite wrapper for patent data
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <ctime>
#include <sqlite3.h>

struct Patent {
    int id = 0;
    std::string geke_code;
    std::string application_number;
    std::string title;
    std::string proposal_name;
    std::string application_status;
    std::string patent_type;
    std::string patent_level;
    std::string application_date;
    std::string authorization_date;
    std::string expiration_date;
    std::string geke_handler;
    std::string rd_department;
    std::string agency_firm;
    std::string original_applicant;
    std::string current_applicant;
    std::string inventor;
    std::string notes;
    std::string class_level1;
    std::string class_level2;
    std::string class_level3;
    long long updated_at = 0; // timestamp for sync conflict detection
};

struct OARecord {
    int id = 0;
    int patent_id = 0;
    std::string geke_code;
    std::string patent_title;
    std::string oa_type;
    std::string official_deadline;
    std::string issue_date;
    std::string response_date;
    std::string handler;
    std::string writer;
    std::string progress;
    std::string agency;
    std::string oa_summary;
    bool is_completed = false;
    bool is_extendable = false;
    bool extension_requested = false;
    int extension_months = 0;
    std::string extended_deadline;
    std::string notes;
};

struct PCTPatent {
    int id = 0;
    std::string geke_code;
    std::string domestic_source;
    std::string application_no;
    std::string country_app_no;
    std::string title;
    std::string application_status;
    std::string handler;
    std::string inventor;
    std::string filing_date;
    std::string application_date;
    std::string priority_date;
    std::string country;
    std::string notes;
};

struct SoftwareCopyright {
    int id = 0;
    std::string case_no;
    std::string reg_no;
    std::string title;
    std::string original_owner;
    std::string current_owner;
    std::string application_status;
    std::string handler;
    std::string developer;
    std::string inventor;
    std::string dev_complete_date;
    std::string application_date;
    std::string reg_date;
    std::string version;
    std::string notes;
};

struct ICLayout {
    int id = 0;
    std::string case_no;
    std::string reg_no;
    std::string title;
    std::string original_owner;
    std::string current_owner;
    std::string application_status;
    std::string handler;
    std::string designer;
    std::string inventor;
    std::string application_date;
    std::string creation_date;
    std::string cert_date;
    std::string notes;
};

struct ForeignPatent {
    int id = 0;
    std::string case_no;
    std::string pct_no;
    std::string country_app_no;
    std::string title;
    std::string owner;
    std::string patent_status;
    std::string handler;
    std::string inventor;
    std::string application_date;
    std::string authorization_date;
    std::string country;
    std::string application_no;
    std::string notes;
};

class Database {
public:
    Database(const std::string& db_path);
    ~Database();

    bool IsOpen() const { return db_ != nullptr; }
    sqlite3* GetHandle() { return db_; }

    // Patents
    std::vector<Patent> GetPatents(const std::string& status_filter = "",
                                    const std::string& level_filter = "",
                                    const std::string& handler_filter = "");
    Patent GetPatentById(int id);
    Patent GetPatentByCode(const std::string& geke_code);
    int InsertPatent(const Patent& p, bool log_undo = true);
    bool UpdatePatent(int id, const Patent& p, bool log_undo = true);
    bool DeletePatent(int id, bool log_undo = true);
    std::vector<Patent> SearchPatents(const std::string& keyword);

    // OA Records
    std::vector<OARecord> GetOARecords(const std::string& filter_type = "",
                                        const std::string& handler_filter = "",
                                        const std::string& writer_filter = "");
    OARecord GetOAById(int id);
    std::vector<OARecord> GetOAByPatent(const std::string& geke_code);
    int InsertOA(const OARecord& oa, bool log_undo = true);
    bool UpdateOA(int id, const OARecord& oa, bool log_undo = true);
    bool DeleteOA(int id, bool log_undo = true);
    bool MarkOACompleted(int id);

    // PCT
    std::vector<PCTPatent> GetPCTPatents(const std::string& handler_filter = "");
    PCTPatent GetPCTById(int id);
    int InsertPCT(const PCTPatent& p);
    bool UpdatePCT(int id, const PCTPatent& p);
    bool DeletePCT(int id);

    // Software
    std::vector<SoftwareCopyright> GetSoftwareCopyrights(const std::string& handler_filter = "");
    SoftwareCopyright GetSoftwareById(int id);
    int InsertSoftware(const SoftwareCopyright& s);
    bool DeleteSoftware(int id);

    // IC Layouts
    std::vector<ICLayout> GetICLayouts();
    ICLayout GetICById(int id);
    int InsertIC(const ICLayout& ic);
    bool DeleteIC(int id);

    // Foreign
    std::vector<ForeignPatent> GetForeignPatents();
    ForeignPatent GetForeignById(int id);
    int InsertForeign(const ForeignPatent& f);
    bool DeleteForeign(int id);

    // Utility
    std::vector<std::string> GetDistinctValues(const std::string& table, const std::string& column);
    void SetConfig(const std::string& key, const std::string& value);
    std::string GetConfig(const std::string& key);

    // Undo support
    void BeginBatch();
    int Undo();
    bool CanUndo() const;

private:
    sqlite3* db_ = nullptr;

    void InitTables();
    void MigrateTables();
    bool Execute(const std::string& sql);
    std::string EscapeString(const std::string& s);
    std::string GetCurrentDate();

    // JSON serialization for undo
    std::string PatentToJson(const Patent& p);
    std::string OAToJson(const OARecord& oa);
};

// Global undo manager accessor
class UndoManager;
UndoManager& GetUndoManager();
