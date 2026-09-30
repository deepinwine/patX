// Excel IO tests: real XLSX export (openable workbook with correct cells),
// CSV export quoting, structured-field import.
#include "test_registry.hpp"

#include "database.hpp"
#include "excel_io.hpp"
#include "web_dossier.hpp"

#include <OpenXLSX.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

using namespace testutil;

static void WriteOAWorkbook(
    const std::string& path,
    const std::vector<std::vector<std::string>>& rows
) {
    ExcelIO io;
    ExportTable table;
    table.sheet_name = "OA";
    table.headers = {"编号", "专利名称", "OA类型", "发文日", "官方期限", "处理人"};
    table.rows = rows;
    CHECK(io.ExportXlsx(table, path));
}

TEST(excel_export_writes_real_xlsx) {
    std::string path = TempDbPath("export.xlsx");
    {
        ExcelIO io;
        ExportTable table;
        table.sheet_name = "Patents";
        table.headers = {"编号", "标题", "备注"};
        table.rows.push_back({"GK-1", "图像处理,装置", "has \"quotes\""});
        table.rows.push_back({"GK-2", "第二行", "line1\nline2"});
        CHECK(io.ExportXlsx(table, path));
    }

    // The file must be a real ZIP-based xlsx, not CSV text with a new suffix
    {
        std::ifstream f(path, std::ios::binary);
        char magic[4] = {0};
        f.read(magic, 4);
        CHECK_EQ(static_cast<unsigned char>(magic[0]), 0x50u);  // 'P'
        CHECK_EQ(static_cast<unsigned char>(magic[1]), 0x4Bu);  // 'K'
    }

    // Reopen with OpenXLSX and validate the cells
    {
        OpenXLSX::XLDocument doc;
        doc.open(path);
        auto ws = doc.workbook().worksheet("Patents");
        CHECK_EQ(ws.rowCount(), 3u);

        auto cell = [](OpenXLSX::XLWorksheet& w, uint32_t r, uint16_t c) {
            return w.cell(r, c).value().get<std::string>();
        };
        CHECK_EQ(cell(ws, 1, 1), std::string("编号"));
        CHECK_EQ(cell(ws, 2, 2), std::string("图像处理,装置"));
        CHECK_EQ(cell(ws, 2, 3), std::string("has \"quotes\""));
        CHECK_EQ(cell(ws, 3, 3), std::string("line1\nline2"));
        doc.close();
    }
    std::filesystem::remove(path);
}

TEST(excel_export_csv_quoting) {
    std::string path = TempDbPath("export.csv");
    {
        ExcelIO io;
        ExportTable table;
        table.headers = {"a", "b"};
        table.rows.push_back({"plain", "with,comma"});
        table.rows.push_back({"with\"quote", "multi\nline"});
        CHECK(io.ExportCsv(table, path));
    }
    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(content.find("\"with,comma\"") != std::string::npos);
    CHECK(content.find("\"with\"\"quote\"") != std::string::npos);
    CHECK(content.size() > 3);   // BOM + content
    std::filesystem::remove(path);
}

TEST(excel_import_csv_into_structured_fields) {
    std::string csv_path =
        (std::filesystem::temp_directory_path() /
         ("patx_test_import_" + std::to_string(::testutil::getpid_valid()) + ".csv")).string();
    std::string db_path = TempDbPath("import");
    std::filesystem::remove(db_path);
    {
        std::ofstream out(csv_path);
        out << "\xEF\xBB\xBF";
        out << "编码,申请号,发明名称,技术路线,研发项目,标签,代理人编码,代理人,1st OA,PCT提醒\n";
        out << "GK-IMP-1,202510000002.X,导入专利,光模块,光通信项目,光;AI,AG77,孙代理,2025-07-01,2027-07-01\n";
    }
    {
        Database db(db_path);
        ExcelIO io;
        auto result = io.ImportPatents(csv_path, db);
        CHECK(result.added == 1);

        Patent p = db.GetPatentByCode("GK-IMP-1");
        CHECK_STR_EQ(p.application_number, "202510000002.X");
        CHECK_STR_EQ(p.technology_route, "光模块");
        CHECK_STR_EQ(p.rd_project, "光通信项目");
        CHECK_STR_EQ(p.tags, "光;AI");
        CHECK_STR_EQ(p.agent_code, "AG77");
        CHECK_STR_EQ(p.agent_name, "孙代理");
        CHECK_STR_EQ(p.oa_reminder_1, "2025-07-01");
        CHECK_STR_EQ(p.pct_reminder, "2027-07-01");
        // Structured fields must NOT be duplicated into notes
        CHECK(p.notes.find("技术路线") == std::string::npos);
        CHECK(p.notes.empty());
    }
    std::filesystem::remove(csv_path);
    std::filesystem::remove(db_path);
}

