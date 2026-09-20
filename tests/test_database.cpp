// Database layer tests: schema migration, full-field CRUD for every module,
// unified query filters, deadline rules.
#include "test_registry.hpp"

#include "database.hpp"
#include "patx/schema_migrations.hpp"

#include <filesystem>
#include <fstream>
#include <sqlite3.h>

using namespace testutil;

namespace {

Patent MakePatent() {
    Patent p;
    p.geke_code = "GK-TEST-001";
    p.application_number = "202510000001.X";
    p.title = "测试专利";
    p.proposal_name = "测试提案";
    p.application_status = "pending";
    p.patent_type = "invention";
    p.patent_level = "core";
    p.application_date = "2025-01-15";
    p.geke_handler = "张三";
    p.inventor = "李四; 王五";
    p.technology_route = "AI芯片";
    p.rd_project = "智算平台";
    p.tags = "核心;AI";
    p.agent_name = "赵代理";
    p.agent_code = "AG001";
    p.disclosure_writer = "钱撰写";
    p.fee_status = "已缴费";
    p.project_id = "PRJ-9";
    p.oa_reminder_1 = "2025-06-01";
    p.pct_reminder = "2027-06-01";
    p.notes = "手工备注";
    return p;
}

} // namespace

TEST(database_fresh_schema_is_versioned) {
    std::string path = TempDbPath("fresh");
    std::filesystem::remove(path);
    {
        Database db(path);
        CHECK(db.IsOpen());
        CHECK_EQ(db.SchemaVersion(), patx::kSchemaVersionCurrent);
    }
    std::filesystem::remove(path);
}

TEST(database_patent_full_field_roundtrip) {
    std::string path = TempDbPath("patent");
    std::filesystem::remove(path);
    {
        Database db(path);
        Patent in = MakePatent();
        int id = db.InsertPatent(in);
        CHECK(id > 0);

        Patent out = db.GetPatentById(id);
        CHECK_STR_EQ(out.geke_code, "GK-TEST-001");
        CHECK_STR_EQ(out.technology_route, "AI芯片");
        CHECK_STR_EQ(out.rd_project, "智算平台");
        CHECK_STR_EQ(out.tags, "核心;AI");
        CHECK_STR_EQ(out.agent_name, "赵代理");
        CHECK_STR_EQ(out.agent_code, "AG001");
        CHECK_STR_EQ(out.disclosure_writer, "钱撰写");
        CHECK_STR_EQ(out.fee_status, "已缴费");
        CHECK_STR_EQ(out.project_id, "PRJ-9");
        CHECK_STR_EQ(out.oa_reminder_1, "2025-06-01");
        CHECK_STR_EQ(out.pct_reminder, "2027-06-01");
        CHECK_STR_EQ(out.notes, "手工备注");
        CHECK(out.updated_at > 0);

        // Update must persist structured fields too
        out.technology_route = "存算一体";
        out.rd_project = "新项目";
        CHECK(db.UpdatePatent(id, out));
        Patent after = db.GetPatentById(id);
        CHECK_STR_EQ(after.technology_route, "存算一体");
        CHECK_STR_EQ(after.rd_project, "新项目");
        CHECK(after.updated_at >= out.updated_at);
    }
    std::filesystem::remove(path);
}

