/**
 * patX Excel Import/Export Implementation
 */

#include "excel_io.hpp"
#include "database.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <regex>
#include <iostream>

#include <OpenXLSX.hpp>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

// Debug log file
static std::ofstream debug_log;
static void DebugLog(const std::string& msg) {
    if (!debug_log.is_open()) {
        debug_log.open("patx_import_debug.txt", std::ios::out | std::ios::app);
    }
    debug_log << msg << std::endl;
    debug_log.flush();
}

ExcelIO::ExcelIO() = default;
ExcelIO::~ExcelIO() = default;

// Helper to get cell value as string (handles numbers, strings, empty)
static std::string CellToString(OpenXLSX::XLCellValue value) {
    if (value.type() == OpenXLSX::XLValueType::Empty) return "";
    if (value.type() == OpenXLSX::XLValueType::String) return value.get<std::string>();
    if (value.type() == OpenXLSX::XLValueType::Integer) return std::to_string(value.get<int64_t>());
    if (value.type() == OpenXLSX::XLValueType::Float) {
        double d = value.get<double>();
        // Check if it's actually an integer (no fractional part)
        if (d == static_cast<int64_t>(d) && d >= -9223372036854775807.0 && d <= 9223372036854775807.0) {
            return std::to_string(static_cast<int64_t>(d));
        }
        // For large numbers that might be application numbers (like 202510121074.1),
        // keep only 1 decimal digit to avoid floating-point precision artifacts
        // e.g., 202510121074.899993896484375 -> 202510121074.9
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);
        oss << d;
        std::string result = oss.str();
        // Remove trailing ".0" if it's actually an integer
        if (result.size() > 2 && result.substr(result.size() - 2) == ".0") {
            result = result.substr(0, result.size() - 2);
        }
        return result;
    }
    // Fallback: try string
    try { return value.get<std::string>(); }
    catch (...) { return ""; }
}

bool ExcelIO::IsExcelFile(const std::string& file_path) {
    auto pos = file_path.find_last_of('.');
    if (pos == std::string::npos) return false;
    std::string ext = file_path.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".xlsx" || ext == ".xls";
}

bool ExcelIO::IsCsvFile(const std::string& file_path) {
    auto pos = file_path.find_last_of('.');
    if (pos == std::string::npos) return false;
    std::string ext = file_path.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".csv" || ext == ".tsv";
}

// ===================== Sheet Type Detection =====================

SheetType ExcelIO::DetectSheetType(const std::string& sheet_name, const std::vector<std::string>& headers) {
    // Sheet名检测（优先级最高）
    std::string lower_sheet = sheet_name;
    std::transform(lower_sheet.begin(), lower_sheet.end(), lower_sheet.begin(), ::tolower);

    if (lower_sheet.find("pct") != std::string::npos) return SheetType::PCT;
    if (sheet_name.find("软著") != std::string::npos || sheet_name.find("软件") != std::string::npos || lower_sheet.find("software") != std::string::npos) return SheetType::Software;
    if (sheet_name.find("集成电路") != std::string::npos || lower_sheet.find("ic") != std::string::npos) return SheetType::IC;
    if (sheet_name.find("国外") != std::string::npos || sheet_name.find("海外") != std::string::npos || lower_sheet.find("foreign") != std::string::npos) return SheetType::Foreign;
    if (sheet_name.find("OA") != std::string::npos || sheet_name.find("oa") != std::string::npos || lower_sheet.find("审查意见") != std::string::npos) return SheetType::OA;

    // 计算专利和OA的匹配分数（而非优先级）
    int patent_hits = 0;
    int oa_hits = 0;

    // OA专有关键词（权重高，表示一定是OA表）
    std::vector<std::string> oa_strong = {"OA性质", "审查意见摘要", "官方期限", "绝限", "审通", "通知书",
        "oa_type", "official_deadline", "审查意见", "发文日", "答复", "期限"};
    // 专利专有关键词（含通用编码关键词，支持任意公司）
    std::vector<std::string> patent_strong = {"编码", "编号", "申请号", "发明名称", "提案名称", "授权日", "到期日",
        "我司编码", "内部编码", "geke_code", "application_number", "proposal", "authorization", "expiration"};
    // 通用关键词（两边都可能匹配）
    std::vector<std::string> common_cols = {"名称", "处理人", "状态", "备注", "类型",
        "title", "handler", "status", "note", "type"};

    for (const auto& h : headers) {
        std::string lower_h = h;
        std::transform(lower_h.begin(), lower_h.end(), lower_h.begin(), ::tolower);

        for (const auto& kw : oa_strong) {
            std::string lower_kw = kw;
            std::transform(lower_kw.begin(), lower_kw.end(), lower_kw.begin(), ::tolower);
            if (lower_h.find(lower_kw) != std::string::npos) { oa_hits += 3; break; }
        }
        for (const auto& kw : patent_strong) {
            std::string lower_kw = kw;
            std::transform(lower_kw.begin(), lower_kw.end(), lower_kw.begin(), ::tolower);
            if (lower_h.find(lower_kw) != std::string::npos) { patent_hits += 2; break; }
        }
        for (const auto& kw : common_cols) {
            std::string lower_kw = kw;
            std::transform(lower_kw.begin(), lower_kw.end(), lower_kw.begin(), ::tolower);
            if (lower_h.find(lower_kw) != std::string::npos) { patent_hits++; oa_hits++; break; }
        }
    }

    if (oa_hits > patent_hits) return SheetType::OA;
    if (patent_hits > 0) return SheetType::Patent;
    if (oa_hits > 0) return SheetType::OA;

    // 默认专利
    return SheetType::Patent;
}

// ===================== Column Mapping =====================

