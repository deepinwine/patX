/**
 * patX 主程序入口
 */

#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <iomanip>
#include <fstream>
#include "patent.hpp"
#include "database.hpp"
#include "index.hpp"

// Rust FFI 声明
extern "C" {
    int patx_init();
    int patx_shutdown();
    char* patx_version();
    void patx_free_string(char* s);
    unsigned long long patx_submit_import(const char* file_path, int format);
    unsigned long long patx_submit_export(const char* file_path, int format);
    unsigned long long patx_submit_search(const char* query, size_t limit);
}

// 辅助函数：获取当前时间字符串
std::string GetCurrentTime() {
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buffer);
}

void print_usage() {
    std::cout << R"(
patX - 专利数据管理系统 C++/Rust 极速版

用法:
  patx <命令> [参数]

命令:
  init        初始化数据库
  import      导入专利数据 (Excel/CSV/JSON)
  export      导出专利数据
  search      检索专利
  list        列出所有专利
  stats       显示统计信息
  add         添加单条专利
  delete      删除专利
  version     显示版本
  help        显示帮助

选项:
  --applicant    按申请人筛选
  --type         按专利类型筛选
  --status       按法律状态筛选
  --from         起始日期
  --to           结束日期

示例:
  patx init
  patx import data.xlsx
  patx import data.csv --format csv
  patx search --applicant "华为" --type "发明"
  patx stats
  patx add --geke GC-0001 --title "测试专利"
  patx delete GC-0001
)" << std::endl;
}

void print_patent(const patx::Patent* p) {
    if (!p) return;
    
    std::cout << "----------------------------------------" << std::endl;
    std::cout << "格科编码: " << p->geke_code << std::endl;
    std::cout << "申请号: " << p->application_no << std::endl;
    std::cout << "标题: " << p->title << std::endl;
    std::cout << "申请人: " << p->applicant << std::endl;
    std::cout << "申请日期: " << p->application_date << std::endl;
    std::cout << "专利类型: " << p->patent_type << std::endl;
    std::cout << "法律状态: " << p->legal_status << std::endl;
    std::cout << "发明人: " << p->inventor << std::endl;
    std::cout << "代理人: " << p->agent << std::endl;
    std::cout << "代理机构: " << p->agency << std::endl;
    std::cout << "处理人: " << p->handler << std::endl;
    std::cout << "备注: " << p->notes << std::endl;
    std::cout << "专利等级: " << p->patent_level << std::endl;
    std::cout << "创建时间: " << p->created_at << std::endl;
    std::cout << "更新时间: " << p->updated_at << std::endl;
}

void cmd_init(const std::vector<std::string>& args) {
    std::string db_path = "patents.db";
    
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--db" && i + 1 < args.size()) {
            db_path = args[++i];
        }
    }
    
    auto& db = patx::GetDatabase();
    if (db.Open(db_path)) {
        std::cout << "数据库初始化成功: " << db_path << std::endl;
    } else {
        std::cerr << "数据库初始化失败: " << db.GetLastError() << std::endl;
    }
}

void cmd_import(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "用法: patx import <文件路径> [--format excel|csv|json]" << std::endl;
        return;
    }
    
    std::string file_path = args[1];
    int format = 0; // 默认 Excel
    
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--format" && i + 1 < args.size()) {
            std::string fmt = args[++i];
            if (fmt == "csv") format = 1;
            else if (fmt == "json") format = 2;
        }
    }
    
    uint64_t task_id = patx_submit_import(file_path.c_str(), format);
    if (task_id > 0) {
        std::cout << "导入任务已提交, 任务ID: " << task_id << std::endl;
        std::cout << "请稍后使用 'patx stats' 查看结果" << std::endl;
    } else {
        std::cerr << "导入任务提交失败" << std::endl;
    }
}

void cmd_export(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "用法: patx export <文件路径> [--format excel|csv|json]" << std::endl;
        return;
    }
    
    std::string file_path = args[1];
    int format = 0; // 默认 Excel
    
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--format" && i + 1 < args.size()) {
            std::string fmt = args[++i];
            if (fmt == "csv") format = 1;
            else if (fmt == "json") format = 2;
        }
    }
    
    uint64_t task_id = patx_submit_export(file_path.c_str(), format);
    if (task_id > 0) {
        std::cout << "导出任务已提交, 任务ID: " << task_id << std::endl;
    } else {
        std::cerr << "导出任务提交失败" << std::endl;
    }
}