TEST(database_legacy_migration_preserves_data) {
    // Build a legacy (v1-shaped) database by hand: patents with notes that
    // carry the old "<prefix>: value" segments, no schema_info.
    std::string path = TempDbPath("legacy");
    std::filesystem::remove(path);
    {
        sqlite3* raw = nullptr;
        if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) {
            sqlite3_close(raw);
            Fail("cannot create legacy test db");
        }
        const char* legacy_schema =
            "CREATE TABLE patents ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " geke_code TEXT UNIQUE,"
            " application_number TEXT,"
            " title TEXT,"
            " proposal_name TEXT,"
            " application_status TEXT,"
            " patent_type TEXT,"
            " patent_level TEXT,"
            " application_date TEXT,"
            " authorization_date TEXT,"
            " expiration_date TEXT,"
            " geke_handler TEXT,"
            " rd_department TEXT,"
            " agency_firm TEXT,"
            " original_applicant TEXT,"
            " current_applicant TEXT,"
            " inventor TEXT,"
            " notes TEXT,"
            " class_level1 TEXT,"
            " class_level2 TEXT,"
            " class_level3 TEXT,"
            " updated_at INTEGER DEFAULT 0);"
            "INSERT INTO patents (geke_code, title, notes) VALUES ("
            " 'GK-OLD-1', '旧数据', "
            " '技术路线: 图像传感器; 代理人: 刘代理; 1st OA: 2025-03-01; 纯文本备注');";
        char* err = nullptr;
        if (sqlite3_exec(raw, legacy_schema, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "legacy setup failed";
            sqlite3_free(err);
            sqlite3_close(raw);
            Fail(msg);
        }
        sqlite3_close(raw);
    }

    {
        Database db(path);   // constructor runs the migration
        CHECK(db.IsOpen());
        CHECK_EQ(db.SchemaVersion(), 2);

        Patent p = db.GetPatentByCode("GK-OLD-1");
        CHECK_STR_EQ(p.title, "旧数据");
        // Prefixes migrated into structured columns
        CHECK_STR_EQ(p.technology_route, "图像传感器");
        CHECK_STR_EQ(p.agent_name, "刘代理");
        CHECK_STR_EQ(p.oa_reminder_1, "2025-03-01");
        // Non-prefix text stays in notes
        CHECK(p.notes.find("纯文本备注") != std::string::npos);
        CHECK(p.notes.find("技术路线") == std::string::npos);

        // A pre-migration backup must exist next to the db
        bool backup_found = false;
        for (auto& entry : std::filesystem::directory_iterator(
                 std::filesystem::path(path).parent_path())) {
            std::string name = entry.path().filename().string();
            if (name.find("pre_migration") != std::string::npos &&
                name.find("legacy") != std::string::npos) {
                backup_found = true;
                std::filesystem::remove(entry.path());
            }
        }
        CHECK(backup_found);
    }
    std::filesystem::remove(path);
}

TEST(database_query_filters) {
    std::string path = TempDbPath("filter");
    std::filesystem::remove(path);
    {
        Database db(path);
        Patent a = MakePatent();
        a.geke_code = "GK-F-1";
        a.title = "图像处理装置";
        a.geke_handler = "张三";
        a.application_status = "pending";
        db.InsertPatent(a);

        Patent b = MakePatent();
        b.geke_code = "GK-F-2";
        b.title = "通信方法";
        b.geke_handler = "李四";
        b.application_status = "granted";
        b.tags = "通信";
        db.InsertPatent(b);

        QueryFilter f;
        CHECK_EQ(db.GetPatents(f).size(), 2u);

        f.handler = "张三";
        CHECK_EQ(db.GetPatents(f).size(), 1u);

        f = QueryFilter{};
        f.keyword = "通信";
        auto hits = db.GetPatents(f);
        CHECK_EQ(hits.size(), 1u);
        CHECK_STR_EQ(hits[0].geke_code, "GK-F-2");

        // LIKE wildcards in the keyword must not widen the query
        f = QueryFilter{};
        f.keyword = "%";
        CHECK_EQ(db.GetPatents(f).size(), 0u);
    }
    std::filesystem::remove(path);
}

