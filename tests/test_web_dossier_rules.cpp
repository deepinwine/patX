// Offline tests for the web-dossier OA merge rules and DB layer. No wx, no
// network: everything runs against a throwaway SQLite file.
#include "database.hpp"
#include "web_dossier.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <process.h>
#define GETPID _getpid
#else
#include <unistd.h>
#define GETPID getpid
#endif

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  "    \
                      << #cond << std::endl;                               \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_STR_EQ(a, b)                                                 \
    do {                                                                   \
        if ((a) != (b)) {                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  '"   \
                      << (a) << "' != '" << (b) << "'" << std::endl;       \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static std::string TempDb() {
    auto dir = std::filesystem::temp_directory_path();
    static int counter = 0;
    return (dir / ("patx_wd_test_" + std::to_string(++counter) + "_" +
                   std::to_string(GETPID()) + ".db"))
        .string();
}

using namespace webdossier;

// ---------------------------------------------------------------------------
// OA type normalization
// ---------------------------------------------------------------------------

static void TestNormalizeOaType() {
    CHECK_STR_EQ(NormalizeOaTypeCn("第一次审查意见通知书"), "第一次审查意见通知书");
    CHECK_STR_EQ(NormalizeOaTypeCn("第二次审查意见通知书"), "第二次审查意见通知书");
    CHECK_STR_EQ(NormalizeOaTypeCn("第3次审查意见通知书"), "第三次审查意见通知书");
    CHECK_STR_EQ(NormalizeOaTypeCn("第１０次审查意见通知书"), "第十次审查意见通知书");
    CHECK_STR_EQ(NormalizeOaTypeCn("二通"), "第二次审查意见通知书");
    CHECK_STR_EQ(NormalizeOaTypeCn("审查意见通知书"), "审查意见通知书");
    CHECK_STR_EQ(NormalizeOaTypeCn("第二次 审查意见 通知书"), "第二次审查意见通知书");
    // Non-OA titles come back untouched
    CHECK_STR_EQ(NormalizeOaTypeCn("驳回决定"), "驳回决定");
    CHECK_STR_EQ(NormalizeOaTypeCn("意见陈述书"), "意见陈述书");

    CHECK(IsOfficeActionTypeCn(NormalizeOaTypeCn("二通")));
    CHECK(!IsOfficeActionTypeCn(NormalizeOaTypeCn("授权通知")));
    CHECK(OaTypeOrdinalCn("第3次审查意见通知书") == 3);
    CHECK(OaTypeOrdinalCn("驳回决定") == 0);
}

// ---------------------------------------------------------------------------
// DB layer: migration, queue filter, fingerprint dedup, OA protections
// ---------------------------------------------------------------------------

static void TestQueueFilter() {
    std::string path = TempDb();
    {
        Database db(path);
        Patent active;
        active.geke_code = "GC-1001";
        active.application_status = "实质审查中";
        active.application_number = "202410123457.5";
        db.InsertPatent(active, false);

        Patent dead;
        dead.geke_code = "GC-1002";
        dead.application_status = "已放弃";
        db.InsertPatent(dead, false);

        Patent granted;
        granted.geke_code = "GC-1003";
        granted.application_status = "授权公告";
        db.InsertPatent(granted, false);

        Patent waiting;
        waiting.geke_code = "GC-1004";
        waiting.application_status = "等待审查";
        db.InsertPatent(waiting, false);

        auto active_cases = db.GetPatentsForDossierCheck(false);
        CHECK(active_cases.size() == 2);   // 1001 + 1004
        bool has_1001 = false, has_1004 = false, has_dead = false;
        for (const auto& p : active_cases) {
            if (p.geke_code == "GC-1001") has_1001 = true;
            if (p.geke_code == "GC-1004") has_1004 = true;
            if (p.geke_code == "GC-1002" || p.geke_code == "GC-1003") has_dead = true;
        }
        CHECK(has_1001 && has_1004 && !has_dead);

        auto incl_granted = db.GetPatentsForDossierCheck(true);
        CHECK(incl_granted.size() == 3);   // + granted
    }
    std::filesystem::remove(path);
}