int ExcelIO::MapColumnToField(const std::string& column_name) {
    // Patent columns (matching Python version)
    // 我司编码 申请号 提案名称 发明名称 原申请人 现申请人 申请状态 关联案信息 缴费状态
    // 研发项目 一级（新） 二级（新） 三级（新） 四级（新） 标签 具体内容 重要级 立案日
    // 申请日 授权日 技术交底书撰写人 研发部门 发明人 处理人 类型 事务所
    // 代理人编码 代理人 备注 无形资产评估 内部研发项目 技术路线 项目ID
    // 1st OA 2nd OA 3rd OA 4th OA 5OA 复审 浦东资助情况 PCT提醒

    // Field mapping: 1=geke_code, 2=application_number, 3=title, 4=current_applicant,
    // 5=application_date, 6=patent_type, 7=application_status, 8=inventor,
    // 9-11=class_level1-3, 12=patent_level, 13=geke_handler, 14=notes,
    // 15=proposal_name, 16=original_applicant, 17=authorization_date,
    // 18=expiration_date, 19=rd_department, 20=agency_firm

    // 编码列识别：支持任意公司（华为编码、小米编码等）及通用名称
    if (column_name == "编码" || column_name == "编号" || column_name == "专利编号" ||
        column_name.find("我司编码") != std::string::npos ||
        column_name.find("内部编码") != std::string::npos ||
        column_name.find("格科编码") != std::string::npos ||
        (column_name.find("编码") != std::string::npos && column_name.find("分类编码") == std::string::npos)) return 1;
    if (column_name.find("申请号") != std::string::npos && column_name.find("PCT") == std::string::npos &&
        column_name.find("国家") == std::string::npos && column_name.find("国内") == std::string::npos) return 2;
    if (column_name.find("提案名称") != std::string::npos) return 15;
    if (column_name.find("发明名称") != std::string::npos || column_name.find("专利名称") != std::string::npos ||
        column_name.find("名称") != std::string::npos) return 3;
    if (column_name.find("原申请人") != std::string::npos) return 16;
    if (column_name.find("现申请人") != std::string::npos || column_name.find("申请人") != std::string::npos) return 4;
    if (column_name.find("申请状态") != std::string::npos || column_name.find("状态") != std::string::npos ||
        column_name.find("法律状态") != std::string::npos) return 7;
    if (column_name.find("重要级") != std::string::npos || column_name.find("专利等级") != std::string::npos ||
        column_name.find("等级") != std::string::npos) return 12;
    if (column_name.find("立案日") != std::string::npos) return 5;  // use as app date if no 申请日
    if (column_name.find("申请日") != std::string::npos || column_name.find("申请日期") != std::string::npos) return 5;
    if (column_name.find("授权日") != std::string::npos || column_name.find("授权日期") != std::string::npos) return 17;
    if (column_name.find("到期日") != std::string::npos || column_name.find("届满") != std::string::npos) return 18;
    if (column_name.find("类型") != std::string::npos || column_name.find("专利类型") != std::string::npos) return 6;
    if (column_name.find("发明人") != std::string::npos || column_name.find("设计人") != std::string::npos) return 8;
    if (column_name.find("一级") != std::string::npos && column_name.find("分类") != std::string::npos) return 9;
    if (column_name.find("二级") != std::string::npos && column_name.find("分类") != std::string::npos) return 10;
    if (column_name.find("三级") != std::string::npos && column_name.find("分类") != std::string::npos) return 11;
    if (column_name.find("处理人") != std::string::npos || column_name.find("负责人") != std::string::npos ||
        column_name.find("IPR") != std::string::npos || column_name.find("经办人") != std::string::npos) return 13;
    if (column_name.find("研发部门") != std::string::npos) return 19;
    if (column_name.find("事务所") != std::string::npos || column_name.find("代理") != std::string::npos) return 20;
    if (column_name.find("备注") != std::string::npos || column_name.find("具体内容") != std::string::npos) return 14;

    // English fallback
    std::string lower = column_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("code") != std::string::npos || lower.find("geke") != std::string::npos) return 1;
    if (lower.find("application") != std::string::npos && lower.find("number") != std::string::npos) return 2;
    if (lower.find("title") != std::string::npos || lower.find("name") != std::string::npos) return 3;
    if (lower.find("applicant") != std::string::npos) return 4;
    if (lower.find("date") != std::string::npos) return 5;
    if (lower.find("type") != std::string::npos) return 6;
    if (lower.find("status") != std::string::npos) return 7;
    if (lower.find("inventor") != std::string::npos) return 8;
    if (lower.find("level") != std::string::npos) return 12;
    if (lower.find("handler") != std::string::npos) return 13;
    if (lower.find("note") != std::string::npos) return 14;

    return 0;
}

int ExcelIO::MapOAColumnToField(const std::string& column_name) {
    if (column_name == "编码" || column_name == "编号" || column_name == "专利编号" ||
        column_name.find("我司编号") != std::string::npos ||
        column_name.find("我司编码") != std::string::npos ||
        column_name.find("内部编码") != std::string::npos ||
        (column_name.find("编码") != std::string::npos && column_name.find("分类编码") == std::string::npos)) return 1;
    if (column_name.find("专利名称") != std::string::npos) return 2;
    // OA性质 = OA类型 (5-OA, 驳回, 授权等)
    if (column_name == "OA性质" || column_name.find("OA类型") != std::string::npos) return 3;
    // 答辩性质/答复性质 = 进度状态
    if (column_name.find("答辩性质") != std::string::npos || column_name.find("答复性质") != std::string::npos) return 6;
    if (column_name.find("官方期限") != std::string::npos || column_name.find("期限") != std::string::npos) return 4;
    if (column_name.find("处理人") != std::string::npos) return 5;
    if (column_name.find("进度") != std::string::npos || column_name.find("状态") != std::string::npos) return 6;
    if (column_name.find("审查意见摘要") != std::string::npos || column_name.find("摘要") != std::string::npos) return 7;
    if (column_name.find("事务所") != std::string::npos) return 8;
    if (column_name.find("更新日期") != std::string::npos || column_name.find("发文日") != std::string::npos) return 9;

    std::string lower = column_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("code") != std::string::npos || lower.find("geke") != std::string::npos) return 1;
    if (lower.find("title") != std::string::npos) return 2;
    if (lower.find("deadline") != std::string::npos) return 4;
    if (lower.find("handler") != std::string::npos) return 5;
    if (lower.find("progress") != std::string::npos) return 6;
    if (lower.find("agency") != std::string::npos) return 8;

    return 0;
}