void cmd_search(const std::vector<std::string>& args) {
    std::string geke_code;
    std::string applicant;
    std::string patent_type;
    std::string legal_status;
    std::string date_from;
    std::string date_to;
    
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--geke" && i + 1 < args.size()) {
            geke_code = args[++i];
        } else if (args[i] == "--applicant" && i + 1 < args.size()) {
            applicant = args[++i];
        } else if (args[i] == "--type" && i + 1 < args.size()) {
            patent_type = args[++i];
        } else if (args[i] == "--status" && i + 1 < args.size()) {
            legal_status = args[++i];
        } else if (args[i] == "--from" && i + 1 < args.size()) {
            date_from = args[++i];
        } else if (args[i] == "--to" && i + 1 < args.size()) {
            date_to = args[++i];
        }
    }
    
    auto& index = patx::GetGlobalIndex();
    
    if (!geke_code.empty()) {
        // 精确搜索
        patx::Patent* p = index.FindByGekeCode(geke_code);
        if (p) {
            print_patent(p);
        } else {
            std::cout << "未找到专利: " << geke_code << std::endl;
        }
    } else {
        // 组合搜索
        auto results = index.SearchMulti(applicant, patent_type, date_from, date_to, legal_status);
        
        std::cout << "找到 " << results.size() << " 条记录" << std::endl;
        
        for (size_t i = 0; i < results.size() && i < 20; ++i) {
            std::cout << (i + 1) << ". ";
            std::cout << "[" << results[i]->geke_code << "] ";
            std::cout << results[i]->title << " - " << results[i]->applicant << std::endl;
        }
        
        if (results.size() > 20) {
            std::cout << "... 还有 " << (results.size() - 20) << " 条记录" << std::endl;
        }
    }
}

void cmd_list(const std::vector<std::string>& args) {
    auto& index = patx::GetGlobalIndex();
    auto all = index.GetAll();
    
    std::cout << "共 " << all.size() << " 条专利记录" << std::endl;
    
    int limit = 50;
    for (int i = 0; i < static_cast<int>(all.size()) && i < limit; ++i) {
        std::cout << std::setw(4) << (i + 1) << ". ";
        std::cout << "[" << all[i]->geke_code << "] ";
        std::cout << all[i]->title;
        std::cout << " (" << all[i]->applicant << ")" << std::endl;
    }
    
    if (static_cast<int>(all.size()) > limit) {
        std::cout << "... 还有 " << (all.size() - limit) << " 条记录" << std::endl;
    }
}

void cmd_stats() {
    auto& db = patx::GetDatabase();
    auto& index = patx::GetGlobalIndex();
    
    auto db_stats = db.GetStatistics();
    auto idx_stats = index.GetStatistics();
    
    std::cout << "=== patX 统计信息 ===" << std::endl;
    std::cout << std::endl;
    std::cout << "数据库统计:" << std::endl;
    std::cout << "  专利总数: " << db_stats.total_patents << std::endl;
    std::cout << "  待处理 OA: " << db_stats.pending_oa << std::endl;
    std::cout << "  紧急 OA: " << db_stats.urgent_oa << std::endl;
    std::cout << std::endl;
    std::cout << "按类型分布:" << std::endl;
    std::cout << "  发明专利: " << db_stats.by_type[0] << std::endl;
    std::cout << "  实用新型: " << db_stats.by_type[1] << std::endl;
    std::cout << "  外观设计: " << db_stats.by_type[2] << std::endl;
    std::cout << std::endl;
    std::cout << "按状态分布:" << std::endl;
    std::cout << "  有效: " << db_stats.by_status[0] << std::endl;
    std::cout << "  失效: " << db_stats.by_status[1] << std::endl;
    std::cout << "  审中: " << db_stats.by_status[2] << std::endl;
    std::cout << std::endl;
    std::cout << "内存索引:" << std::endl;
    std::cout << "  索引条目: " << idx_stats.total_patents << std::endl;
}

