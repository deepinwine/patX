/**
 * patX 单元测试
 */

#include <iostream>
#include <cassert>
#include <cstring>
#include <chrono>
#include "../include/patent.hpp"
#include "../include/database.hpp"
#include "../include/index.hpp"

using namespace patx;

// 测试专利数据结构
void test_patent_struct() {
    std::cout << "[TEST] Patent struct..." << std::endl;
    
    Patent p;
    memset(&p, 0, sizeof(p));
    
    p.id = 1;
    strncpy(p.geke_code, "GC-0001", sizeof(p.geke_code) - 1);
    strncpy(p.application_no, "CN202310000001.X", sizeof(p.application_no) - 1);
    strncpy(p.title, "测试专利标题", sizeof(p.title) - 1);
    strncpy(p.applicant, "测试申请人", sizeof(p.applicant) - 1);
    strncpy(p.application_date, "2023-01-15", sizeof(p.application_date) - 1);
    strncpy(p.patent_type, "发明", sizeof(p.patent_type) - 1);
    strncpy(p.legal_status, "有效", sizeof(p.legal_status) - 1);
    p.patent_level = 1;
    
    assert(p.id == 1);
    assert(strcmp(p.geke_code, "GC-0001") == 0);
    assert(strcmp(p.title, "测试专利标题") == 0);
    
    std::cout << "  [PASS] Patent struct test" << std::endl;
}

// 测试数据库操作
void test_database() {
    std::cout << "[TEST] Database operations..." << std::endl;
    
    Database db;
    
    // 测试打开数据库
    bool opened = db.Open(":memory:");
    assert(opened);
    assert(db.IsOpen());
    
    // 测试插入
    Patent p1;
    memset(&p1, 0, sizeof(p1));
    p1.id = 1;
    strncpy(p1.geke_code, "GC-0001", sizeof(p1.geke_code) - 1);
    strncpy(p1.title, "专利1", sizeof(p1.title) - 1);
    strncpy(p1.applicant, "公司A", sizeof(p1.applicant) - 1);
    strncpy(p1.patent_type, "发明", sizeof(p1.patent_type) - 1);
    
    bool inserted = db.InsertPatent(p1);
    assert(inserted);
    
    // 测试查找
    Patent* found = db.FindPatentById(1);
    assert(found != nullptr);
    assert(strcmp(found->geke_code, "GC-0001") == 0);
    
    // 测试更新
    strncpy(found->title, "更新后的标题", sizeof(found->title) - 1);
    bool updated = db.UpdatePatent(*found);
    assert(updated);
    
    // 测试删除
    bool deleted = db.DeletePatent(1);
    assert(deleted);
    
    found = db.FindPatentById(1);
    assert(found == nullptr);
    
    db.Close();
    assert(!db.IsOpen());
    
    std::cout << "  [PASS] Database test" << std::endl;
}

// 测试内存索引
void test_index() {
    std::cout << "[TEST] Memory index..." << std::endl;
    
    PatentIndex index;
    index.Init(100);
    
    // 添加测试数据
    for (int i = 1; i <= 10; ++i) {
        Patent p;
        memset(&p, 0, sizeof(p));
        p.id = i;
        
        char geke[16];
        snprintf(geke, sizeof(geke), "GC-%04d", i);
        strncpy(p.geke_code, geke, sizeof(p.geke_code) - 1);
        
        snprintf(p.title, sizeof(p.title), "测试专利%d", i);
        strncpy(p.applicant, i <= 5 ? "公司A" : "公司B", sizeof(p.applicant) - 1);
        strncpy(p.patent_type, i % 2 == 0 ? "发明" : "实用新型", sizeof(p.patent_type) - 1);
        
        index.Add(p);
    }
    
    // 测试数量
    assert(index.Count() == 10);
    
    // 测试精确查找
    Patent* found = index.FindByGekeCode("GC-0001");
    assert(found != nullptr);
    assert(strcmp(found->geke_code, "GC-0001") == 0);
    
    // 测试组合搜索
    auto results = index.SearchMulti("公司A", "", "", "", "");
    assert(results.size() == 5);
    
    results = index.SearchMulti("", "发明", "", "", "");
    assert(results.size() == 5);
    
    // 测试删除
    index.Remove(1);
    assert(index.Count() == 9);
    
    found = index.FindByGekeCode("GC-0001");
    assert(found == nullptr);
    
    std::cout << "  [PASS] Index test" << std::endl;
}