// PCT field mapping: 1=geke_code, 2=domestic_source, 3=application_no, 4=country_app_no,
//   5=title, 6=application_status, 7=filing_date, 8=application_date, 9=priority_date,
//   10=inventor, 11=handler
int ExcelIO::MapPCTColumnToField(const std::string& column_name) {
    // 编码列识别：支持任意公司及通用名称
    if (column_name == "编码" || column_name == "编号" || column_name == "专利编号" ||
        column_name.find("我司编码") != std::string::npos ||
        column_name.find("内部编码") != std::string::npos ||
        column_name.find("格科编码") != std::string::npos ||
        (column_name.find("编码") != std::string::npos && column_name.find("分类编码") == std::string::npos)) return 1;
    if (column_name.find("国内同源") != std::string::npos) return 2;
    if (column_name.find("国内申请号") != std::string::npos) return 2; // also domestic source
    if (column_name.find("申请号") != std::string::npos && column_name.find("国家") == std::string::npos) return 3;
    if (column_name.find("国家申请号") != std::string::npos) return 4;
    if (column_name.find("发明名称") != std::string::npos) return 5;
    if (column_name.find("申请状态") != std::string::npos) return 6;
    if (column_name.find("立案日") != std::string::npos) return 7;
    if (column_name.find("申请日") != std::string::npos) return 8;
    if (column_name.find("优先权日") != std::string::npos) return 9;
    if (column_name.find("发明人") != std::string::npos) return 10;
    if (column_name.find("处理人") != std::string::npos) return 11;
    if (column_name.find("类型") != std::string::npos) return 12; // country type
    if (column_name.find("授权日") != std::string::npos) return 13;

    std::string lower = column_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("code") != std::string::npos || lower.find("geke") != std::string::npos) return 1;
    if (lower.find("priority") != std::string::npos) return 9;
    if (lower.find("inventor") != std::string::npos) return 10;
    if (lower.find("handler") != std::string::npos) return 11;
    if (lower.find("title") != std::string::npos) return 5;
    if (lower.find("status") != std::string::npos) return 6;

    return 0;
}

// Software field mapping: 1=case_no, 2=reg_no, 3=title, 4=original_owner,
//   5=current_owner, 6=application_status, 7=dev_complete_date, 8=application_date,
//   9=reg_date, 10=handler
int ExcelIO::MapSoftwareColumnToField(const std::string& column_name) {
    if (column_name.find("案号") != std::string::npos) return 1;
    if (column_name.find("登记号") != std::string::npos) return 2;
    if (column_name.find("发明名称") != std::string::npos || column_name.find("名称") != std::string::npos) return 3;
    if (column_name.find("原权利人") != std::string::npos) return 4;
    if (column_name.find("现权利人") != std::string::npos) return 5;
    if (column_name.find("申请状态") != std::string::npos) return 6;
    if (column_name.find("开发完成") != std::string::npos) return 7;
    if (column_name.find("申请日") != std::string::npos) return 8;
    if (column_name.find("登记日期") != std::string::npos || column_name.find("登记日") != std::string::npos) return 9;
    if (column_name.find("处理人") != std::string::npos) return 10;
    if (column_name.find("首次发表") != std::string::npos) return 11;

    return 0;
}

// IC field mapping: 1=case_no, 2=reg_no, 3=title, 4=original_owner,
//   5=current_owner, 6=application_status, 7=application_date, 8=creation_date,
//   9=cert_date, 10=inventor, 11=handler, 12=notes
int ExcelIO::MapICColumnToField(const std::string& column_name) {
    if (column_name.find("案号") != std::string::npos) return 1;
    if (column_name.find("布图设计登记号") != std::string::npos || column_name.find("登记号") != std::string::npos) return 2;
    if (column_name.find("发明名称") != std::string::npos || column_name.find("名称") != std::string::npos) return 3;
    if (column_name.find("原申请人") != std::string::npos) return 4;
    if (column_name.find("现申请人") != std::string::npos) return 5;
    if (column_name.find("申请状态") != std::string::npos) return 6;
    if (column_name.find("申请日") != std::string::npos) return 7;
    if (column_name.find("创作完成日") != std::string::npos) return 8;
    if (column_name.find("布图设计颁证日") != std::string::npos || column_name.find("颁证日") != std::string::npos) return 9;
    if (column_name.find("发明人") != std::string::npos) return 10;
    if (column_name.find("处理人") != std::string::npos) return 11;
    if (column_name.find("具体内容") != std::string::npos) return 12;
    if (column_name.find("事务所") != std::string::npos) return 13;
    if (column_name.find("应用产品") != std::string::npos) return 14;
    if (column_name.find("内部研发") != std::string::npos) return 15;

    return 0;
}

