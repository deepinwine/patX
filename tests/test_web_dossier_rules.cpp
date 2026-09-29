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

static void TestDueQueueFilter() {
    std::string path = TempDb();
    {
        Database db(path);
        auto add = [&](const std::string& code, const std::string& app,
                       const std::string& publication, const std::string& status,
                       long long next_check) {
            Patent p;
            p.geke_code = code;
            p.application_number = app;
            p.publication_number = publication;
            p.application_status = status;
            int id = db.InsertPatent(p, false);
            CHECK(id > 0);
            CHECK(db.Execute("UPDATE patents SET next_dossier_check_at = " +
                             std::to_string(next_check) + " WHERE id = " +
                             std::to_string(id)));
            return id;
        };

        add("DUE-CN-PREFIX", "CN202410000001.1", "", "实质审查中", 1000);
        add("DUE-NUMERIC", "202410000002.2", "", "等待审查", 0);
        int null_due_id = add("DUE-NULL-NEXT", "202410000006.6", "", "pending", 0);
        CHECK(db.Execute("UPDATE patents SET next_dossier_check_at = NULL, "
                         "application_status = NULL WHERE id = " +
                         std::to_string(null_due_id)));
        add("FUTURE", "202410000003.3", "", "实质审查中", 1001);
        add("DUE-X", "202410123457X", "", "实质审查中", 800);
        add("DUE-BARE-12", "202410123458", "", "实质审查中", 850);
        add("DUE-PUBLICATION", "", "CN119870049A", "实质审查中", 900);
        add("NON-CN", "US17/123456", "US2025000001A1", "pending", 0);
        add("EMPTY-ID", "", "", "pending", 0);
        add("BAD-APP-8", "12345678", "", "pending", -9);
        add("BAD-APP-10", "1234567890", "", "pending", -8);
        add("BAD-APP-11", "12345678901", "", "pending", -7);
        add("BAD-APP-13", "12345678901X2", "", "pending", -6);
        add("BAD-APP-14", "12345678901234", "", "pending", -5);
        add("BAD-PUB-LENGTH", "", "CN12345678A", "pending", -4);
        add("BAD-PUB-KIND", "", "CN119870049C", "pending", -3);
        add("BAD-MIXED", "123US/FOO", "", "pending", -2);
        add("BAD-SUFFIX", "9XYZ", "", "pending", -1);
        add("BAD-PUNCT", ".", "", "pending", 0);
        add("GRANTED", "202410000004.4", "", "授权公告", 0);
        add("TERMINAL", "202410000005.5", "", "已放弃", 0);

        auto due = db.GetPatentsDueForDossierCheck(false, 1000, 0);
        CHECK(due.size() == 6);
        CHECK_STR_EQ(due[0].geke_code, "DUE-NUMERIC");
        CHECK_STR_EQ(due[1].geke_code, "DUE-NULL-NEXT");
        CHECK_STR_EQ(due[2].geke_code, "DUE-X");
        CHECK_STR_EQ(due[3].geke_code, "DUE-BARE-12");
        CHECK_STR_EQ(due[4].geke_code, "DUE-PUBLICATION");
        CHECK_STR_EQ(due[5].geke_code, "DUE-CN-PREFIX");

        auto limited = db.GetPatentsDueForDossierCheck(false, 1000, 2);
        CHECK(limited.size() == 2);
        CHECK_STR_EQ(limited[0].geke_code, "DUE-NUMERIC");
        CHECK_STR_EQ(limited[1].geke_code, "DUE-NULL-NEXT");

        auto first_valid = db.GetPatentsDueForDossierCheck(false, 1000, 1);
        CHECK(first_valid.size() == 1);
        CHECK_STR_EQ(first_valid[0].geke_code, "DUE-NUMERIC");

        auto with_granted = db.GetPatentsDueForDossierCheck(true, 1000, 0);
        CHECK(with_granted.size() == 7);
        bool found_granted = false;
        for (const auto& p : with_granted) {
            if (p.geke_code == "GRANTED") found_granted = true;
            CHECK(p.geke_code != "NON-CN");
            CHECK(p.geke_code != "EMPTY-ID");
            CHECK(p.geke_code != "FUTURE");
            CHECK(p.geke_code != "TERMINAL");
            CHECK(p.geke_code != "BAD-MIXED");
            CHECK(p.geke_code != "BAD-SUFFIX");
            CHECK(p.geke_code != "BAD-PUNCT");
            CHECK(p.geke_code.find("BAD-APP-") != 0);
            CHECK(p.geke_code.find("BAD-PUB-") != 0);
        }
        CHECK(found_granted);
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

static void TestEventKeyDedupAndDocumentRoundtrip() {
    std::string path = TempDb();
    {
        Database db(path);

        ProsecutionDocumentRecord first;
        first.patent_id = 11;
        first.jurisdiction = "US";
        first.application_number = "US17123456";
        first.publication_number = "US2025000001A1";
        first.source = "USPTO";
        first.remote_document_id = "us-remote-1";
        first.document_type = "OFFICE_ACTION";
        first.document_title = "Normalized title";
        first.raw_title = "Non-Final Rejection";
        first.document_code = "CTNF";
        first.document_version = "ORIGINAL";
        first.official_date = "2026-09-01";
        first.direction = "official";
        first.source_url = "https://example.test/us/source";
        first.download_url = "https://example.test/us/download";
        first.download_available = true;
        first.fingerprint = "us-fingerprint";
        first.event_key = "oa:2026-09-01:non-final";
        first.source_trace = " uspto, legacy , uspto,  ";
        first.raw_metadata = "{\"provider\":\"uspto\"}";

        bool created = false;
        int first_id = db.UpsertProsecutionDocument(first, &created);
        CHECK(first_id > 0);
        CHECK(created);

        ProsecutionDocumentRecord stored = db.GetProsecutionDocumentById(first_id);
        CHECK(stored.id == first_id);
        CHECK(stored.patent_id == 11);
        CHECK_STR_EQ(stored.jurisdiction, "US");
        CHECK_STR_EQ(stored.application_number, "US17123456");
        CHECK_STR_EQ(stored.publication_number, "US2025000001A1");
        CHECK_STR_EQ(stored.source, "USPTO");
        CHECK_STR_EQ(stored.remote_document_id, "us-remote-1");
        CHECK_STR_EQ(stored.document_type, "OFFICE_ACTION");
        CHECK_STR_EQ(stored.document_title, "Normalized title");
        CHECK_STR_EQ(stored.raw_title, "Non-Final Rejection");
        CHECK_STR_EQ(stored.document_code, "CTNF");
        CHECK_STR_EQ(stored.document_version, "ORIGINAL");
        CHECK_STR_EQ(stored.official_date, "2026-09-01");
        CHECK_STR_EQ(stored.direction, "official");
        CHECK_STR_EQ(stored.source_url, "https://example.test/us/source");
        CHECK_STR_EQ(stored.download_url, "https://example.test/us/download");
        CHECK(stored.download_available);
        CHECK_STR_EQ(stored.fingerprint, "us-fingerprint");
        CHECK_STR_EQ(stored.event_key, "oa:2026-09-01:non-final");
        CHECK_STR_EQ(stored.source_trace, "uspto,legacy");
        CHECK_STR_EQ(stored.raw_metadata, "{\"provider\":\"uspto\"}");
        CHECK(stored.first_seen_at > 0);
        CHECK(stored.last_seen_at >= stored.first_seen_at);

        CHECK(db.Execute("UPDATE prosecution_documents SET last_seen_at = 1 WHERE id = " +
                         std::to_string(first_id)));
        ProsecutionDocumentRecord same_event = first;
        same_event.source = "EPO";
        same_event.source_trace = "LEGACY, epo, Partner, EPO";
        same_event.remote_document_id = "epo-remote-should-not-win";
        same_event.document_title = "EPO semantic title must not win";
        same_event.official_date = "2030-01-01";
        same_event.raw_metadata = "{\"provider\":\"epo\"}";
        bool duplicate_created = true;
        int duplicate_id = db.UpsertProsecutionDocument(same_event, &duplicate_created);
        CHECK(duplicate_id == first_id);
        CHECK(!duplicate_created);

        ProsecutionDocumentRecord merged = db.GetProsecutionDocumentById(first_id);
        CHECK_STR_EQ(merged.source, "USPTO");
        CHECK_STR_EQ(merged.remote_document_id, "us-remote-1");
        CHECK_STR_EQ(merged.document_title, "Normalized title");
        CHECK_STR_EQ(merged.official_date, "2026-09-01");
        CHECK_STR_EQ(merged.source_trace, "uspto,legacy,epo,partner");
        CHECK_STR_EQ(merged.raw_metadata, "{\"provider\":\"epo\"}");
        CHECK(merged.last_seen_at > 1);
        CHECK(merged.first_seen_at == stored.first_seen_at);

        bool repeated_created = true;
        CHECK(db.UpsertProsecutionDocument(same_event, &repeated_created) == first_id);
        CHECK(!repeated_created);
        CHECK_STR_EQ(db.GetProsecutionDocumentById(first_id).source_trace,
                     "uspto,legacy,epo,partner");

        ProsecutionDocumentRecord other_patent = same_event;
        other_patent.patent_id = 12;
        bool other_created = false;
        int other_id = db.UpsertProsecutionDocument(other_patent, &other_created);
        CHECK(other_id > 0 && other_id != first_id);
        CHECK(other_created);

        ProsecutionDocumentRecord legacy;
        legacy.patent_id = 11;
        legacy.source = "cnipa";
        legacy.application_number = "202410000001.1";
        legacy.fingerprint = "legacy-fingerprint";
        legacy.raw_metadata = "old";
        bool legacy_created = false;
        int legacy_id = db.UpsertProsecutionDocument(legacy, &legacy_created);
        CHECK(legacy_id > 0 && legacy_created);
        CHECK_STR_EQ(db.GetProsecutionDocumentById(legacy_id).source_trace, "cnipa");
        legacy.raw_metadata = "refreshed";
        bool legacy_duplicate_created = true;
        CHECK(db.UpsertProsecutionDocument(legacy, &legacy_duplicate_created) == legacy_id);
        CHECK(!legacy_duplicate_created);
        CHECK_STR_EQ(db.GetProsecutionDocumentById(legacy_id).raw_metadata, "refreshed");

        ProsecutionDocumentRecord other_source = legacy;
        other_source.source = "epo";
        bool other_source_created = false;
        CHECK(db.UpsertProsecutionDocument(other_source, &other_source_created) > 0);
        CHECK(other_source_created);

        ProsecutionDocumentRecord invalid_trace;
        invalid_trace.patent_id = 55;
        invalid_trace.source = "bad,source";
        invalid_trace.application_number = "trace-app";
        invalid_trace.fingerprint = "trace-fp";
        invalid_trace.source_trace = " GOOD.ONE, good.one, bad token, x@y, valid_1 ";
        bool invalid_trace_created = false;
        int invalid_trace_id = db.UpsertProsecutionDocument(invalid_trace, &invalid_trace_created);
        CHECK(invalid_trace_id > 0 && invalid_trace_created);
        CHECK_STR_EQ(db.GetProsecutionDocumentById(invalid_trace_id).source_trace,
                     "good.one,valid_1");

        CHECK(db.GetProsecutionDocumentById(999999).id == 0);
    }
    std::filesystem::remove(path);
}

static void TestLegacyEventKeyPromotion() {
    std::string path = TempDb();
    {
        Database db(path);

        ProsecutionDocumentRecord legacy;
        legacy.patent_id = 21;
        legacy.source = "uspto";
        legacy.application_number = "US17111111";
        legacy.fingerprint = "legacy-promotion-fp";
        legacy.remote_document_id = "legacy-remote";
        legacy.document_title = "Legacy semantic title";
        legacy.official_date = "2026-08-01";
        legacy.raw_metadata = "old metadata";
        bool created = false;
        int legacy_id = db.UpsertProsecutionDocument(legacy, &created);
        CHECK(legacy_id > 0 && created);
        CHECK(db.Execute("UPDATE prosecution_documents SET last_seen_at = 1 WHERE id = " +
                         std::to_string(legacy_id)));

        ProsecutionDocumentRecord promoted = legacy;
        promoted.event_key = "oa:2026-08-01:non-final";
        promoted.source_trace = " uspto, mirror, uspto ";
        promoted.remote_document_id = "must-not-overwrite";
        promoted.document_title = "Must not overwrite legacy semantics";
        promoted.official_date = "2030-01-01";
        promoted.raw_metadata = "promoted metadata";
        bool promoted_created = true;
        int promoted_id = db.UpsertProsecutionDocument(promoted, &promoted_created);
        CHECK(promoted_id == legacy_id);
        CHECK(!promoted_created);

        auto upgraded = db.GetProsecutionDocumentById(legacy_id);
        CHECK_STR_EQ(upgraded.event_key, "oa:2026-08-01:non-final");
        CHECK_STR_EQ(upgraded.source_trace, "uspto,mirror");
        CHECK_STR_EQ(upgraded.raw_metadata, "promoted metadata");
        CHECK(upgraded.last_seen_at > 1);
        CHECK_STR_EQ(upgraded.remote_document_id, "legacy-remote");
        CHECK_STR_EQ(upgraded.document_title, "Legacy semantic title");
        CHECK_STR_EQ(upgraded.official_date, "2026-08-01");

        ProsecutionDocumentRecord conflict_legacy;
        conflict_legacy.patent_id = 31;
        conflict_legacy.source = "uspto";
        conflict_legacy.application_number = "US17222222";
        conflict_legacy.fingerprint = "conflict-legacy-fp";
        conflict_legacy.document_title = "Legacy row";
        bool conflict_legacy_created = false;
        int conflict_legacy_id =
            db.UpsertProsecutionDocument(conflict_legacy, &conflict_legacy_created);
        CHECK(conflict_legacy_id > 0 && conflict_legacy_created);

        ProsecutionDocumentRecord event_row;
        event_row.patent_id = 31;
        event_row.source = "epo";
        event_row.application_number = "EP-event-row";
        event_row.fingerprint = "conflict-event-fp";
        event_row.event_key = "shared-event-key";
        event_row.document_title = "Existing event semantics";
        event_row.source_trace = "epo";
        event_row.raw_metadata = "event metadata";
        bool event_created = false;
        int event_id = db.UpsertProsecutionDocument(event_row, &event_created);
        CHECK(event_id > 0 && event_created && event_id != conflict_legacy_id);

        ProsecutionDocumentRecord competing = conflict_legacy;
        competing.event_key = "shared-event-key";
        competing.source_trace = " archive, uspto ";
        competing.raw_metadata = "merged metadata";
        competing.document_title = "Must not overwrite event semantics";
        bool competing_created = true;
        int merged_id = db.UpsertProsecutionDocument(competing, &competing_created);
        CHECK(merged_id == event_id);
        CHECK(!competing_created);

        auto merged = db.GetProsecutionDocumentById(event_id);
        CHECK_STR_EQ(merged.document_title, "Existing event semantics");
        CHECK_STR_EQ(merged.raw_metadata, "merged metadata");
        CHECK_STR_EQ(merged.source_trace, "epo,uspto,archive");
        CHECK_STR_EQ(db.GetProsecutionDocumentById(conflict_legacy_id).event_key, "");
    }
    std::filesystem::remove(path);
}

static void TestCanonicalLegacyRowsAreMergedTransactionally() {
    std::string path = TempDb();
    {
        Database db(path);
        auto count_rows = [&]() {
            sqlite3_stmt* stmt = nullptr;
            int count = -1;
            if (sqlite3_prepare_v2(db.GetHandle(),
                    "SELECT COUNT(*) FROM prosecution_documents WHERE patent_id=71",
                    -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
                count = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
            return count;
        };

        ProsecutionDocumentRecord legacy;
        legacy.patent_id = 71;
        legacy.source = "USPTO";
        legacy.application_number = "US17333333";
        legacy.fingerprint = "merge-legacy-fp";
        legacy.document_title = "Legacy semantics";
        legacy.download_url = "https://legacy/download";
        legacy.raw_metadata = "legacy metadata";
        legacy.source_trace = "Archive";
        bool legacy_created = false;
        int legacy_id = db.UpsertProsecutionDocument(legacy, &legacy_created);
        CHECK(legacy_id > 0 && legacy_created);
        CHECK(db.Execute("UPDATE prosecution_documents SET first_seen_at=100, last_seen_at=150, "
                         "local_path='/legacy/file.pdf', downloaded_at=175, "
                         "download_available=1 WHERE id=" +
                         std::to_string(legacy_id)));

        ProsecutionDocumentRecord canonical;
        canonical.patent_id = 71;
        canonical.source = "epo";
        canonical.application_number = "EP-canonical";
        canonical.fingerprint = "merge-event-fp";
        canonical.event_key = "merge-event-key";
        canonical.document_title = "Canonical semantics";
        canonical.remote_document_id = "canonical-remote";
        canonical.source_trace = "EPO";
        bool canonical_created = false;
        int canonical_id = db.UpsertProsecutionDocument(canonical, &canonical_created);
        CHECK(canonical_id > 0 && canonical_created);
        CHECK(db.Execute("UPDATE prosecution_documents SET first_seen_at=200, last_seen_at=220 "
                         "WHERE id=" + std::to_string(canonical_id)));
        CHECK(count_rows() == 2);

        ProsecutionDocumentRecord incoming = legacy;
        incoming.event_key = "merge-event-key";
        incoming.raw_metadata.clear();
        incoming.source_trace = " uspto, New.Provider ";
        incoming.document_title = "Must not replace canonical";
        bool merged_created = true;
        int merged_id = db.UpsertProsecutionDocument(incoming, &merged_created);
        CHECK(merged_id == canonical_id);
        CHECK(!merged_created);
        CHECK(count_rows() == 1);
        CHECK(db.GetProsecutionDocumentById(legacy_id).id == 0);

        auto merged = db.GetProsecutionDocumentById(canonical_id);
        CHECK_STR_EQ(merged.source, "epo");
        CHECK_STR_EQ(merged.remote_document_id, "canonical-remote");
        CHECK_STR_EQ(merged.document_title, "Canonical semantics");
        CHECK_STR_EQ(merged.download_url, "https://legacy/download");
        CHECK(merged.download_available);
        CHECK_STR_EQ(merged.raw_metadata, "");
        CHECK_STR_EQ(merged.source_trace, "epo,uspto,archive,new.provider");
        CHECK(merged.first_seen_at == 100);
        CHECK(merged.last_seen_at >= 220);
        sqlite3_stmt* file = nullptr;
        CHECK(sqlite3_prepare_v2(db.GetHandle(),
            "SELECT local_path,downloaded_at FROM prosecution_documents WHERE id=?",
            -1, &file, nullptr) == SQLITE_OK);
        sqlite3_bind_int(file, 1, canonical_id);
        CHECK(sqlite3_step(file) == SQLITE_ROW);
        const char* local_path = reinterpret_cast<const char*>(sqlite3_column_text(file, 0));
        const std::string stored_local_path = local_path ? local_path : "";
        CHECK_STR_EQ(stored_local_path, "/legacy/file.pdf");
        CHECK(sqlite3_column_int64(file, 1) == 175);
        sqlite3_finalize(file);

        ProsecutionDocumentRecord rollback_legacy;
        rollback_legacy.patent_id = 72;
        rollback_legacy.source = "uspto";
        rollback_legacy.application_number = "US17444444";
        rollback_legacy.fingerprint = "rollback-legacy";
        rollback_legacy.raw_metadata = "rollback legacy metadata";
        bool rollback_legacy_created = false;
        int rollback_legacy_id = db.UpsertProsecutionDocument(
            rollback_legacy, &rollback_legacy_created);
        CHECK(rollback_legacy_id > 0 && rollback_legacy_created);

        ProsecutionDocumentRecord rollback_event;
        rollback_event.patent_id = 72;
        rollback_event.source = "epo";
        rollback_event.application_number = "EP-rollback";
        rollback_event.fingerprint = "rollback-event";
        rollback_event.event_key = "rollback-key";
        rollback_event.raw_metadata = "canonical before rollback";
        rollback_event.source_trace = "epo";
        bool rollback_event_created = false;
        int rollback_event_id = db.UpsertProsecutionDocument(
            rollback_event, &rollback_event_created);
        CHECK(rollback_event_id > 0 && rollback_event_created);
        CHECK(db.Execute("CREATE TRIGGER block_legacy_delete BEFORE DELETE ON prosecution_documents "
                         "WHEN OLD.id=" + std::to_string(rollback_legacy_id) +
                         " BEGIN SELECT RAISE(ABORT,'blocked delete'); END"));

        ProsecutionDocumentRecord rollback_incoming = rollback_legacy;
        rollback_incoming.event_key = "rollback-key";
        rollback_incoming.raw_metadata = "must roll back";
        bool rollback_created = true;
        CHECK(db.UpsertProsecutionDocument(rollback_incoming, &rollback_created) == 0);
        CHECK(!rollback_created);
        CHECK_STR_EQ(db.GetProsecutionDocumentById(rollback_event_id).raw_metadata,
                     "canonical before rollback");
        sqlite3_stmt* count = nullptr;
        CHECK(sqlite3_prepare_v2(db.GetHandle(),
            "SELECT COUNT(*) FROM prosecution_documents WHERE patent_id=72",
            -1, &count, nullptr) == SQLITE_OK);
        CHECK(sqlite3_step(count) == SQLITE_ROW);
        CHECK(sqlite3_column_int(count, 0) == 2);
        sqlite3_finalize(count);

        ProsecutionDocumentRecord key_a;
        key_a.patent_id = 73;
        key_a.source = "epo";
        key_a.application_number = "EP-key-a";
        key_a.fingerprint = "key-a-fp";
        key_a.event_key = "event-key-a";
        key_a.raw_metadata = "key-a metadata";
        bool key_a_created = false;
        int key_a_id = db.UpsertProsecutionDocument(key_a, &key_a_created);
        CHECK(key_a_id > 0 && key_a_created);

        ProsecutionDocumentRecord key_b;
        key_b.patent_id = 73;
        key_b.source = "uspto";
        key_b.application_number = "US17555555";
        key_b.fingerprint = "key-b-fp";
        key_b.event_key = "event-key-b";
        key_b.raw_metadata = "key-b metadata";
        bool key_b_created = false;
        int key_b_id = db.UpsertProsecutionDocument(key_b, &key_b_created);
        CHECK(key_b_id > 0 && key_b_created);

        ProsecutionDocumentRecord conflicting_key = key_b;
        conflicting_key.event_key = "event-key-a";
        conflicting_key.raw_metadata = "must not merge";
        bool conflicting_created = true;
        CHECK(db.UpsertProsecutionDocument(conflicting_key, &conflicting_created) == 0);
        CHECK(!conflicting_created);
        CHECK_STR_EQ(db.GetProsecutionDocumentById(key_a_id).raw_metadata, "key-a metadata");
        CHECK_STR_EQ(db.GetProsecutionDocumentById(key_b_id).raw_metadata, "key-b metadata");
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

static RemoteDocument OfficialEvent(const std::string& type,
                                    const std::string& title,
                                    const std::string& date,
                                    const std::string& event_key,
                                    int ordinal = 0) {
    RemoteDocument document;
    document.source = "epo_global_dossier";
    document.document_type = type;
    document.document_title = title;
    document.raw_title = title + " (ORIGINAL)";
    document.document_version = "ORIGINAL";
    document.official_date = date;
    document.direction = "official";
    document.remote_document_id = "epo-" + event_key;
    document.event_key = event_key;
    document.source_trace = "epo_global_dossier,cnipa";
    document.confidence = "HIGH";
    document.oa_ordinal = ordinal;
    return document;
}

static void TestOfficialEventTitleAndMergeRules() {
    CHECK_STR_EQ(OfficialEventTitleCn(
                     OfficialEvent("OFFICE_ACTION_FIRST", "审查意见通知书",
                                   "2026-01-10", "oa-1", 1)),
                 "第一次审查意见通知书");
    CHECK_STR_EQ(OfficialEventTitleCn(
                     OfficialEvent("OFFICE_ACTION_SECOND", "审查意见通知书",
                                   "2026-02-10", "oa-2", 2)),
                 "第二次审查意见通知书");
    CHECK_STR_EQ(OfficialEventTitleCn(
                     OfficialEvent("OFFICE_ACTION_NTH", "审查意见通知书",
                                   "2026-03-10", "oa-6", 6)),
                 "第六次审查意见通知书");
    CHECK_STR_EQ(OfficialEventTitleCn(
                     OfficialEvent("REJECTION_DECISION", "Decision of rejection",
                                   "2026-04-10", "reject")),
                 "驳回决定");
    CHECK_STR_EQ(OfficialEventTitleCn(
                     OfficialEvent("GRANT_NOTICE", "Notification to grant patent right",
                                   "2026-05-10", "grant")),
                 "授权通知");
    CHECK_STR_EQ(OfficialEventTitleCn(
                     OfficialEvent("CORRECTION_NOTICE", "Correction notice",
                                   "2026-06-10", "correction")),
                 "补正通知");
    CHECK_STR_EQ(OfficialEventTitleCn(
                     OfficialEvent("OTHER_OFFICIAL", "手续合格通知书",
                                   "2026-07-10", "other")),
                 "手续合格通知书");
    CHECK(ResultCodeFromString("NEW_OFFICIAL_EVENT") == ResultCode::NewOfficialEvent);
    CHECK(ResultCodeFromString("NEW_OFFICE_ACTION") == ResultCode::NewOfficialEvent);
    CHECK_STR_EQ(std::string(ToString(ResultCode::NewOfficialEvent)), "NEW_OFFICIAL_EVENT");

    std::string path = TempDb();
    {
        Database db(path);
        Patent patent;
        patent.geke_code = "GC-EVENTS";
        patent.title = "官方事件合并测试";
        patent.application_number = "202410000001.1";
        patent.application_status = "实质审查中";
        patent.id = db.InsertPatent(patent, false);
        CHECK(patent.id > 0);

        const std::vector<RemoteDocument> events = {
            OfficialEvent("OFFICE_ACTION_FIRST", "审查意见通知书",
                          "2026-01-10", "oa-1", 1),
            OfficialEvent("OFFICE_ACTION_SECOND", "审查意见通知书",
                          "2026-02-10", "oa-2", 2),
            OfficialEvent("OFFICE_ACTION_NTH", "审查意见通知书",
                          "2026-03-10", "oa-6", 6),
            OfficialEvent("REJECTION_DECISION", "Decision of rejection",
                          "2026-04-10", "reject"),
            OfficialEvent("GRANT_NOTICE", "Notification to grant patent right",
                          "2026-05-10", "grant"),
            OfficialEvent("CORRECTION_NOTICE", "Correction notice",
                          "2026-06-10", "correction"),
            OfficialEvent("OTHER_OFFICIAL", "手续合格通知书",
                          "2026-07-10", "other"),
        };
        for (const auto& event : events) {
            EventMergeResult merged = MergeOfficialEvent(db, patent, event);
            CHECK(merged.code == ResultCode::NewOfficialEvent);
            CHECK(merged.oa_created_id > 0);
            CHECK(!merged.canonical_title.empty());
        }

        auto records = db.GetOAsForPatentId(patent.id);
        CHECK(records.size() == events.size());
        CHECK_STR_EQ(records.back().oa_type, "手续合格通知书");
        CHECK_STR_EQ(records.back().source, "epo_global_dossier");
        CHECK_STR_EQ(records.back().remote_document_id, "epo-other");

        EventMergeResult duplicate = MergeOfficialEvent(db, patent, events.front());
        CHECK(duplicate.code == ResultCode::NoChange);
        CHECK(db.GetOAsForPatentId(patent.id).size() == events.size());

        RemoteDocument same_date_different_identity = events[5];
        same_date_different_identity.remote_document_id = "epo-correction-same-date";
        same_date_different_identity.event_key = "correction-same-date";
        EventMergeResult same_date_different_identity_result =
            MergeOfficialEvent(db, patent, same_date_different_identity);
        CHECK(same_date_different_identity_result.code == ResultCode::NewOfficialEvent);
        CHECK(db.GetOAsForPatentId(patent.id).size() == events.size() + 1);

        RemoteDocument changed_title_same_identity = events.back();
        changed_title_same_identity.document_title = "更名后的手续通知";
        EventMergeResult changed_title_same_identity_result =
            MergeOfficialEvent(db, patent, changed_title_same_identity);
        CHECK(changed_title_same_identity_result.code == ResultCode::NoChange);
        CHECK(db.GetOAsForPatentId(patent.id).size() == events.size() + 1);

        RemoteDocument second_correction = OfficialEvent(
            "CORRECTION_NOTICE", "补正通知", "2026-06-20", "correction-2");
        EventMergeResult second_correction_result =
            MergeOfficialEvent(db, patent, second_correction);
        CHECK(second_correction_result.code == ResultCode::NewOfficialEvent);
        CHECK(db.GetOAsForPatentId(patent.id).size() == events.size() + 2);

        RemoteDocument changed_same_correction = second_correction;
        changed_same_correction.official_date = "2026-06-21";
        EventMergeResult changed_same_correction_result =
            MergeOfficialEvent(db, patent, changed_same_correction);
        CHECK(changed_same_correction_result.code == ResultCode::DateConflict);
        CHECK_STR_EQ(db.GetOAById(second_correction_result.oa_created_id).issue_date,
                     "2026-06-20");
        CHECK(db.GetOAsForPatentId(patent.id).size() == events.size() + 2);

        OARecord unrelated_empty;
        unrelated_empty.patent_id = patent.id;
        unrelated_empty.geke_code = patent.geke_code;
        unrelated_empty.oa_type = "补正通知";
        unrelated_empty.source = "epo_global_dossier";
        unrelated_empty.remote_document_id = "epo-unrelated-empty";
        unrelated_empty.sync_flag = "web_new";
        int unrelated_empty_id = db.InsertOA(unrelated_empty, false);
        CHECK(unrelated_empty_id > 0);
        RemoteDocument third_correction = OfficialEvent(
            "CORRECTION_NOTICE", "补正通知", "2026-06-30", "correction-3");
        EventMergeResult third_correction_result =
            MergeOfficialEvent(db, patent, third_correction);
        CHECK(third_correction_result.code == ResultCode::NewOfficialEvent);
        CHECK_STR_EQ(db.GetOAById(unrelated_empty_id).issue_date, "");

        // Terminal official events are reminders only and never alter case status.
        CHECK_STR_EQ(db.GetPatentById(patent.id).application_status, "实质审查中");

        OARecord empty_date;
        empty_date.patent_id = patent.id;
        empty_date.geke_code = patent.geke_code;
        empty_date.oa_type = "其他官方通知";
        empty_date.handler = "人工经办人";
        empty_date.notes = "人工备注";
        int empty_id = db.InsertOA(empty_date, false);
        CHECK(empty_id > 0);
        RemoteDocument fill = OfficialEvent("OTHER_OFFICIAL", "其他官方通知",
                                            "2026-08-10", "fill-date");
        EventMergeResult filled = MergeOfficialEvent(db, patent, fill);
        CHECK(filled.code == ResultCode::NewOfficialEvent);
        OARecord filled_record = db.GetOAById(empty_id);
        CHECK_STR_EQ(filled_record.issue_date, "");
        CHECK_STR_EQ(filled_record.source, "");
        CHECK_STR_EQ(filled_record.remote_document_id, "");
        CHECK_STR_EQ(filled_record.handler, "人工经办人");
        CHECK_STR_EQ(filled_record.notes, "人工备注");

        OARecord legacy_empty;
        legacy_empty.patent_id = patent.id;
        legacy_empty.geke_code = patent.geke_code;
        legacy_empty.oa_type = "复审通知";
        legacy_empty.sync_flag = "web_new";
        legacy_empty.handler = "旧同步记录经办人";
        legacy_empty.writer = "旧同步记录撰写人";
        legacy_empty.official_deadline = "2026-12-31";
        legacy_empty.notes = "旧同步记录备注";
        int legacy_empty_id = db.InsertOA(legacy_empty, false);
        CHECK(legacy_empty_id > 0);
        RemoteDocument claim_legacy = OfficialEvent(
            "OTHER_OFFICIAL", "复审通知", "2026-08-09", "claim-legacy");
        EventMergeResult claimed = MergeOfficialEvent(db, patent, claim_legacy);
        CHECK(claimed.code == ResultCode::NoChange);
        OARecord claimed_record = db.GetOAById(legacy_empty_id);
        CHECK_STR_EQ(claimed_record.issue_date, "2026-08-09");
        CHECK_STR_EQ(claimed_record.source, "epo_global_dossier");
        CHECK_STR_EQ(claimed_record.remote_document_id, "epo-claim-legacy");
        CHECK_STR_EQ(claimed_record.handler, "旧同步记录经办人");
        CHECK_STR_EQ(claimed_record.writer, "旧同步记录撰写人");
        CHECK_STR_EQ(claimed_record.official_deadline, "2026-12-31");
        CHECK_STR_EQ(claimed_record.notes, "旧同步记录备注");

        RemoteDocument source_preference = OfficialEvent(
            "OTHER_OFFICIAL", "来源精确匹配后的新标题",
            "2026-08-21", "source-preference");
        OARecord source_less_identity;
        source_less_identity.patent_id = patent.id;
        source_less_identity.oa_type = "来源空的旧标题";
        source_less_identity.issue_date = "2026-08-20";
        source_less_identity.remote_document_id = source_preference.remote_document_id;
        source_less_identity.sync_flag = "web_new";
        int source_less_identity_id = db.InsertOA(source_less_identity, false);
        CHECK(source_less_identity_id > 0);
        OARecord exact_source_identity = source_less_identity;
        exact_source_identity.oa_type = "精确来源的旧标题";
        exact_source_identity.issue_date = "2026-08-21";
        exact_source_identity.source = "epo_global_dossier";
        int exact_source_identity_id = db.InsertOA(exact_source_identity, false);
        CHECK(exact_source_identity_id > 0);
        EventMergeResult source_preference_result =
            MergeOfficialEvent(db, patent, source_preference);
        CHECK(source_preference_result.code == ResultCode::NoChange);
        CHECK_STR_EQ(db.GetOAById(source_less_identity_id).sync_flag, "web_new");
        CHECK_STR_EQ(db.GetOAById(exact_source_identity_id).sync_flag, "web_new");

        OARecord source_less_legacy;
        source_less_legacy.patent_id = patent.id;
        source_less_legacy.oa_type = "旧同步来源优先通知";
        source_less_legacy.sync_flag = "web_new";
        int source_less_legacy_id = db.InsertOA(source_less_legacy, false);
        CHECK(source_less_legacy_id > 0);
        OARecord exact_source_legacy = source_less_legacy;
        exact_source_legacy.source = "epo_global_dossier";
        int exact_source_legacy_id = db.InsertOA(exact_source_legacy, false);
        CHECK(exact_source_legacy_id > 0);
        RemoteDocument claim_exact_source_legacy = OfficialEvent(
            "OTHER_OFFICIAL", "旧同步来源优先通知",
            "2026-08-22", "claim-exact-source");
        EventMergeResult exact_source_claimed =
            MergeOfficialEvent(db, patent, claim_exact_source_legacy);
        CHECK(exact_source_claimed.code == ResultCode::NoChange);
        CHECK_STR_EQ(db.GetOAById(source_less_legacy_id).issue_date, "");
        OARecord exact_source_claimed_record = db.GetOAById(exact_source_legacy_id);
        CHECK_STR_EQ(exact_source_claimed_record.issue_date, "2026-08-22");
        CHECK_STR_EQ(exact_source_claimed_record.remote_document_id,
                     "epo-claim-exact-source");

        OARecord conflict;
        conflict.patent_id = patent.id;
        conflict.geke_code = patent.geke_code;
        conflict.oa_type = "复审决定";
        conflict.issue_date = "2026-08-01";
        conflict.writer = "人工撰写人";
        int conflict_id = db.InsertOA(conflict, false);
        CHECK(conflict_id > 0);
        RemoteDocument conflicting = OfficialEvent("OTHER_OFFICIAL", "复审决定",
                                                   "2026-08-11", "conflict");
        EventMergeResult conflict_result = MergeOfficialEvent(db, patent, conflicting);
        CHECK(conflict_result.code == ResultCode::NewOfficialEvent);
        CHECK(!conflict_result.date_conflict);
        CHECK_STR_EQ(db.GetOAById(conflict_id).issue_date, "2026-08-01");
        CHECK_STR_EQ(db.GetOAById(conflict_id).writer, "人工撰写人");
        CHECK_STR_EQ(db.GetOAById(conflict_id).sync_flag, "");

        const size_t before_rejected = db.GetOAsForPatentId(patent.id).size();
        RemoteDocument low = OfficialEvent("OTHER_OFFICIAL", "低置信度通知",
                                           "2026-08-12", "low");
        low.confidence = "LOW";
        CHECK(MergeOfficialEvent(db, patent, low).code ==
              ResultCode::ManualReviewRequired);
        RemoteDocument translated = OfficialEvent("OTHER_OFFICIAL", "译文通知",
                                                  "2026-08-13", "translated");
        translated.document_version = "TRANSLATED";
        CHECK(MergeOfficialEvent(db, patent, translated).code == ResultCode::NoChange);
        RemoteDocument applicant = OfficialEvent("OTHER_OFFICIAL", "申请人文件",
                                                 "2026-08-14", "applicant");
        applicant.direction = "applicant";
        CHECK(MergeOfficialEvent(db, patent, applicant).code == ResultCode::NoChange);

        RemoteDocument invalid_date = OfficialEvent(
            "OTHER_OFFICIAL", "无效日期通知", "2026-02-30", "invalid-date");
        CHECK(MergeOfficialEvent(db, patent, invalid_date).code ==
              ResultCode::DateParseFailed);
        CHECK(db.GetOAsForPatentId(patent.id).size() == before_rejected);

        sqlite3_stmt* stmt = nullptr;
        CHECK(sqlite3_prepare_v2(db.GetHandle(),
            "SELECT COUNT(*) FROM prosecution_documents "
            "WHERE COALESCE(local_path,'') <> ''", -1, &stmt, nullptr) == SQLITE_OK);
        CHECK(sqlite3_step(stmt) == SQLITE_ROW);
        CHECK(sqlite3_column_int(stmt, 0) == 0);
        sqlite3_finalize(stmt);
    }
    std::filesystem::remove(path);
}

static void TestOfficialEventPersistenceAndSchedulingRules() {
    RemoteDocument event = OfficialEvent(
        "GRANT_NOTICE", "授权通知", "2026-08-10", "grant-event");
    CHECK(IsPersistableOfficialEvent(event));

    RemoteDocument translated = event;
    translated.document_version = "TRANSLATED";
    CHECK(!IsPersistableOfficialEvent(translated));

    RemoteDocument applicant = event;
    applicant.direction = "applicant";
    CHECK(!IsPersistableOfficialEvent(applicant));

    RemoteDocument low = event;
    low.confidence = "LOW";
    CHECK(!IsPersistableOfficialEvent(low));

    RemoteDocument unknown = event;
    unknown.document_type = "UNKNOWN";
    CHECK(!IsPersistableOfficialEvent(unknown));

    RemoteDocument invalid_date = event;
    invalid_date.official_date = "2026-02-30";
    CHECK(!IsPersistableOfficialEvent(invalid_date));

    CHECK(NormalizeCheckIntervalDays(1) == 1);
    CHECK(NormalizeCheckIntervalDays(3) == 3);
    CHECK(NormalizeCheckIntervalDays(7) == 7);
    CHECK(NormalizeCheckIntervalDays(0) == 1);
    CHECK(NormalizeCheckIntervalDays(365) == 1);

    const long long now = 1'800'000'000LL;
    CHECK(NextDossierCheckAt(ResultCode::Ok, now, 3) == now + 3 * 86400);
    CHECK(NextDossierCheckAt(ResultCode::NoChange, now, 7) == now + 7 * 86400);
    CHECK(NextDossierCheckAt(ResultCode::NewOfficialEvent, now, 1) == now + 86400);
    CHECK(NextDossierCheckAt(ResultCode::DateConflict, now, 3) == now + 3 * 86400);
    CHECK(NextDossierCheckAt(ResultCode::NetworkError, now, 7) == now + 1800);
    CHECK(NextDossierCheckAt(ResultCode::RateLimited, now, 7) == now + 1800);
    CHECK(NextDossierCheckAt(ResultCode::PageStructureChanged, now, 7) == now + 1800);
    CHECK(NextDossierCheckAt(ResultCode::Ok, now, 365) == now + 86400);
}

static void TestBatchSummaryClassifiesConflictsAsManualReview() {
    BatchSummary summary;
    CaseSyncReport report;

    report.code = ResultCode::NewOfficialEvent;
    AccumulateBatchSummary(summary, report);
    CHECK(summary.checked == 1);
    CHECK(summary.new_events == 1);
    CHECK(summary.manual_review == 0);
    CHECK(summary.findings.size() == 1);

    report.code = ResultCode::DateConflict;
    AccumulateBatchSummary(summary, report);
    CHECK(summary.checked == 2);
    CHECK(summary.new_events == 1);
    CHECK(summary.manual_review == 1);
    CHECK(summary.findings.size() == 2);

    report.code = ResultCode::ManualReviewRequired;
    AccumulateBatchSummary(summary, report);
    CHECK(summary.manual_review == 2);

    report.code = ResultCode::NoChange;
    AccumulateBatchSummary(summary, report);
    CHECK(summary.no_change == 1);

    report.code = ResultCode::NetworkError;
    AccumulateBatchSummary(summary, report);
    CHECK(summary.failed == 1);
    CHECK(summary.failures.size() == 1);
}

static void TestRemoteCaseParsingAndApplicationBoundary() {
    const std::string body = R"json({
        "ok": true,
        "code": "OK",
        "message": "",
        "resolved_application_number": "CN202410000001.1",
        "auth_state": "AUTHENTICATED",
        "provider_used": "epo_global_dossier",
        "documents": [
            {
                "source": "",
                "document_type": "OFFICE_ACTION_FIRST",
                "document_title": "第一次审查意见通知书",
                "raw_title": "First notice (ORIGINAL)",
                "document_code": "210401-CN",
                "document_version": "ORIGINAL",
                "official_date": "2026-01-10",
                "direction": "official",
                "remote_document_id": "epo-oa-1",
                "fingerprint": "fp-oa-1",
                "event_key": "event-oa-1",
                "source_trace": ["epo_global_dossier", "cnipa"],
                "confidence": "HIGH",
                "oa_ordinal": 1
            },
            {
                "source": "epo_global_dossier",
                "document_type": "GRANT_NOTICE",
                "document_title": "授权通知",
                "raw_title": "Notification to grant patent right (ORIGINAL)",
                "document_code": "grant-code",
                "document_version": "ORIGINAL",
                "official_date": "2026-02-10",
                "direction": "official",
                "remote_document_id": "epo-grant-1",
                "fingerprint": "fp-grant-1",
                "event_key": "event-grant-1",
                "source_trace": ["epo_global_dossier"],
                "confidence": "HIGH"
            },
            {
                "source": "epo_global_dossier",
                "document_type": "GRANT_NOTICE",
                "document_title": "授权通知",
                "document_version": "TRANSLATED",
                "official_date": "2026-02-10",
                "direction": "official",
                "remote_document_id": "epo-grant-translated",
                "event_key": "event-grant-translated",
                "confidence": "HIGH"
            },
            {
                "source": "epo_global_dossier",
                "document_type": "OTHER_OFFICIAL",
                "document_title": "申请人文件",
                "document_version": "ORIGINAL",
                "official_date": "2026-02-11",
                "direction": "applicant",
                "remote_document_id": "epo-applicant",
                "event_key": "event-applicant",
                "confidence": "HIGH"
            }
        ],
        "latest_event": {
            "source": "epo_global_dossier",
            "document_type": "GRANT_NOTICE",
            "document_title": "授权通知",
            "raw_title": "Notification to grant patent right (ORIGINAL)",
            "document_code": "grant-code",
            "document_version": "ORIGINAL",
            "official_date": "2026-02-10",
            "direction": "official",
            "remote_document_id": "epo-grant-1",
            "fingerprint": "fp-grant-1",
            "event_key": "event-grant-1",
            "source_trace": ["epo_global_dossier"],
            "confidence": "HIGH"
        }
    })json";

    RemoteCaseResult remote;
    CHECK(ParseRemoteCaseResultJson(body, remote));
    CHECK_STR_EQ(remote.provider_used, "epo_global_dossier");
    CHECK(remote.documents.size() == 4);
    CHECK_STR_EQ(remote.documents[0].source, "epo_global_dossier");
    CHECK_STR_EQ(remote.documents[0].document_code, "210401-CN");
    CHECK_STR_EQ(remote.documents[0].document_version, "ORIGINAL");
    CHECK_STR_EQ(remote.documents[0].event_key, "event-oa-1");
    CHECK_STR_EQ(remote.documents[0].source_trace, "epo_global_dossier,cnipa");
    CHECK(remote.has_latest_event);
    CHECK_STR_EQ(remote.latest_event.remote_document_id, "epo-grant-1");

    std::string path = TempDb();
    {
        Database db(path);
        Patent patent;
        patent.geke_code = "GC-BOUNDARY";
        patent.application_number = "202410000001.1";
        patent.application_status = "实质审查中";
        patent.title = "边界测试";
        patent.id = db.InsertPatent(patent, false);

        const long long now = 1'800'000'000LL;
        CaseSyncReport first = ApplyRemoteCaseResult(db, patent, remote, 3, now);
        CHECK(first.code == ResultCode::NewOfficialEvent);
        CHECK(first.documents_total == 4);
        CHECK(first.documents_new == 2);
        CHECK(db.GetOAsForPatentId(patent.id).size() == 1);
        OARecord grant = db.GetOAsForPatentId(patent.id).front();
        CHECK_STR_EQ(grant.oa_type, "授权通知");
        CHECK_STR_EQ(grant.source, "epo_global_dossier");
        CHECK_STR_EQ(grant.remote_document_id, "epo-grant-1");

        sqlite3_stmt* stmt = nullptr;
        CHECK(sqlite3_prepare_v2(db.GetHandle(),
            "SELECT id FROM prosecution_documents ORDER BY id", -1,
            &stmt, nullptr) == SQLITE_OK);
        std::vector<int> document_ids;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            document_ids.push_back(sqlite3_column_int(stmt, 0));
        }
        sqlite3_finalize(stmt);
        CHECK(document_ids.size() == 2);
        ProsecutionDocumentRecord first_document =
            db.GetProsecutionDocumentById(document_ids.front());
        CHECK_STR_EQ(first_document.source, "epo_global_dossier");
        CHECK_STR_EQ(first_document.document_code, "210401-CN");
        CHECK_STR_EQ(first_document.event_key, "event-oa-1");
        CHECK_STR_EQ(first_document.source_trace, "epo_global_dossier,cnipa");

        CHECK(sqlite3_prepare_v2(db.GetHandle(),
            "SELECT last_dossier_check_at,next_dossier_check_at FROM patents WHERE id=?",
            -1, &stmt, nullptr) == SQLITE_OK);
        sqlite3_bind_int(stmt, 1, patent.id);
        CHECK(sqlite3_step(stmt) == SQLITE_ROW);
        CHECK(sqlite3_column_int64(stmt, 0) == now);
        CHECK(sqlite3_column_int64(stmt, 1) == now + 3 * 86400);
        sqlite3_finalize(stmt);

        auto states = db.GetDossierSyncStates();
        CHECK(states.size() == 1);
        CHECK_STR_EQ(states.front().provider, "epo_global_dossier");
        CHECK_STR_EQ(states.front().latest_remote_oa_type, "授权通知");

        CaseSyncReport duplicate = ApplyRemoteCaseResult(db, patent, remote, 3, now + 10);
        CHECK(duplicate.code == ResultCode::NoChange);
        CHECK(duplicate.documents_new == 0);
        CHECK(db.GetOAsForPatentId(patent.id).size() == 1);

        RemoteCaseResult network;
        network.code = "NETWORK_ERROR";
        network.provider_used = "epo_global_dossier";
        ApplyRemoteCaseResult(db, patent, network, 7, now + 20);
        CHECK(sqlite3_prepare_v2(db.GetHandle(),
            "SELECT next_dossier_check_at FROM patents WHERE id=?", -1,
            &stmt, nullptr) == SQLITE_OK);
        sqlite3_bind_int(stmt, 1, patent.id);
        CHECK(sqlite3_step(stmt) == SQLITE_ROW);
        CHECK(sqlite3_column_int64(stmt, 0) == now + 20 + 1800);
        sqlite3_finalize(stmt);
    }
    std::filesystem::remove(path);
}

int main() {
    TestNormalizeOaType();
    TestQueueFilter();
    TestDueQueueFilter();
    TestFingerprintDedup();
    TestEventKeyDedupAndDocumentRoundtrip();
    TestLegacyEventKeyPromotion();
    TestCanonicalLegacyRowsAreMergedTransactionally();
    TestOaMergeRules();
    TestOfficialEventTitleAndMergeRules();
    TestOfficialEventPersistenceAndSchedulingRules();
    TestBatchSummaryClassifiesConflictsAsManualReview();
    TestRemoteCaseParsingAndApplicationBoundary();
    if (g_failures == 0) {
        std::cout << "all web dossier rule tests passed" << std::endl;
        return 0;
    }
    std::cout << g_failures << " failure(s)" << std::endl;
    return 1;
}