static void TestFingerprintDedup() {
    std::string path = TempDb();
    {
        Database db(path);
        Patent p;
        p.geke_code = "GC-1784";
        db.InsertPatent(p, false);

        ProsecutionDocumentRecord doc;
        doc.patent_id = 1;
        doc.jurisdiction = "CN";
        doc.application_number = "202410123457.5";
        doc.source = "cnipa";
        doc.document_type = "OFFICE_ACTION_SECOND";
        doc.document_title = "第二次审查意见通知书";
        doc.official_date = "2026-09-18";
        doc.fingerprint = "fp-abc";
        bool created = false;
        int id1 = db.UpsertProsecutionDocument(doc, &created);
        CHECK(id1 > 0 && created);

        // Same fingerprint again -> same row, only last_seen refreshed
        bool created2 = true;
        int id2 = db.UpsertProsecutionDocument(doc, &created2);
        CHECK(id2 == id1 && !created2);

        // Different date -> new row
        doc.official_date = "2026-03-12";
        doc.fingerprint = "fp-def";
        int id3 = db.UpsertProsecutionDocument(doc, &created);
        CHECK(id3 > 0 && id3 != id1 && created);

        // Bookkeeping columns
        CHECK(db.UpdatePatentDossierCheck(1, 100, 200));

        DossierSyncState state;
        state.patent_id = 1;
        state.provider = "cnipa";
        state.last_checked_at = 100;
        state.last_success_at = 100;
        state.latest_remote_oa_date = "2026-09-18";
        state.latest_remote_oa_type = "第二次审查意见通知书";
        state.auth_state = "AUTHENTICATED";
        CHECK(db.UpsertDossierSyncState(state));
        state.last_error_code = "AUTH_REQUIRED";
        state.last_error_at = 200;
        CHECK(db.UpsertDossierSyncState(state));   // upsert overwrites
        auto states = db.GetDossierSyncStates();
        CHECK(states.size() == 1);
        CHECK_STR_EQ(states[0].last_error_code, "AUTH_REQUIRED");
        CHECK_STR_EQ(states[0].latest_remote_oa_date, "2026-09-18");
    }
    std::filesystem::remove(path);
}