// Foreign field mapping: 1=case_no, 2=owner, 3=current_owner, 4=patent_status,
//   5=application_no, 6=country, 7=title, 8=application_date, 9=authorization_date,
//   10=handler, 11=notes
int ExcelIO::MapForeignColumnToField(const std::string& column_name) {
    if (column_name.find("我方编号") != std::string::npos) return 1;
    if (column_name.find("权利人") != std::string::npos && column_name.find("现") == std::string::npos) return 2;
    if (column_name.find("现权利人") != std::string::npos) return 3;
    if (column_name.find("专利状态") != std::string::npos || column_name.find("申请状态") != std::string::npos) return 4;
    if (column_name.find("授权专利号") != std::string::npos) return 5;
    if (column_name.find("申请号") != std::string::npos) return 6;
    if (column_name.find("国别") != std::string::npos) return 7;
    if (column_name.find("发明名称") != std::string::npos || column_name.find("名称") != std::string::npos) return 8;
    if (column_name.find("申请日") != std::string::npos) return 9;
    if (column_name.find("授权日") != std::string::npos) return 10;
    if (column_name.find("优先权日") != std::string::npos) return 11;
    if (column_name.find("处理人") != std::string::npos) return 12;
    if (column_name.find("代理所") != std::string::npos || column_name.find("代理人") != std::string::npos) return 13;
    if (column_name.find("备注") != std::string::npos) return 14;
    if (column_name.find("转让日期") != std::string::npos) return 15;
    if (column_name.find("公开日") != std::string::npos) return 16;

    std::string lower = column_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("country") != std::string::npos) return 7;
    if (lower.find("title") != std::string::npos) return 8;
    if (lower.find("status") != std::string::npos) return 4;
    if (lower.find("handler") != std::string::npos) return 12;
    if (lower.find("note") != std::string::npos) return 14;

    return 0;
}

// ===================== Import Entry =====================

ImportResult ExcelIO::ImportPatents(
    const std::string& file_path,
    Database& db,
    std::function<bool(int, int)> progress_callback
) {
    ImportResult result;
    try {
        if (IsCsvFile(file_path)) {
            result = ImportPatentsFromCsv(file_path, db, progress_callback);
        } else if (IsExcelFile(file_path)) {
            result = ImportPatentsFromXlsx(file_path, db, progress_callback);
        } else {
            last_error_ = "Unsupported file format";
        }
    } catch (const std::exception& e) {
        last_error_ = e.what();
    }
    return result;
}

// ===================== CSV Import =====================

ImportResult ExcelIO::ImportPatentsFromCsv(
    const std::string& file_path,
    Database& db,
    std::function<bool(int, int)> progress_callback
) {
    ImportResult result;
    std::ifstream file(file_path);
    if (!file.is_open()) {
        last_error_ = "Cannot open file: " + file_path;
        return result;
    }

    std::string line;
    std::vector<int> field_map;
    int row_num = 0;

    if (std::getline(file, line)) {
        auto headers = ParseCsvLine(line);
        for (const auto& h : headers) {
            field_map.push_back(MapColumnToField(h));
        }
    }

    db.BeginBatch();

    while (std::getline(file, line)) {
        row_num++;
        auto row = ParseCsvLine(line);
        if (row.empty() || row[0].empty()) continue;

        std::string geke_code = CleanValue(row[0]);
        if (!IsValidGekeCode(geke_code)) continue;

        Patent p;
        p.geke_code = geke_code;

        for (size_t i = 0; i < row.size() && i < field_map.size(); i++) {
            const std::string& value = CleanValue(row[i]);
            int field = field_map[i];
            switch (field) {
                case 2: p.application_number = value; break;
                case 3: p.title = value; break;
                case 5: p.application_date = ParseDate(value); break;
                case 6: p.patent_type = value; break;
                case 7: p.application_status = value; break;
                case 8: p.inventor = value; break;
                case 15: p.patent_level = value; break;
                case 16: p.geke_handler = value; break;
                case 17: p.notes = value; break;
            }
        }

        if (p.patent_type.empty()) p.patent_type = "invention";
        if (p.patent_level.empty()) p.patent_level = "normal";
        if (p.application_status.empty()) p.application_status = "pending";

        Patent existing = db.GetPatentByCode(p.geke_code);
        if (existing.id > 0) {
            Patent merged = MergePatents(existing, p);
            if (IsChanged(existing, merged)) {
                db.UpdatePatent(existing.id, merged);
                result.updated++;
            } else {
                result.skipped++;
            }
        } else {
            db.InsertPatent(p);
            result.added++;
        }

        if (progress_callback && (row_num % 100 == 0)) {
            progress_callback(row_num, row_num + 100);
        }
    }

    return result;
}

// ===================== XLSX Import =====================

ImportResult ExcelIO::ImportPatentsFromXlsx(
    const std::string& file_path,
    Database& db,
    std::function<bool(int, int)> progress_callback
) {
    ImportResult result;

    try {
        // OpenXLSX fails with non-ASCII paths on Windows. Copy to temp.
        std::string temp_path = file_path;
        bool use_temp = false;

#ifdef _WIN32
        wchar_t temp_dir[MAX_PATH];
        GetTempPathW(MAX_PATH, temp_dir);
        std::wstring temp_path_w = std::wstring(temp_dir) + L"patx_import_temp.xlsx";
        temp_path = std::filesystem::path(temp_path_w).string();

        int len = MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), -1, nullptr, 0);
        std::wstring wpath(len, 0);
        MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), -1, &wpath[0], len);

        if (!CopyFileW(wpath.c_str(), temp_path_w.c_str(), FALSE)) {
            last_error_ = "Failed to copy file to temp directory";
            return result;
        }
        use_temp = true;
