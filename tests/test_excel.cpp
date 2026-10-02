// Excel IO tests: real XLSX export (openable workbook with correct cells),
// CSV export quoting, structured-field import.
#include "test_registry.hpp"

#include "database.hpp"
#include "excel_io.hpp"

#include <OpenXLSX.hpp>
#include <filesystem>
#include <fstream>

using namespace testutil;

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
    // Windows 下不能删除被打开句柄占用的文件：读完先关闭再 remove
    std::string content;
    {
        std::ifstream f(path);
        content.assign((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
    }
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


// ---------------------------------------------------------------------------
// OA 导入预检（移植自 codex 分支设计：预检与执行分离、处理人保护）
// ---------------------------------------------------------------------------
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

TEST(oa_import_preview_message_includes_all_action_counts) {
    OAImportPreview preview;
    preview.added = 2;
    preview.handler_updates = 1;
    preview.unchanged = 4;
    preview.handler_conflicts = 3;
    preview.match_conflicts = 1;

    const std::string message = FormatOAImportPreview(preview);

    CHECK(message.find("新增 2") != std::string::npos);
    CHECK(message.find("自动更新处理人 1") != std::string::npos);
    CHECK(message.find("不变 4") != std::string::npos);
    CHECK(message.find("处理人冲突 3") != std::string::npos);
    CHECK(message.find("重复匹配冲突 1") != std::string::npos);
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
            xlsx_path, db, nullptr,
            [&](const OAImportPreview& value) {
                review_calls++;
                preview = value;
                return OAHandlerConflictPolicy::PreserveExisting;
            });
        records = db.GetOARecords();
    }
    std::filesystem::remove(xlsx_path);
    std::filesystem::remove(db_path);

    CHECK_EQ(review_calls, 1);
    CHECK_EQ(preview.added, 1);
    CHECK_EQ(result.added, 1);
    CHECK_EQ(records.size(), 1u);
    CHECK_STR_EQ(records.front().handler, "新处理人");
}

TEST(oa_excel_import_protects_existing_handler) {
    const std::string db_path = TempDbPath("oa_import_protect");
    std::filesystem::remove(db_path);
    {
        Database db(db_path);
        OARecord existing;
        existing.geke_code = "GK-OA-2";
        existing.oa_type = "第一次审查意见通知书";
        existing.issue_date = "2026-08-01";
        existing.handler = "原处理人";
        db.InsertOA(existing, false);

        const std::string xlsx_path = TempDbPath("oa_import_protect") + ".xlsx";
        std::filesystem::remove(xlsx_path);
        WriteOAWorkbook(xlsx_path, {{"GK-OA-2", "OA 导入专利",
                                     "第一次审查意见通知书",
                                     "2026-08-01", "2026-12-01", "新处理人"}});
        ExcelIO io;
        auto result = io.ImportPatents(xlsx_path, db, nullptr,
            [](const OAImportPreview&) {
                return OAHandlerConflictPolicy::PreserveExisting;
            });
        CHECK_EQ(result.handler_conflicts, 1);
        CHECK_EQ(result.added, 0);
        auto records = db.GetOARecords();
        CHECK_EQ(records.size(), 1u);
        CHECK_STR_EQ(records.front().handler, "原处理人");   // 未被覆盖
        std::filesystem::remove(xlsx_path);
    }
    std::filesystem::remove(db_path);
}

TEST(oa_excel_import_overwrite_policy_replaces_handler) {
    const std::string db_path = TempDbPath("oa_import_overwrite");
    std::filesystem::remove(db_path);
    {
        Database db(db_path);
        OARecord existing;
        existing.geke_code = "GK-OA-3";
        existing.oa_type = "1-OA";
        existing.issue_date = "2026-07-01";
        existing.handler = "原处理人";
        db.InsertOA(existing, false);

        const std::string xlsx_path = TempDbPath("oa_import_overwrite") + ".xlsx";
        std::filesystem::remove(xlsx_path);
        // 类型写法不同（1-OA vs 第一次审查意见通知书）也要规范化后匹配上
        WriteOAWorkbook(xlsx_path, {{"GK-OA-3", "OA 导入专利",
                                     "第一次审查意见通知书",
                                     "2026-07-01", "2026-11-01", "新处理人"}});
        ExcelIO io;
        auto result = io.ImportPatents(xlsx_path, db, nullptr,
            [](const OAImportPreview&) {
                return OAHandlerConflictPolicy::OverwriteWithExcel;
            });
        CHECK_EQ(result.updated, 1);
        auto records = db.GetOARecords();
        CHECK_EQ(records.size(), 1u);
        CHECK_STR_EQ(records.front().handler, "新处理人");   // 授权覆盖
        std::filesystem::remove(xlsx_path);
    }
    std::filesystem::remove(db_path);
}

TEST(oa_excel_import_fills_only_empty_handler) {
    const std::string db_path = TempDbPath("oa_import_fill");
    std::filesystem::remove(db_path);
    {
        Database db(db_path);
        OARecord existing;   // 无处理人
        existing.geke_code = "GK-OA-4";
        existing.oa_type = "第二次审查意见通知书";
        existing.issue_date = "2026-06-01";
        db.InsertOA(existing, false);

        const std::string xlsx_path = TempDbPath("oa_import_fill") + ".xlsx";
        std::filesystem::remove(xlsx_path);
        // 工作表内重复行（同身份两次）只算一条，处理人取有值那行
        WriteOAWorkbook(xlsx_path, {
            {"GK-OA-4", "", "二通", "2026-06-01", "", ""},
            {"GK-OA-4", "", "第二次审查意见通知书", "2026-06-01", "", "补填处理人"},
        });
        ExcelIO io;
        auto result = io.ImportPatents(xlsx_path, db, nullptr,
            [](const OAImportPreview&) {
                return OAHandlerConflictPolicy::PreserveExisting;
            });
        CHECK_EQ(result.updated, 1);
        auto records = db.GetOARecords();
        CHECK_EQ(records.size(), 1u);              // 重复行没有产生第二条
        CHECK_STR_EQ(records.front().handler, "补填处理人");
        std::filesystem::remove(xlsx_path);
    }
    std::filesystem::remove(db_path);
}