// The core merge rules, exercised exactly like Manager::ApplyRemoteResult.
static void TestOaMergeRules() {
    std::string path = TempDb();
    {
        Database db(path);
        Patent p;
        p.geke_code = "GC-1784";
        p.title = "图像处理装置";
        db.InsertPatent(p, false);

        // OA1 exists with a HUMAN date and handler data that must survive.
        OARecord oa1;
        oa1.patent_id = 1;
        oa1.geke_code = "GC-1784";
        oa1.oa_type = "第一次审查意见通知书";
        oa1.issue_date = "2026-03-12";
        oa1.handler = "张三";
        oa1.writer = "李四";
        oa1.oa_summary = "人工摘要";
        oa1.official_deadline = "2026-06-10";
        int oa1_id = db.InsertOA(oa1, false);
        CHECK(oa1_id > 0);

        // --- Rule A: new remote OA (二通) -> INSERT, never touch OA1 ---
        {
            auto existing = db.GetOAsForPatentId(1);
            CHECK(existing.size() == 1);
            std::string canonical = NormalizeOaTypeCn("第二次审查意见通知书");
            const OARecord* same_type = nullptr;
            const OARecord* same_date = nullptr;
            for (const auto& e : existing) {
                if (NormalizeOaTypeCn(e.oa_type) == canonical) same_type = &e;
                if (e.issue_date == "2026-09-18") same_date = &e;
            }
            CHECK(same_type == nullptr);   // no local 二通 yet
            CHECK(same_date == nullptr);
            OARecord oa2;
            oa2.patent_id = 1;
            oa2.geke_code = "GC-1784";
            oa2.oa_type = canonical;
            oa2.issue_date = "2026-09-18";
            oa2.source = "cnipa";
            oa2.sync_flag = "web_new";
            int oa2_id = db.InsertOA(oa2, false);
            CHECK(oa2_id > oa1_id);
        }
        auto after_insert = db.GetOAsForPatentId(1);
        CHECK(after_insert.size() == 2);

        // --- Rule B: same type, same date -> NO_CHANGE (no duplicate) ---
        {
            std::string canonical = NormalizeOaTypeCn("第二次审查意见通知书");
            OARecord same_type_copy;
            bool found = false;
            auto fetched = db.GetOAsForPatentId(1);   // own the vector
            for (const auto& e : fetched) {
                if (NormalizeOaTypeCn(e.oa_type) == canonical) {
                    same_type_copy = e;
                    found = true;
                }
            }
            CHECK(found);
            CHECK_STR_EQ(same_type_copy.issue_date, "2026-09-18");
        }

        // --- Rule C: OA with EMPTY date gets it filled, nothing else ---
        OARecord oa3;
        oa3.patent_id = 1;
        oa3.oa_type = "第三次审查意见通知书";   // stored without a date
        int oa3_id = db.InsertOA(oa3, false);
        CHECK(db.UpdateOASyncFields(oa3_id, "2026-10-01", "auto_filled_date"));
        {
            auto fetched = db.GetOAsForPatentId(1);
            for (const auto& e : fetched) {
                if (e.id == oa3_id) {
                    CHECK_STR_EQ(e.issue_date, "2026-10-01");
                    CHECK_STR_EQ(e.sync_flag, "auto_filled_date");
                }
            }
        }

        // --- Rule D: same type, DIFFERENT date -> flag only, never overwrite
        // (local 三通 now has 2026-10-01; remote says 2026-11-05) ---
        CHECK(db.UpdateOASyncFields(oa3_id, "", "date_conflict"));
        {
            auto fetched = db.GetOAsForPatentId(1);
            for (const auto& e : fetched) {
                if (e.id == oa3_id) {
                    CHECK_STR_EQ(e.issue_date, "2026-10-01");   // unchanged!
                    CHECK_STR_EQ(e.sync_flag, "date_conflict");
                }
            }
        }

        // --- Fill-date is a no-op when a date already exists ---
        CHECK(db.UpdateOASyncFields(oa1_id, "2030-01-01", ""));
        auto final_records = db.GetOAsForPatentId(1);
        for (const auto& e : final_records) {
            if (e.id == oa1_id) {
                CHECK_STR_EQ(e.issue_date, "2026-03-12");       // human data intact
                CHECK_STR_EQ(e.handler, "张三");
                CHECK_STR_EQ(e.oa_summary, "人工摘要");
                CHECK_STR_EQ(e.official_deadline, "2026-06-10");
            }
        }
    }
    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// Official-event merge (OA + rejection/grant/correction/other)
// ---------------------------------------------------------------------------

static void TestOfficialEventTitleCn() {
    RemoteDocument oa;
    oa.document_type = "OFFICE_ACTION_FIRST";
    oa.document_title = "第一次审查意见通知书";
    CHECK_STR_EQ(OfficialEventTitleCn(oa), "第一次审查意见通知书");

    RemoteDocument rejection;
    rejection.document_type = "REJECTION_DECISION";
    CHECK_STR_EQ(OfficialEventTitleCn(rejection), "驳回决定");

    RemoteDocument grant;
    grant.document_type = "GRANT_NOTICE";
    CHECK_STR_EQ(OfficialEventTitleCn(grant), "授权通知");

    RemoteDocument correction;
    correction.document_type = "CORRECTION_NOTICE";
    CHECK_STR_EQ(OfficialEventTitleCn(correction), "补正通知");

    RemoteDocument other;
    other.document_type = "OTHER_OFFICIAL";
    other.document_title = "费用减缴审批通知书";
    CHECK_STR_EQ(OfficialEventTitleCn(other), "费用减缴审批通知书");
    other.document_title = "";
    CHECK_STR_EQ(OfficialEventTitleCn(other), "其他官方通知");
}

static void TestMergeOfficialEvent() {
    std::string path = TempDb();
    {
        Database db(path);
        Patent p;
        p.geke_code = "GC-270";
        p.title = "图像处理装置";
        p.application_status = "实质审查中";
        p.application_number = "CN201510375387.3";
        p.id = db.InsertPatent(p, false);
        CHECK(p.id > 0);

        // --- grant event: creates an OA row, never touches application_status
        RemoteDocument grant;
        grant.source = "epo_global_dossier";
        grant.document_type = "GRANT_NOTICE";
        grant.document_title = "授权通知";
        grant.raw_title = "Notification to grant patent right (ORIGINAL)";
        grant.official_date = "2026-08-10";
        grant.remote_document_id = "epo-grant-1";
        grant.event_key = "grant-event";
        grant.confidence = "HIGH";
        auto merged = MergeOfficialEvent(db, p, grant);
        CHECK(merged.code == ResultCode::NewOfficialEvent);
        CHECK(merged.oa_created_id > 0);
        {
            auto oas = db.GetOAsForPatentId(p.id);
            CHECK(oas.size() == 1);
            CHECK_STR_EQ(oas.back().oa_type, "授权通知");
            CHECK_STR_EQ(oas.back().issue_date, "2026-08-10");
            CHECK_STR_EQ(oas.back().source, "epo_global_dossier");
            // non-OA events get no suggested deadline
            CHECK_STR_EQ(oas.back().official_deadline, "");
        }
        Patent stored = db.GetPatentById(p.id);
        CHECK_STR_EQ(stored.application_status, "实质审查中");

        // --- same event from another source: same event_key -> NO duplicate
        auto again = MergeOfficialEvent(db, p, grant);
        CHECK(again.code == ResultCode::NoChange);
        CHECK(db.GetOAsForPatentId(p.id).size() == 1);

        // --- rejection: another new event type
        RemoteDocument rejection;
        rejection.source = "uspto_global_dossier";
        rejection.document_type = "REJECTION_DECISION";
        rejection.document_title = "驳回决定";
        rejection.official_date = "2026-09-01";
        rejection.event_key = "reject-event";
        rejection.confidence = "HIGH";
        auto rej = MergeOfficialEvent(db, p, rejection);
        CHECK(rej.code == ResultCode::NewOfficialEvent);
        CHECK(db.GetOAsForPatentId(p.id).size() == 2);

        // --- OA with a human record: date conflict never overwrites
        OARecord human;
        human.patent_id = p.id;
        human.geke_code = "GC-270";
        human.oa_type = "第一次审查意见通知书";
        human.issue_date = "2026-05-23";
        human.handler = "张三";
        human.oa_summary = "人工摘要";
        int human_id = db.InsertOA(human, false);
        CHECK(human_id > 0);

        RemoteDocument oa_remote;
        oa_remote.source = "uspto_global_dossier";
        oa_remote.document_type = "OFFICE_ACTION_FIRST";
        oa_remote.document_title = "第一次审查意见通知书";
        oa_remote.official_date = "2026-06-01";     // different from local
        oa_remote.event_key = "oa1-event";
        oa_remote.confidence = "HIGH";
        auto conflict = MergeOfficialEvent(db, p, oa_remote);
        CHECK(conflict.code == ResultCode::DateConflict);
        CHECK(conflict.date_conflict);
        {
            auto stored_oa = db.GetOAById(human_id);
            CHECK_STR_EQ(stored_oa.issue_date, "2026-05-23");   // untouched
            CHECK_STR_EQ(stored_oa.handler, "张三");
            CHECK_STR_EQ(stored_oa.oa_summary, "人工摘要");
            CHECK_STR_EQ(stored_oa.sync_flag, "date_conflict");
        }

        // --- OA record without a date gets it filled automatically, plus a
        // calculated response deadline from the rule engine (invention: 4mo)
        OARecord undated;
        undated.patent_id = p.id;
        undated.oa_type = "第二次审查意见通知书";
        int undated_id = db.InsertOA(undated, false);
        RemoteDocument oa2;
        oa2.document_type = "OFFICE_ACTION_SECOND";
        oa2.document_title = "第二次审查意见通知书";
        oa2.official_date = "2026-07-15";
        oa2.event_key = "oa2-event";
        oa2.confidence = "HIGH";
        auto filled = MergeOfficialEvent(db, p, oa2);
        CHECK(filled.code == ResultCode::NoChange);
        auto stored2 = db.GetOAById(undated_id);
        CHECK_STR_EQ(stored2.issue_date, "2026-07-15");
        CHECK_STR_EQ(stored2.sync_flag, "auto_filled_date");
        CHECK_STR_EQ(stored2.official_deadline, "2026-11-15");   // +4 months
        CHECK_STR_EQ(stored2.deadline_source, "calculated");

        // --- a human deadline is never touched by the suggested fill
        OARecord manual;
        manual.patent_id = p.id;
        manual.oa_type = "第一次审查意见通知书";
        manual.official_deadline = "2030-01-01";
        manual.deadline_source = "manual";
        int manual_id = db.InsertOA(manual, false);
        CHECK(!db.FillOADeadlineIfEmpty(manual_id, "2027-01-01"));
        auto manual_stored = db.GetOAById(manual_id);
        CHECK_STR_EQ(manual_stored.official_deadline, "2030-01-01");
        CHECK_STR_EQ(manual_stored.deadline_source, "manual");

        // --- utility-model patents use the 2-month rule
        Patent util;
        util.geke_code = "GC-UTIL";
        util.patent_type = "utility";
        util.application_status = "实质审查中";
        util.id = db.InsertPatent(util, false);
        RemoteDocument util_oa;
        util_oa.document_type = "OFFICE_ACTION_FIRST";
        util_oa.document_title = "第一次审查意见通知书";
        util_oa.official_date = "2026-09-20";
        util_oa.event_key = "util-oa-event";
        util_oa.confidence = "HIGH";
        auto utilres = MergeOfficialEvent(db, util, util_oa);
        CHECK(utilres.code == ResultCode::NewOfficialEvent);
        auto util_stored = db.GetOAById(utilres.oa_created_id);
        CHECK_STR_EQ(util_stored.official_deadline, "2026-11-20");   // +2 months
        CHECK_STR_EQ(util_stored.deadline_source, "calculated");

        // --- LOW confidence never writes
        RemoteDocument low;
        low.document_type = "GRANT_NOTICE";
        low.document_title = "授权通知";
        low.official_date = "2026-09-20";
        low.confidence = "LOW";
        auto lowres = MergeOfficialEvent(db, p, low);
        CHECK(lowres.code == ResultCode::ManualReviewRequired);
        CHECK(lowres.oa_created_id == 0);

        // --- missing date never writes
        RemoteDocument undated_remote;
        undated_remote.document_type = "CORRECTION_NOTICE";
        undated_remote.document_title = "补正通知";
        undated_remote.official_date = "";
        undated_remote.confidence = "HIGH";
        auto nores = MergeOfficialEvent(db, p, undated_remote);
        CHECK(nores.code == ResultCode::DateParseFailed);
    }
    std::filesystem::remove(path);
}

int main() {
    TestNormalizeOaType();
    TestQueueFilter();
    TestFingerprintDedup();
    TestOaMergeRules();
    TestOfficialEventTitleCn();
    TestMergeOfficialEvent();
    if (g_failures == 0) {
        std::cout << "all web dossier rule tests passed" << std::endl;
        return 0;
    }
    std::cout << g_failures << " failure(s)" << std::endl;
    return 1;
}