#else
        bool has_non_ascii = false;
        for (unsigned char c : file_path) {
            if (c > 127) { has_non_ascii = true; break; }
        }
        if (has_non_ascii) {
            std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
            temp_path = (temp_dir / "patx_import_temp.xlsx").string();
            std::filesystem::copy_file(file_path, temp_path, std::filesystem::copy_options::overwrite_existing);
            use_temp = true;
        }
#endif

        OpenXLSX::XLDocument doc(temp_path);
        auto wb = doc.workbook();

        int total_sheets = static_cast<int>(wb.worksheetNames().size());
        int sheet_idx = 0;

        for (const auto& sheet_name : wb.worksheetNames()) {
            sheet_idx++;
            auto ws = wb.worksheet(sheet_name);
            uint32_t rowCount = ws.rowCount();
            if (rowCount < 2) continue;

            // Read headers
            std::vector<std::string> headers;
            uint16_t colCount = 50;
            for (uint16_t col = 1; col <= colCount; col++) {
                std::string header = CellToString(ws.cell(1, col).value());
                headers.push_back(header);
                if (header.empty() && col > 5) {
                    colCount = col - 1;
                    headers.pop_back();
                    break;
                }
            }

            SheetType type = DetectSheetType(sheet_name, headers);

            // Debug输出
            std::string type_str;
            switch (type) {
                case SheetType::Patent: type_str = "PATENT"; break;
                case SheetType::OA: type_str = "OA"; break;
                case SheetType::PCT: type_str = "PCT"; break;
                case SheetType::Software: type_str = "Software"; break;
                case SheetType::IC: type_str = "IC"; break;
                case SheetType::Foreign: type_str = "Foreign"; break;
                default: type_str = "Unknown"; break;
            }
            DebugLog("\n[Import] Sheet '" + sheet_name + "' detected as: " + type_str);

            std::string hdr_str = "Headers: ";
            for (size_t i = 0; i < headers.size() && i < 10; i++) hdr_str += "[" + headers[i] + "] ";
            DebugLog(hdr_str);

            // Count for this sheet
            int sheet_added = 0, sheet_updated = 0, sheet_skipped = 0;

            if (type == SheetType::OA) {
                std::vector<int> field_map;
                for (uint16_t col = 0; col < colCount; col++) {
                    field_map.push_back(MapOAColumnToField(headers[col]));
                }

                db.BeginBatch();
                for (uint32_t row = 2; row <= rowCount; row++) {
                    OARecord oa;
                    for (uint16_t col = 0; col < colCount; col++) {
                        std::string raw = CellToString(ws.cell(row, col + 1).value());
                        if (raw.empty()) continue;
                        std::string value = CleanValue(raw);
                        int field = field_map[col];
                        switch (field) {
                            case 1: oa.geke_code = value; break;
                            case 2: oa.patent_title = value; break;
                            case 3: oa.oa_type = value; break;
                            case 4: oa.official_deadline = ParseDate(value); break;
                            case 5: oa.handler = value; break;
                            case 6: oa.progress = value; break;
                            case 7: oa.oa_summary = value; break;
                            case 8: oa.agency = value; break;
                            case 9: oa.issue_date = ParseDate(value); break;
                        }
                    }
                    if (oa.geke_code.empty() || !IsValidGekeCode(oa.geke_code)) continue;
                    db.InsertOA(oa);
                    sheet_added++;
                }
                result.type_summary += "OA: " + std::to_string(sheet_added) + " added; ";

            } else if (type == SheetType::PCT) {
                std::vector<int> field_map;
                for (uint16_t col = 0; col < colCount; col++) {
                    field_map.push_back(MapPCTColumnToField(headers[col]));
                }

                db.BeginBatch();
                for (uint32_t row = 2; row <= rowCount; row++) {
                    PCTPatent pct;
                    for (uint16_t col = 0; col < colCount; col++) {
                        std::string raw = CellToString(ws.cell(row, col + 1).value());
                        if (raw.empty()) continue;
                        std::string value = CleanValue(raw);
                        int field = field_map[col];
                        switch (field) {
                            case 1: pct.geke_code = value; break;
                            case 2: pct.domestic_source = value; break;
                            case 3: pct.application_no = value; break;
                            case 4: pct.country_app_no = value; break;
                            case 5: pct.title = value; break;
                            case 6: pct.application_status = value; break;
                            case 7: pct.filing_date = ParseDate(value); break;
                            case 8: pct.application_date = ParseDate(value); break;
                            case 9: pct.priority_date = ParseDate(value); break;
                            case 10: pct.inventor = value; break;
                            case 11: pct.handler = value; break;
                        }
                    }
                    if (pct.geke_code.empty() || !IsValidGekeCode(pct.geke_code)) continue;
                    db.InsertPCT(pct);
                    sheet_added++;
                }
                result.type_summary += "PCT: " + std::to_string(sheet_added) + " added; ";

            } else if (type == SheetType::Software) {
                std::vector<int> field_map;
                for (uint16_t col = 0; col < colCount; col++) {
                    field_map.push_back(MapSoftwareColumnToField(headers[col]));
                }

                db.BeginBatch();
                for (uint32_t row = 2; row <= rowCount; row++) {
                    SoftwareCopyright sw;
                    for (uint16_t col = 0; col < colCount; col++) {
                        std::string raw = CellToString(ws.cell(row, col + 1).value());
                        if (raw.empty()) continue;
                        std::string value = CleanValue(raw);
                        int field = field_map[col];
                        switch (field) {
                            case 1: sw.case_no = value; break;
                            case 2: sw.reg_no = value; break;
                            case 3: sw.title = value; break;
                            case 4: sw.original_owner = value; break;
                            case 5: sw.current_owner = value; break;
                            case 6: sw.application_status = value; break;
                            case 7: sw.dev_complete_date = ParseDate(value); break;
                            case 8: sw.application_date = ParseDate(value); break;
                            case 9: sw.reg_date = ParseDate(value); break;
                            case 10: sw.handler = value; break;
                        }
                    }
                    if (sw.case_no.empty() && sw.title.empty()) continue;
                    if (!IsValidRowCode(sw.case_no) && !IsValidRowCode(sw.reg_no)) continue;
                    db.InsertSoftware(sw);
                    sheet_added++;
                }
                result.type_summary += "Software: " + std::to_string(sheet_added) + " added; ";

            } else if (type == SheetType::IC) {
                std::vector<int> field_map;
                for (uint16_t col = 0; col < colCount; col++) {
                    field_map.push_back(MapICColumnToField(headers[col]));
                }

                db.BeginBatch();
                for (uint32_t row = 2; row <= rowCount; row++) {
                    ICLayout ic;
                    for (uint16_t col = 0; col < colCount; col++) {
                        std::string raw = CellToString(ws.cell(row, col + 1).value());
                        if (raw.empty()) continue;
                        std::string value = CleanValue(raw);
                        int field = field_map[col];
                        switch (field) {
                            case 1: ic.case_no = value; break;
                            case 2: ic.reg_no = value; break;
                            case 3: ic.title = value; break;
                            case 4: ic.original_owner = value; break;
                            case 5: ic.current_owner = value; break;
                            case 6: ic.application_status = value; break;
                            case 7: ic.application_date = ParseDate(value); break;
                            case 8: ic.creation_date = ParseDate(value); break;
                            case 9: ic.cert_date = ParseDate(value); break;
                            case 10: ic.inventor = value; break;
                            case 11: ic.handler = value; break;
                            case 12:
                                if (ic.notes.empty()) ic.notes = value;
                                else ic.notes += "; " + value;
                                break;
                            case 13:
                                if (ic.notes.empty()) ic.notes = value;
                                else ic.notes += "; 事务所: " + value;
                                break;
                        }
                    }
                    if (ic.case_no.empty() && ic.title.empty()) continue;
                    if (!IsValidRowCode(ic.case_no) && !IsValidRowCode(ic.reg_no)) continue;
                    db.InsertIC(ic);
                    sheet_added++;
                }
                result.type_summary += "IC: " + std::to_string(sheet_added) + " added; ";

            } else if (type == SheetType::Foreign) {
                std::vector<int> field_map;
                for (uint16_t col = 0; col < colCount; col++) {
                    field_map.push_back(MapForeignColumnToField(headers[col]));
                }

                db.BeginBatch();
                for (uint32_t row = 2; row <= rowCount; row++) {
                    ForeignPatent fp;
                    for (uint16_t col = 0; col < colCount; col++) {
                        std::string raw = CellToString(ws.cell(row, col + 1).value());
                        if (raw.empty()) continue;
                        std::string value = CleanValue(raw);
                        int field = field_map[col];
                        switch (field) {
                            case 1: fp.case_no = value; break;
                            case 2: fp.owner = value; break;
                            case 3:
                                if (fp.owner.empty()) fp.owner = value;
                                break;
                            case 4: fp.patent_status = value; break;
                            case 5: // authorized patent number
                                if (fp.application_no.empty()) fp.application_no = value;
                                break;
                            case 6: fp.application_no = value; break;
                            case 7: fp.country = value; break;
                            case 8: fp.title = value; break;
                            case 9: fp.application_date = ParseDate(value); break;
                            case 10: fp.authorization_date = ParseDate(value); break;
                            case 11: // priority date - store in notes
                                break;
                            case 12: fp.handler = value; break;
                            case 14:
                                if (fp.notes.empty()) fp.notes = value;
                                else fp.notes += "; " + value;
                                break;
                        }
                    }
                    // For foreign patents, accept any row with title or case_no
                    if (fp.title.empty() && fp.case_no.empty()) continue;
                    if (!IsValidRowCode(fp.case_no) && !IsValidRowCode(fp.application_no)) continue;
                    if (fp.case_no.empty()) fp.case_no = "F-" + std::to_string(row);
                    db.InsertForeign(fp);
                    sheet_added++;
                }
                result.type_summary += "Foreign: " + std::to_string(sheet_added) + " added; ";

            } else if (type == SheetType::Patent) {
                std::vector<int> field_map;
                int code_col = -1;  // Find the column with geke_code
                for (uint16_t col = 0; col < colCount; col++) {
                    int field = MapColumnToField(headers[col]);
                    field_map.push_back(field);
                    if (field == 1) code_col = col;  // field 1 = geke_code
                    DebugLog("  Col " + std::to_string(col) + " [" + headers[col] + "] -> field " + std::to_string(field));
                }

                // If no geke_code column found, use first column
                if (code_col < 0) {
                    code_col = 0;
                    DebugLog("  No geke_code column found, using col 0");
                } else {
                    DebugLog("  geke_code column = " + std::to_string(code_col) + " (header: " + headers[code_col] + ")");
                }

                db.BeginBatch();
                int valid_count = 0, invalid_count = 0;
                for (uint32_t row = 2; row <= rowCount && row <= 10; row++) {  // debug first 10 rows
                    std::string geke_code = CleanValue(CellToString(ws.cell(row, code_col + 1).value()));
                    bool valid = IsValidGekeCode(geke_code);
                    DebugLog("  Row " + std::to_string(row) + ": geke_code=[" + geke_code + "] valid=" + (valid ? "true" : "false"));
                    if (valid) valid_count++; else invalid_count++;
                }
                DebugLog("  Summary: valid=" + std::to_string(valid_count) + " invalid=" + std::to_string(invalid_count) + " total_rows=" + std::to_string(rowCount));

                for (uint32_t row = 2; row <= rowCount; row++) {
                    std::string geke_code = CleanValue(CellToString(ws.cell(row, code_col + 1).value()));
                    if (!IsValidGekeCode(geke_code)) continue;

                    // Debug first 3 insertions
                    if (sheet_added < 3) {
                        DebugLog("  Inserting row " + std::to_string(row) + ": geke_code=" + geke_code);
                    }

                    Patent p;
                    p.geke_code = geke_code;

                    for (uint16_t col = 0; col < colCount; col++) {
                        std::string raw = CellToString(ws.cell(row, col + 1).value());
                        if (raw.empty()) continue;
                        std::string value = CleanValue(raw);
                        int field = field_map[col];
                        switch (field) {
                            case 1: p.geke_code = value; break;
                            case 2: p.application_number = value; break;
                            case 3: p.title = value; break;
                            case 4:
                                if (p.current_applicant.empty()) p.current_applicant = value;
                                break;
                            case 5: p.application_date = ParseDate(value); break;
                            case 6: p.patent_type = value; break;
                            case 7: p.application_status = value; break;
                            case 8: p.inventor = value; break;
                            case 9: p.class_level1 = value; break;
                            case 10: p.class_level2 = value; break;
                            case 11: p.class_level3 = value; break;
                            case 12: p.patent_level = value; break;
                            case 13: p.geke_handler = value; break;
                            case 14:
                                if (p.notes.empty()) p.notes = value;
                                else p.notes += "; " + value;
                                break;
                            case 15: p.proposal_name = value; break;
                            case 16: p.original_applicant = value; break;
                            case 17: p.authorization_date = ParseDate(value); break;
                            case 18: p.expiration_date = ParseDate(value); break;
                            case 19: p.rd_department = value; break;
                            case 20: p.agency_firm = value; break;
                        }
                    }

                    if (p.patent_type.empty()) p.patent_type = "invention";
                    if (p.application_status.empty()) p.application_status = "pending";

                    Patent existing = db.GetPatentByCode(p.geke_code);
                    if (existing.id > 0) {
                        Patent merged = MergePatents(existing, p);
                        if (IsChanged(existing, merged)) {
                            db.UpdatePatent(existing.id, merged);
                            sheet_updated++;
                        } else {
                            sheet_skipped++;
                        }
                    } else {
                        db.InsertPatent(p);
                        sheet_added++;
                    }
                }
                result.type_summary += "Patent: " + std::to_string(sheet_added) + " added, " +
                    std::to_string(sheet_updated) + " updated; ";

            } else {
                // Unknown type - skip
                result.type_summary += sheet_name + ": skipped (unknown type); ";
            }

            result.added += sheet_added;
            result.updated += sheet_updated;
            result.skipped += sheet_skipped;

            if (progress_callback) {
                progress_callback(sheet_idx * 100, total_sheets * 100);
            }
        }

        doc.close();

        if (use_temp) {
            std::filesystem::remove(temp_path);
        }
    } catch (const std::exception& e) {
        last_error_ = e.what();
    }

    return result;
}

