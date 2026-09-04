/**
 * patX Excel IO module
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include "database.hpp"

enum class SheetType {
    Patent,
    OA,
    PCT,
    Software,
    IC,
    Foreign,
    Unknown
};

struct SheetInfo {
    std::string name;
    int rows;
    int cols;
};

struct ImportResult {
    int added = 0;
    int updated = 0;
    int skipped = 0;
    int errors = 0;
    std::string type_summary; // summary of what was imported per sheet type
    std::vector<std::string> error_messages;
};

struct ExportOptions {
    bool include_headers = true;
    bool include_notes = true;
    std::string sheet_name = "PatentData";
    int start_row = 1;
    int start_col = 1;
};

// Generic export table used by every module's export button. ExcelIO writes
// a REAL OpenXLSX workbook for .xlsx paths and CSV text for .csv paths -
// never CSV content behind an .xlsx extension.
struct ExportTable {
    std::string sheet_name = "Data";
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;
};

class ExcelIO {
public:
    ExcelIO();
    ~ExcelIO();

    ImportResult ImportPatents(
        const std::string& file_path,
        Database& db,
        std::function<bool(int, int)> progress_callback = nullptr
    );

    // Module-agnostic exports. The extension of file_path selects the format.
    bool ExportXlsx(const ExportTable& table, const std::string& file_path);
    bool ExportCsv(const ExportTable& table, const std::string& file_path);

    // Convenience builders: current tab data -> ExportTable
    static ExportTable BuildPatentExport(const std::vector<Patent>& patents);
    static ExportTable BuildOAExport(const std::vector<OARecord>& records);
    static ExportTable BuildPCTExport(const std::vector<PCTPatent>& rows);
    static ExportTable BuildSoftwareExport(const std::vector<SoftwareCopyright>& rows);
    static ExportTable BuildICExport(const std::vector<ICLayout>& rows);
    static ExportTable BuildForeignExport(const std::vector<ForeignPatent>& rows);

    bool IsExcelFile(const std::string& file_path);
    bool IsCsvFile(const std::string& file_path);
    std::string GetLastError() const;

private:
    std::string last_error_;

    SheetType DetectSheetType(const std::string& sheet_name, const std::vector<std::string>& headers);
    int MapColumnToField(const std::string& column_name);
    int MapOAColumnToField(const std::string& column_name);
    int MapPCTColumnToField(const std::string& column_name);
    int MapSoftwareColumnToField(const std::string& column_name);
    int MapICColumnToField(const std::string& column_name);
    int MapForeignColumnToField(const std::string& column_name);

    ImportResult ImportPatentsFromCsv(
        const std::string& file_path,
        Database& db,
        std::function<bool(int, int)> progress_callback
    );

    ImportResult ImportPatentsFromXlsx(
        const std::string& file_path,
        Database& db,
        std::function<bool(int, int)> progress_callback
    );

    std::vector<std::string> ParseCsvLine(const std::string& line);
    std::string CleanValue(const std::string& value);
    std::string ParseDate(const std::string& value);
    bool IsValidGekeCode(const std::string& code);
    bool IsValidRowCode(const std::string& code);
    Patent MergePatents(const Patent& existing, const Patent& new_data);
    bool IsChanged(const Patent& old_data, const Patent& new_data);
};

ExcelIO& GetExcelIO();
