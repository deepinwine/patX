/**
 * patX 主程序入口
 */

#include <iostream>
#include <string>
#include <cstring>
#include "patent.hpp"
#include "database.hpp"
#include "index.hpp"

// Rust FFI 声明
extern "C" {
    int patx_init();
    int patx_shutdown();
    char* patx_version();
}

void print_usage() {
    std::cout << R"(
patX - 专利数据管理系统 C++/Rust 极速版

用法:
  patx <命令> [参数]

命令:
  init        初始化数据库
  import      导入专利数据 (Excel/CSV)
  export      导出专利数据
  search      检索专利
  stats       显示统计信息
  version     显示版本

示例:
  patx init
  patx import data.xlsx
  patx search --applicant "华为"
  patx stats
)" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 0;
    }
    
    std::string cmd = argv[1];
    
    // 初始化 Rust 运行时
    if (patx_init() != 0) {
        std::cerr << "Failed to initialize Rust runtime" << std::endl;
        return 1;
    }
    
    // 获取全局实例
    auto& db = patx::GetDatabase();
    auto& index = patx::GetGlobalIndex();
    
    if (cmd == "version") {
        char* version = patx_version();
        std::cout << "patX version " << version << std::endl;
        
    } else if (cmd == "init") {
        std::string db_path = argc > 2 ? argv[2] : "patents.db";
        if (db.Open(db_path)) {
            std::cout << "Database initialized: " << db_path << std::endl;
        } else {
            std::cerr << "Failed to open database: " << db.GetLastError() << std::endl;
        }
        
    } else if (cmd == "stats") {
        auto stats = index.GetStatistics();
        std::cout << "专利总数: " << stats.total_patents << std::endl;
        std::cout << "待处理OA: " << stats.pending_oa << std::endl;
        std::cout << "紧急OA: " << stats.urgent_oa << std::endl;
        
    } else if (cmd == "search") {
        if (argc < 3) {
            std::cerr << "Usage: patx search <geke_code>" << std::endl;
            return 1;
        }
        Patent* p = index.FindByGekeCode(argv[2]);
        if (p) {
            std::cout << "专利: " << p->title << std::endl;
            std::cout << "申请号: " << p->application_no << std::endl;
        } else {
            std::cout << "未找到专利: " << argv[2] << std::endl;
        }
        
    } else {
        print_usage();
    }
    
    patx_shutdown();
    return 0;
}