// ===================== Utility Functions =====================

std::vector<std::string> ExcelIO::ParseCsvLine(const std::string& line) {
    std::vector<std::string> result;
    std::string cell;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                cell += '"';
                i++;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (c == ',' && !in_quotes) {
            result.push_back(cell);
            cell.clear();
        } else {
            cell += c;
        }
    }
    result.push_back(cell);
    return result;
}

std::string ExcelIO::CleanValue(const std::string& value) {
    std::string result = value;
    size_t start = result.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = result.find_last_not_of(" \t\n\r");
    result = result.substr(start, end - start + 1);

    if (result == "nan" || result == "NaN" || result == "null" || result == "NULL") {
        return "";
    }
    return result;
}

std::string ExcelIO::ParseDate(const std::string& value) {
    if (value.empty()) return "";

    std::regex patterns[] = {
        std::regex(R"((\d{4})-(\d{1,2})-(\d{1,2}))"),
        std::regex(R"((\d{4})/(\d{1,2})/(\d{1,2}))"),
        std::regex(R"((\d{4})\.(\d{1,2})\.(\d{1,2}))"),
    };

    for (const auto& pattern : patterns) {
        std::smatch match;
        if (std::regex_match(value, match, pattern)) {
            std::string year = match[1].str();
            std::string month = match[2].str();
            std::string day = match[3].str();
            if (month.size() == 1) month = "0" + month;
            if (day.size() == 1) day = "0" + day;
            return year + "-" + month + "-" + day;
        }
    }

    // Handle Excel serial date numbers (days since 1900-01-01)
    // e.g. 44927 -> 2023-01-01
    try {
        double serial = std::stod(value);
        if (serial > 40000 && serial < 60000) {
            // Excel epoch: 1900-01-01 = 1 (with the infamous 1900 leap year bug)
            int days = static_cast<int>(serial) - 2; // adjust for Excel bug
            int year = 1900, month = 1, day = 1;
            // Simplified: days from 1900-01-01
            // Use a simple algorithm
            int y = 1900;
            while (true) {
                int days_in_year = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
                if (days < days_in_year) break;
                days -= days_in_year;
                y++;
            }
            year = y;
            int md[] = {31, (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 29 : 28,
                        31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            month = 1;
            while (month < 12 && days >= md[month - 1]) {
                days -= md[month - 1];
                month++;
            }
            day = days + 1;
            char buf[16];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
            return std::string(buf);
        }
    } catch (...) {}

    return value.size() > 10 ? value.substr(0, 10) : value;
}

bool ExcelIO::IsValidGekeCode(const std::string& code) {
    if (code.empty()) return false;

    // Reject if mostly Chinese characters (likely a label/header, not a code)
    int ch_count = 0, alnum_count = 0;
    for (size_t i = 0; i < code.size(); ) {
        unsigned char c = (unsigned char)code[i];
        if (c >= 0xE0 && i + 2 < code.size()) { ch_count++; i += 3; }
        else if (std::isalnum(c)) { alnum_count++; i++; }
        else { i++; }
    }
    // If more Chinese chars than alnum, it's a label not a code
    if (ch_count > alnum_count) return false;

    // Must contain at least some alphanumeric chars (letter+digit combination)
    if (alnum_count < 1) return false;

    // Exclude pure keywords
    std::string lower = code;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::string exclude_kw[] = {"total", "sum", "note", "status", "type", "header",
        "合计", "总计", "备注", "说明", "nan", "null", "none", "n/a"};
    for (const auto& kw : exclude_kw) {
        if (lower == kw) return false;
    }

    return true;
}

bool ExcelIO::IsValidRowCode(const std::string& code) {
    if (code.empty()) return false;

    // Exclude keywords - column headers, labels, status text
    std::string exclude_kw[] = {
        // English generic
        "total", "sum", "note", "status", "type", "header", "number", "code",
        // Chinese generic
        "合计", "总计", "备注", "状态", "编码", "序号", "案号", "登记号",
        // Foreign patent status/labels
        "红色专利", "灰色专利", "绿色专利", "蓝色专利",
        "视为撤回", "中国同源", "国内同源", "同源",
        "授权", "驳回", "审查中", "申请中",
        // Exclude pure numeric status
        "nan", "null", "none", "n/a"
    };

    std::string lower = code;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (const auto& kw : exclude_kw) {
        std::string lower_kw = kw;
        std::transform(lower_kw.begin(), lower_kw.end(), lower_kw.begin(), ::tolower);
        if (lower == lower_kw || lower == lower_kw + "s") return false;
    }

    return true;
}

Patent ExcelIO::MergePatents(const Patent& existing, const Patent& new_data) {
    Patent merged = existing;
    if (!new_data.application_number.empty()) merged.application_number = new_data.application_number;
    if (!new_data.title.empty()) merged.title = new_data.title;
    if (!new_data.application_status.empty()) merged.application_status = new_data.application_status;
    if (!new_data.patent_type.empty()) merged.patent_type = new_data.patent_type;
    if (!new_data.patent_level.empty()) merged.patent_level = new_data.patent_level;
    if (!new_data.application_date.empty()) merged.application_date = new_data.application_date;
    if (!new_data.authorization_date.empty()) merged.authorization_date = new_data.authorization_date;
    if (!new_data.expiration_date.empty()) merged.expiration_date = new_data.expiration_date;
    if (!new_data.geke_handler.empty()) merged.geke_handler = new_data.geke_handler;
    if (!new_data.inventor.empty()) merged.inventor = new_data.inventor;
    if (!new_data.class_level1.empty()) merged.class_level1 = new_data.class_level1;
    if (!new_data.class_level2.empty()) merged.class_level2 = new_data.class_level2;
    if (!new_data.class_level3.empty()) merged.class_level3 = new_data.class_level3;
    if (!new_data.notes.empty()) merged.notes = new_data.notes;
    if (!new_data.proposal_name.empty()) merged.proposal_name = new_data.proposal_name;
    if (!new_data.original_applicant.empty()) merged.original_applicant = new_data.original_applicant;
    if (!new_data.current_applicant.empty()) merged.current_applicant = new_data.current_applicant;
    if (!new_data.rd_department.empty()) merged.rd_department = new_data.rd_department;
    if (!new_data.agency_firm.empty()) merged.agency_firm = new_data.agency_firm;
    return merged;
}

bool ExcelIO::IsChanged(const Patent& old_data, const Patent& new_data) {
    return old_data.application_number != new_data.application_number ||
           old_data.title != new_data.title ||
           old_data.application_status != new_data.application_status ||
           old_data.patent_type != new_data.patent_type ||
           old_data.patent_level != new_data.patent_level ||
           old_data.application_date != new_data.application_date ||
           old_data.authorization_date != new_data.authorization_date ||
           old_data.expiration_date != new_data.expiration_date ||
           old_data.geke_handler != new_data.geke_handler ||
           old_data.inventor != new_data.inventor ||
           old_data.class_level1 != new_data.class_level1 ||
           old_data.class_level2 != new_data.class_level2 ||
           old_data.class_level3 != new_data.class_level3;
}

std::string ExcelIO::GetLastError() const {
    return last_error_;
}

ExcelIO& GetExcelIO() {
    static ExcelIO instance;
    return instance;
}

// Stub implementations for header-only declarations
void ExcelIO::ProcessPatentRows(void*, uint32_t, uint32_t, uint16_t, const std::vector<int>&, Database&, ImportResult&) {}
void ExcelIO::ProcessOARows(void*, uint32_t, uint32_t, uint16_t, const std::vector<int>&, Database&, ImportResult&) {}
void ExcelIO::ProcessPCTRows(void*, uint32_t, uint32_t, uint16_t, const std::vector<int>&, Database&, ImportResult&) {}
void ExcelIO::ProcessSoftwareRows(void*, uint32_t, uint32_t, uint16_t, const std::vector<int>&, Database&, ImportResult&) {}
void ExcelIO::ProcessICRows(void*, uint32_t, uint32_t, uint16_t, const std::vector<int>&, Database&, ImportResult&) {}
void ExcelIO::ProcessForeignRows(void*, uint32_t, uint32_t, uint16_t, const std::vector<int>&, Database&, ImportResult&) {}
