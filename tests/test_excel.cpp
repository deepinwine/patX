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