TEST(oa_excel_import_previews_and_adds_new_handler) {
    const std::string xlsx_path = TempDbPath("oa_import_preview") + ".xlsx";
    const std::string db_path = TempDbPath("oa_import_preview");
    std::filesystem::remove(xlsx_path);
    std::filesystem::remove(db_path);
    WriteOAWorkbook(xlsx_path, {{"GK-OA-1", "OA 导入专利", "第一次审查意见通知书",
                                 "2026-09-01", "2027-01-01", "新处理人"}});

    int review_calls = 0;
    OAImportPreview preview;
    ImportResult result;
    std::vector<OARecord> records;
    {
        Database db(db_path);
        ExcelIO io;
        result = io.ImportPatents(
            xlsx_path,
            db,
            nullptr,
            [&](const OAImportPreview& value) {
                review_calls++;
                preview = value;
                return OAHandlerConflictPolicy::PreserveExisting;
            }
        );
        records = db.GetOARecords();
    }

    std::filesystem::remove(xlsx_path);
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + "-wal");
    std::filesystem::remove(db_path + "-shm");

    CHECK_EQ(review_calls, 1);
    CHECK_EQ(preview.added, 1);
    CHECK_EQ(result.added, 1);
    CHECK_EQ(records.size(), 1u);
    CHECK_STR_EQ(records.front().handler, "新处理人");
}

TEST(oa_excel_import_matches_without_duplicate_and_fills_empty_handler) {
    const std::string xlsx_path = TempDbPath("oa_import_exact_match") + ".xlsx";
    std::filesystem::remove(xlsx_path);
    WriteOAWorkbook(xlsx_path, {{"GK-OA-2", "测试专利", "第一次审查意见通知书",
                                 "2026-09-02", "2027-01-02", "李四"}});

    Database db(":memory:");
    OARecord existing;
    existing.geke_code = "GK-OA-2";
    existing.oa_type = "一通";
    existing.issue_date = "2026-09-02";
    existing.writer = "人工撰写人";
    const int existing_id = db.InsertOA(existing);
    CHECK(existing_id > 0);

    ExcelIO io;
    const ImportResult first = io.ImportPatents(xlsx_path, db);
    CHECK_EQ(first.added, 0);
    CHECK_EQ(first.updated, 1);

    auto records = db.GetOAByPatent("GK-OA-2");
    CHECK_EQ(records.size(), 1u);
    CHECK_EQ(records.front().id, existing_id);
    CHECK_STR_EQ(records.front().handler, "李四");
    CHECK_STR_EQ(records.front().writer, "人工撰写人");

    const ImportResult second = io.ImportPatents(xlsx_path, db);
    CHECK_EQ(second.added, 0);
    CHECK_EQ(second.updated, 0);
    CHECK_EQ(second.skipped, 1);
    records = db.GetOAByPatent("GK-OA-2");
    CHECK_EQ(records.size(), 1u);

    std::filesystem::remove(xlsx_path);
}