void cmd_add(const std::vector<std::string>& args) {
    patx::Patent p;
    memset(&p, 0, sizeof(p));
    
    std::string now = GetCurrentTime();
    strncpy(p.created_at, now.c_str(), sizeof(p.created_at) - 1);
    strncpy(p.updated_at, now.c_str(), sizeof(p.updated_at) - 1);
    p.is_valid = 1;
    p.patent_level = 0;
    
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--geke" && i + 1 < args.size()) {
            strncpy(p.geke_code, args[++i].c_str(), sizeof(p.geke_code) - 1);
        } else if (args[i] == "--title" && i + 1 < args.size()) {
            strncpy(p.title, args[++i].c_str(), sizeof(p.title) - 1);
        } else if (args[i] == "--applicant" && i + 1 < args.size()) {
            strncpy(p.applicant, args[++i].c_str(), sizeof(p.applicant) - 1);
        } else if (args[i] == "--appno" && i + 1 < args.size()) {
            strncpy(p.application_no, args[++i].c_str(), sizeof(p.application_no) - 1);
        } else if (args[i] == "--type" && i + 1 < args.size()) {
            strncpy(p.patent_type, args[++i].c_str(), sizeof(p.patent_type) - 1);
        } else if (args[i] == "--status" && i + 1 < args.size()) {
            strncpy(p.legal_status, args[++i].c_str(), sizeof(p.legal_status) - 1);
        } else if (args[i] == "--inventor" && i + 1 < args.size()) {
            strncpy(p.inventor, args[++i].c_str(), sizeof(p.inventor) - 1);
        } else if (args[i] == "--handler" && i + 1 < args.size()) {
            strncpy(p.handler, args[++i].c_str(), sizeof(p.handler) - 1);
        } else if (args[i] == "--level" && i + 1 < args.size()) {
            p.patent_level = std::stoi(args[++i]);
        } else if (args[i] == "--notes" && i + 1 < args.size()) {
            strncpy(p.notes, args[++i].c_str(), sizeof(p.notes) - 1);
        }
    }
    
    if (p.geke_code[0] == '\0') {
        std::cerr << "错误: 必须指定 --geke 参数" << std::endl;
        return;
    }
    
    auto& db = patx::GetDatabase();
    if (db.InsertPatent(p)) {
        std::cout << "专利添加成功: " << p.geke_code << std::endl;
    } else {
        std::cerr << "添加失败: " << db.GetLastError() << std::endl;
    }
}

void cmd_delete(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "用法: patx delete <格科编码或ID>" << std::endl;
        return;
    }
    
    auto& db = patx::GetDatabase();
    auto& index = patx::GetGlobalIndex();
    
    // 尝试按格科编码查找
    patx::Patent* p = index.FindByGekeCode(args[1]);
    if (p) {
        if (db.DeletePatent(p->id)) {
            std::cout << "专利已删除: " << args[1] << std::endl;
        } else {
            std::cerr << "删除失败: " << db.GetLastError() << std::endl;
        }
    } else {
        // 尝试按 ID 删除
        try {
            int32_t id = std::stoi(args[1]);
            if (db.DeletePatent(id)) {
                std::cout << "专利已删除: ID=" << id << std::endl;
            } else {
                std::cerr << "删除失败: " << db.GetLastError() << std::endl;
            }
        } catch (...) {
            std::cerr << "未找到专利: " << args[1] << std::endl;
        }
    }
}

void cmd_version() {
    char* version = patx_version();
    std::cout << "patX version " << version << std::endl;
    std::cout << "C++/Rust 极速版" << std::endl;
    std::cout << "专利数据管理系统" << std::endl;
    patx_free_string(version);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 0;
    }
    
    // 初始化 Rust 运行时
    if (patx_init() != 0) {
        std::cerr << "Rust 运行时初始化失败" << std::endl;
        return 1;
    }
    
    // 解析命令行参数
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(argv[i]);
    }
    
    std::string cmd = args[0];
    
    try {
        if (cmd == "version" || cmd == "-v" || cmd == "--version") {
            cmd_version();
        } else if (cmd == "help" || cmd == "-h" || cmd == "--help") {
            print_usage();
        } else if (cmd == "init") {
            cmd_init(args);
        } else if (cmd == "import") {
            cmd_import(args);
        } else if (cmd == "export") {
            cmd_export(args);
        } else if (cmd == "search") {
            cmd_search(args);
        } else if (cmd == "list" || cmd == "ls") {
            cmd_list(args);
        } else if (cmd == "stats") {
            cmd_stats();
        } else if (cmd == "add") {
            cmd_add(args);
        } else if (cmd == "delete" || cmd == "del" || cmd == "rm") {
            cmd_delete(args);
        } else {
            std::cerr << "未知命令: " << cmd << std::endl;
            print_usage();
        }
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        patx_shutdown();
        return 1;
    }
    
    patx_shutdown();
    return 0;
}
