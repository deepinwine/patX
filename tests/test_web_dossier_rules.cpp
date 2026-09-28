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

int main() {
    TestNormalizeOaType();
    TestQueueFilter();
    TestDueQueueFilter();
    TestFingerprintDedup();
    TestEventKeyDedupAndDocumentRoundtrip();
    TestLegacyEventKeyPromotion();
    TestCanonicalLegacyRowsAreMergedTransactionally();
    TestOaMergeRules();
    if (g_failures == 0) {
        std::cout << "all web dossier rule tests passed" << std::endl;
        return 0;
    }
    std::cout << g_failures << " failure(s)" << std::endl;
    return 1;
}