TEST(oa_excel_import_deduplicates_sheet_keys_and_rejects_handler_disagreement) {
    const std::string xlsx_path = TempDbPath("oa_import_sheet_duplicates") + ".xlsx";
    std::filesystem::remove(xlsx_path);
    WriteOAWorkbook(xlsx_path, {
        {"GK-OA-DUP", "重复专利", "第一次审查意见通知书",
         "2026-09-03", "2027-01-03", "李四"},
        {"GK-OA-DUP", "重复专利", "一通",
         "2026-09-03", "2027-01-03", "李四"},
        {"GK-OA-DIFF", "冲突专利", "第二次审查意见通知书",
         "2026-09-04", "2027-01-04", "李四"},
        {"GK-OA-DIFF", "冲突专利", "二通",
         "2026-09-04", "2027-01-04", "王五"},
    });

    Database db(":memory:");
    OARecord existing;
    existing.geke_code = "GK-OA-DIFF";
    existing.oa_type = "二通";
    existing.issue_date = "2026-09-04";
    existing.writer = "人工撰写人";
    const int existing_id = db.InsertOA(existing);
    CHECK(existing_id > 0);

    OAImportPreview preview;
    ExcelIO io;
    const ImportResult result = io.ImportPatents(
        xlsx_path,
        db,
        nullptr,
        [&](const OAImportPreview& value) {
            preview = value;
            return OAHandlerConflictPolicy::PreserveExisting;
        }
    );

    CHECK_EQ(preview.added, 1);
    CHECK_EQ(preview.handler_updates, 0);
    CHECK_EQ(preview.unchanged, 1);
    CHECK_EQ(preview.handler_conflicts, 2);
    CHECK_EQ(result.added, 1);
    CHECK_EQ(result.updated, 0);
    CHECK_EQ(result.skipped, 3);
    CHECK_EQ(result.handler_conflicts, 2);

    const auto duplicate_records = db.GetOAByPatent("GK-OA-DUP");
    CHECK_EQ(duplicate_records.size(), 1u);
    CHECK_STR_EQ(duplicate_records.front().handler, "李四");

    const auto conflict_records = db.GetOAByPatent("GK-OA-DIFF");
    CHECK_EQ(conflict_records.size(), 1u);
    CHECK_EQ(conflict_records.front().id, existing_id);
    CHECK(conflict_records.front().handler.empty());
    CHECK_STR_EQ(conflict_records.front().writer, "人工撰写人");

    std::filesystem::remove(xlsx_path);
}

TEST(oa_excel_import_revalidates_database_changes_made_by_review_callback) {
    const std::string xlsx_path = TempDbPath("oa_import_callback_revalidation") + ".xlsx";
    std::filesystem::remove(xlsx_path);
    WriteOAWorkbook(xlsx_path, {
        {"GK-OA-RACE-UPDATE", "更新竞态专利", "第一次审查意见通知书",
         "2026-09-05", "2027-01-05", "李四"},
        {"GK-OA-RACE-INSERT", "插入竞态专利", "第二次审查意见通知书",
         "2026-09-06", "2027-01-06", "李四"},
    });

    Database db(":memory:");
    OARecord existing;
    existing.geke_code = "GK-OA-RACE-UPDATE";
    existing.oa_type = "一通";
    existing.issue_date = "2026-09-05";
    existing.writer = "原人工撰写人";
    const int existing_id = db.InsertOA(existing);
    CHECK(existing_id > 0);

    int review_calls = 0;
    OAImportPreview preview;
    ExcelIO io;
    const ImportResult result = io.ImportPatents(
        xlsx_path,
        db,
        nullptr,
        [&](const OAImportPreview& value) {
            review_calls++;
            preview = value;

            OARecord concurrently_updated = db.GetOAById(existing_id);
            concurrently_updated.handler = "王五";
            CHECK(db.UpdateOA(existing_id, concurrently_updated));

            OARecord concurrently_inserted;
            concurrently_inserted.geke_code = "GK-OA-RACE-INSERT";
            concurrently_inserted.oa_type = "二通";
            concurrently_inserted.issue_date = "2026-09-06";
            concurrently_inserted.handler = "王五";
            concurrently_inserted.writer = "回调人工撰写人";
            CHECK(db.InsertOA(concurrently_inserted) > 0);
            return OAHandlerConflictPolicy::PreserveExisting;
        }
    );

    CHECK_EQ(review_calls, 1);
    CHECK_EQ(preview.added, 1);
    CHECK_EQ(preview.handler_updates, 1);
    CHECK_EQ(result.added, 0);
    CHECK_EQ(result.updated, 0);
    CHECK_EQ(result.skipped, 2);
    CHECK_EQ(result.handler_conflicts, 2);

    const auto update_records = db.GetOAByPatent("GK-OA-RACE-UPDATE");
    CHECK_EQ(update_records.size(), 1u);
    CHECK_STR_EQ(update_records.front().handler, "王五");
    CHECK_STR_EQ(update_records.front().writer, "原人工撰写人");

    const auto insert_records = db.GetOAByPatent("GK-OA-RACE-INSERT");
    CHECK_EQ(insert_records.size(), 1u);
    CHECK_STR_EQ(insert_records.front().handler, "王五");
    CHECK_STR_EQ(insert_records.front().writer, "回调人工撰写人");

    std::filesystem::remove(xlsx_path);
}