// 测试统计功能
void test_statistics() {
    std::cout << "[TEST] Statistics..." << std::endl;
    
    Database db;
    db.Open(":memory:");
    
    // 插入测试数据
    Patent p;
    memset(&p, 0, sizeof(p));
    
    for (int i = 1; i <= 20; ++i) {
        memset(&p, 0, sizeof(p));
        p.id = i;
        
        char geke[16];
        snprintf(geke, sizeof(geke), "GC-%04d", i);
        strncpy(p.geke_code, geke, sizeof(p.geke_code) - 1);
        
        strncpy(p.patent_type, i % 3 == 0 ? "实用新型" : "发明", sizeof(p.patent_type) - 1);
        strncpy(p.legal_status, i % 2 == 0 ? "有效" : "审中", sizeof(p.legal_status) - 1);
        
        db.InsertPatent(p);
    }
    
    Statistics stats = db.GetStatistics();
    assert(stats.total_patents == 20);
    
    std::cout << "  总专利数: " << stats.total_patents << std::endl;
    std::cout << "  发明: " << stats.by_type[0] << std::endl;
    std::cout << "  有效: " << stats.by_status[0] << std::endl;
    
    db.Close();
    
    std::cout << "  [PASS] Statistics test" << std::endl;
}

// 性能测试
void test_performance() {
    std::cout << "[TEST] Performance..." << std::endl;
    
    const int N = 10000;
    
    PatentIndex index;
    index.Init(N);
    
    // 测试批量添加性能
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 1; i <= N; ++i) {
        Patent p;
        memset(&p, 0, sizeof(p));
        p.id = i;
        
        char geke[16];
        snprintf(geke, sizeof(geke), "GC-%05d", i);
        strncpy(p.geke_code, geke, sizeof(p.geke_code) - 1);
        
        index.Add(p);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    double add_ms = std::chrono::duration<double, std::milli>(end - start).count();
    
    std::cout << "  批量添加 " << N << " 条: " << add_ms << "ms" << std::endl;
    
    // 测试查找性能
    start = std::chrono::high_resolution_clock::now();
    
    for (int i = 1; i <= 10000; ++i) {
        char geke[16];
        snprintf(geke, sizeof(geke), "GC-%05d", (i % N) + 1);
        Patent* p = index.FindByGekeCode(geke);
        assert(p != nullptr);
    }
    
    end = std::chrono::high_resolution_clock::now();
    double query_ms = std::chrono::duration<double, std::milli>(end - start).count();
    
    std::cout << "  精确查找 10000 次: " << query_ms << "ms" << std::endl;
    std::cout << "  平均每次: " << (query_ms / 10000) << "ms" << std::endl;
    
    // 性能验证
    assert(add_ms < 1000);  // 1秒内完成添加
    assert(query_ms < 10);  // 10ms 内完成10000次查找
    
    std::cout << "  [PASS] Performance test" << std::endl;
}

// 批量操作测试
void test_batch_operations() {
    std::cout << "[TEST] Batch operations..." << std::endl;
    
    Database db;
    db.Open(":memory:");
    
    // 准备批量数据
    std::vector<Patent> patents;
    for (int i = 1; i <= 100; ++i) {
        Patent p;
        memset(&p, 0, sizeof(p));
        p.id = i;
        
        char geke[16];
        snprintf(geke, sizeof(geke), "GC-%04d", i);
        strncpy(p.geke_code, geke, sizeof(p.geke_code) - 1);
        
        patents.push_back(p);
    }
    
    // 测试批量插入
    auto start = std::chrono::high_resolution_clock::now();
    bool success = db.InsertPatentsBatch(patents);
    auto end = std::chrono::high_resolution_clock::now();
    
    assert(success);
    
    double batch_ms = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "  批量插入 100 条: " << batch_ms << "ms" << std::endl;
    
    // 测试加载全部
    auto loaded = db.LoadAllPatents();
    assert(loaded.size() == 100);
    
    // 测试搜索
    auto results = db.SearchPatents("GC");
    assert(results.size() == 100);
    
    db.Close();
    
    std::cout << "  [PASS] Batch operations test" << std::endl;
}

int main() {
    std::cout << std::endl;
    std::cout << "==================================" << std::endl;
    std::cout << "  patX Unit Tests" << std::endl;
    std::cout << "==================================" << std::endl;
    std::cout << std::endl;
    
    try {
        test_patent_struct();
        test_database();
        test_index();
        test_statistics();
        test_performance();
        test_batch_operations();
        
        std::cout << std::endl;
        std::cout << "==================================" << std::endl;
        std::cout << "  All tests passed!" << std::endl;
        std::cout << "==================================" << std::endl;
        std::cout << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return 1;
    }
}