TEST(database_pct_software_ic_foreign_full_crud) {
    std::string path = TempDbPath("crud");
    std::filesystem::remove(path);
    {
        Database db(path);

        // PCT: insert + full update roundtrip
        PCTPatent pct;
        pct.geke_code = "PCT-1";
        pct.domestic_source = "GK-1";
        pct.application_no = "PCT/CN2025/123456";
        pct.country_app_no = "US17/999888";
        pct.title = "PCT标题";
        pct.application_status = "national phase";
        pct.handler = "张三";
        pct.inventor = "李四";
        pct.filing_date = "2025-02-01";
        pct.application_date = "2025-02-02";
        pct.priority_date = "2024-02-02";
        pct.country = "US";
        pct.notes = "note";
        int pct_id = db.InsertPCT(pct);
        CHECK(pct_id > 0);
        pct.title = "改后PCT";
        pct.inventor = "王五";
        pct.country_app_no = "EP2500011.1";
        CHECK(db.UpdatePCT(pct_id, pct));
        auto pct_out = db.GetPCTById(pct_id);
        CHECK_STR_EQ(pct_out.title, "改后PCT");
        CHECK_STR_EQ(pct_out.inventor, "王五");
        CHECK_STR_EQ(pct_out.country_app_no, "EP2500011.1");
        CHECK_STR_EQ(pct_out.priority_date, "2024-02-02");

        // Software: update (previously missing entirely)
        SoftwareCopyright sw;
        sw.case_no = "SW-1";
        sw.title = "软件A";
        sw.developer = "开发部";
        sw.version = "V2.0";
        sw.notes = "sw-note";
        int sw_id = db.InsertSoftware(sw);
        CHECK(sw_id > 0);
        sw.title = "软件A改";
        sw.version = "V3.0";
        sw.notes = "sw-note-2";
        CHECK(db.UpdateSoftware(sw_id, sw));
        auto sw_out = db.GetSoftwareById(sw_id);
        CHECK_STR_EQ(sw_out.title, "软件A改");
        CHECK_STR_EQ(sw_out.version, "V3.0");
        CHECK_STR_EQ(sw_out.notes, "sw-note-2");

        // IC: update roundtrip
        ICLayout ic;
        ic.case_no = "IC-1";
        ic.title = "布图A";
        ic.designer = "设计部";
        ic.creation_date = "2025-01-01";
        int ic_id = db.InsertIC(ic);
        CHECK(ic_id > 0);
        ic.title = "布图A改";
        ic.cert_date = "2025-08-01";
        CHECK(db.UpdateIC(ic_id, ic));
        auto ic_out = db.GetICById(ic_id);
        CHECK_STR_EQ(ic_out.title, "布图A改");
        CHECK_STR_EQ(ic_out.cert_date, "2025-08-01");

        // Foreign: full insert + update (previously partial insert only)
        ForeignPatent fp;
        fp.case_no = "FP-1";
        fp.country = "US";
        fp.application_no = "17248024";
        fp.title = "US case";
        fp.inventor = "John Doe";
        fp.application_date = "2021-01-05";
        fp.authorization_date = "2023-05-09";
        fp.notes = "fp-note";
        int fp_id = db.InsertForeign(fp);
        CHECK(fp_id > 0);
        fp.title = "US case amended";
        fp.inventor = "Jane Roe";
        fp.notes = "fp-note-2";
        CHECK(db.UpdateForeign(fp_id, fp));
        auto fp_out = db.GetForeignById(fp_id);
        CHECK_STR_EQ(fp_out.title, "US case amended");
        CHECK_STR_EQ(fp_out.inventor, "Jane Roe");
        CHECK_STR_EQ(fp_out.application_no, "17248024");
        CHECK_STR_EQ(fp_out.notes, "fp-note-2");

        // US case candidate lookup: unambiguous match by app number
        CHECK_EQ(db.FindUSCaseCandidate("17/248024"), fp_id);
        // Ambiguity must refuse auto-link
        ForeignPatent fp2 = fp;
        fp2.case_no = "FP-2";
        fp2.id = 0;
        db.InsertForeign(fp2);
        CHECK_EQ(db.FindUSCaseCandidate("17248024"), 0);
    }
    std::filesystem::remove(path);
}