TEST(oa_excel_import_rebuilds_plan_after_callback_clears_or_deletes_matches) {
    const std::string xlsx_path = TempDbPath("oa_import_callback_rebuild") + ".xlsx";
    std::filesystem::remove(xlsx_path);
    WriteOAWorkbook(xlsx_path, {
        {"GK-OA-CLEAR", "清空处理人专利", "第一次审查意见通知书",
         "2026-09-07", "2027-01-07", "李四"},
        {"GK-OA-DELETE", "删除匹配专利", "第二次审查意见通知书",
         "2026-09-08", "2027-01-08", "李四"},
    });

    Database db(":memory:");
    OARecord clear_target;
    clear_target.geke_code = "GK-OA-CLEAR";
    clear_target.oa_type = "一通";
    clear_target.issue_date = "2026-09-07";
    clear_target.handler = "王五";
    clear_target.writer = "清空测试撰写人";
    const int clear_id = db.InsertOA(clear_target);
    CHECK(clear_id > 0);

    OARecord delete_target;
    delete_target.geke_code = "GK-OA-DELETE";
    delete_target.oa_type = "二通";
    delete_target.issue_date = "2026-09-08";
    delete_target.handler = "李四";
    const int delete_id = db.InsertOA(delete_target);
    CHECK(delete_id > 0);

    int review_calls = 0;
    OAImportPreview preview;
    ExcelIO io;
    const ImportResult result = io.ImportPatents(
        xlsx_path,
        db,
        nullptr,
        [&](const OAImportPreview& value) {
            review_calls++;
            preview = value;

            OARecord cleared = db.GetOAById(clear_id);
            cleared.handler.clear();
            CHECK(db.UpdateOA(clear_id, cleared));
            CHECK(db.DeleteOA(delete_id));
            return OAHandlerConflictPolicy::PreserveExisting;
        }
    );

    CHECK_EQ(review_calls, 1);
    CHECK_EQ(preview.handler_conflicts, 1);
    CHECK_EQ(preview.unchanged, 1);
    CHECK_EQ(result.added, 1);
    CHECK_EQ(result.updated, 1);
    CHECK_EQ(result.skipped, 0);
    CHECK_EQ(result.handler_conflicts, 0);

    const auto clear_records = db.GetOAByPatent("GK-OA-CLEAR");
    CHECK_EQ(clear_records.size(), 1u);
    CHECK_EQ(clear_records.front().id, clear_id);
    CHECK_STR_EQ(clear_records.front().handler, "李四");
    CHECK_STR_EQ(clear_records.front().writer, "清空测试撰写人");

    const auto delete_records = db.GetOAByPatent("GK-OA-DELETE");
    CHECK_EQ(delete_records.size(), 1u);
    CHECK_STR_EQ(delete_records.front().handler, "李四");

    std::filesystem::remove(xlsx_path);
}

TEST(oa_excel_import_rebuilds_duplicate_group_conflicts_after_callback) {
    const std::string xlsx_path = TempDbPath("oa_import_callback_group_rebuild") + ".xlsx";
    std::filesystem::remove(xlsx_path);
    WriteOAWorkbook(xlsx_path, {
        {"GK-OA-GROUP-RACE", "分组竞态专利", "第三次审查意见通知书",
         "2026-09-09", "2027-01-09", "李四"},
        {"GK-OA-GROUP-RACE", "分组竞态专利", "三通",
         "2026-09-09", "2027-01-09", "李四"},
    });

    Database db(":memory:");
    int review_calls = 0;
    OAImportPreview preview;
    ExcelIO io;
    const ImportResult result = io.ImportPatents(
        xlsx_path,
        db,
        nullptr,
        [&](const OAImportPreview& value) {
            review_calls++;
            preview = value;

            OARecord concurrent;
            concurrent.geke_code = "GK-OA-GROUP-RACE";
            concurrent.oa_type = "三通";
            concurrent.issue_date = "2026-09-09";
            concurrent.handler = "王五";
            concurrent.writer = "回调分组撰写人";
            CHECK(db.InsertOA(concurrent) > 0);
            return OAHandlerConflictPolicy::PreserveExisting;
        }
    );

    CHECK_EQ(review_calls, 1);
    CHECK_EQ(preview.added, 1);
    CHECK_EQ(preview.unchanged, 1);
    CHECK_EQ(result.added, 0);
    CHECK_EQ(result.updated, 0);
    CHECK_EQ(result.skipped, 2);
    CHECK_EQ(result.handler_conflicts, 2);

    const auto records = db.GetOAByPatent("GK-OA-GROUP-RACE");
    CHECK_EQ(records.size(), 1u);
    CHECK_STR_EQ(records.front().handler, "王五");
    CHECK_STR_EQ(records.front().writer, "回调分组撰写人");

    std::filesystem::remove(xlsx_path);
}

TEST(oa_excel_import_and_background_sync_share_atomic_exact_merge) {
    const std::string xlsx_path = TempDbPath("oa_import_background_race") + ".xlsx";
    const std::string db_path = TempDbPath("oa_import_background_race");
    std::filesystem::remove(xlsx_path);
    std::filesystem::remove(db_path);
    WriteOAWorkbook(xlsx_path, {
        {"GK-OA-BACKGROUND-RACE", "并发同步专利", "一通",
         "2026-09-12", "2027-01-12", "李四"},
    });

    {
        Database background_db(db_path);
        Patent patent;
        patent.geke_code = "GK-OA-BACKGROUND-RACE";
        patent.title = "并发同步专利";
        patent.id = background_db.InsertPatent(patent, false);
        CHECK(patent.id > 0);

        Database excel_db(db_path);
        webdossier::RemoteDocument document;
        document.document_type = "OFFICE_ACTION_FIRST";
        document.document_title = "第一次审查意见通知书";
        document.official_date = "2026-09-12";
        document.direction = "official";
        document.document_version = "ORIGINAL";
        document.confidence = "HIGH";
        document.source = "cnipa";
        document.remote_document_id = "cnipa-race-1";

        std::promise<void> start_promise;
        std::shared_future<void> start = start_promise.get_future().share();
        webdossier::EventMergeResult background_result;
        ImportResult excel_result;
        std::thread background_thread([&] {
            start.wait();
            background_result =
                webdossier::MergeOfficialEvent(background_db, patent, document);
        });
        std::thread excel_thread([&] {
            start.wait();
            ExcelIO io;
            excel_result = io.ImportPatents(xlsx_path, excel_db);
        });
        start_promise.set_value();
        background_thread.join();
        excel_thread.join();

        const auto records = background_db.GetOAByPatent(patent.geke_code);
        CHECK_EQ(records.size(), 1u);
        CHECK_STR_EQ(records.front().handler, "李四");
        CHECK_STR_EQ(records.front().issue_date, "2026-09-12");
        CHECK(excel_result.added + excel_result.updated == 1);
        CHECK(background_result.code == webdossier::ResultCode::NoChange ||
              background_result.code == webdossier::ResultCode::NewOfficialEvent);
    }

    std::filesystem::remove(xlsx_path);
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + "-wal");
    std::filesystem::remove(db_path + "-shm");
}