TEST(database_oa_crud_and_external_link) {
    std::string path = TempDbPath("oa");
    std::filesystem::remove(path);
    {
        Database db(path);
        OARecord oa;
        oa.geke_code = "GK-1";
        oa.oa_type = "1-OA";
        oa.official_deadline = "2025-12-01";
        oa.issue_date = "2025-08-01";
        oa.handler = "张三";
        oa.writer = "李四";
        oa.progress = "drafting";
        oa.oa_summary = "创造性";
        oa.jurisdiction = "US";
        oa.source = "USPTO";
        oa.external_document_id = "LN4VBTHCXBLUEX2";
        oa.deadline_source = "calculated";
        int id = db.InsertOA(oa);
        CHECK(id > 0);

        auto out = db.GetOAById(id);
        CHECK_STR_EQ(out.oa_summary, "创造性");
        CHECK_STR_EQ(out.jurisdiction, "US");
        CHECK_STR_EQ(out.external_document_id, "LN4VBTHCXBLUEX2");
        CHECK_STR_EQ(out.deadline_source, "calculated");

        CHECK_EQ(db.FindOAByExternalDocument("LN4VBTHCXBLUEX2"), id);
        CHECK_EQ(db.FindOAByExternalDocument("missing"), 0);

        // Filter by deadline state
        QueryFilter f;
        f.deadline_state = "incomplete";
        CHECK_EQ(db.GetOARecords(f).size(), 1u);
        db.MarkOACompleted(id);
        f.deadline_state = "completed";
        CHECK_EQ(db.GetOARecords(f).size(), 1u);
    }
    std::filesystem::remove(path);
}

TEST(database_deadline_rules) {
    std::string path = TempDbPath("rules");
    std::filesystem::remove(path);
    {
        Database db(path);
        auto rules = db.GetDeadlineRules();
        CHECK(rules.size() >= 5);   // seeded defaults

        // CN invention OA: 4 months from issue date
        std::string d1 = db.CalculateDeadline("CN", "oa_response_invention", "2025-01-31");
        CHECK_STR_EQ(d1, "2025-05-31");

        // US OA: 3 months
        std::string d2 = db.CalculateDeadline("US", "oa_response", "2025-03-10");
        CHECK_STR_EQ(d2, "2025-06-10");

        // Unknown jurisdiction: no suggestion
        CHECK_STR_EQ(db.CalculateDeadline("XX", "whatever", "2025-01-01"), "");

        // User-defined rule CRUD
        DeadlineRule custom;
        custom.jurisdiction = "EP";
        custom.event_type = "oa_response";
        custom.base_months = 4;
        int rule_id = 0;
        CHECK(db.InsertDeadlineRule(custom, &rule_id));
        CHECK(rule_id > 0);
        std::string d3 = db.CalculateDeadline("EP", "oa_response", "2025-01-01");
        CHECK_STR_EQ(d3, "2025-05-01");

        custom.id = rule_id;
        custom.base_months = 6;
        CHECK(db.UpdateDeadlineRule(custom));
        std::string d4 = db.CalculateDeadline("EP", "oa_response", "2025-01-01");
        CHECK_STR_EQ(d4, "2025-07-01");

        CHECK(db.DeleteDeadlineRule(rule_id));
        CHECK_STR_EQ(db.CalculateDeadline("EP", "oa_response", "2025-01-01"), "");
    }
    std::filesystem::remove(path);
}

TEST(database_backup_api) {
    std::string path = TempDbPath("backup");
    std::string dest = TempDbPath("backup_dest");
    std::filesystem::remove(path);
    std::filesystem::remove(dest);
    {
        Database db(path);
        Patent p = MakePatent();
        db.InsertPatent(p);
        CHECK(db.BackupTo(dest));
    }
    {
        Database restored(dest);
        Patent p = restored.GetPatentByCode("GK-TEST-001");
        CHECK_STR_EQ(p.title, "测试专利");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(dest);
}
