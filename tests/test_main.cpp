/**
 * patX 单元测试
 */

#include <iostream>
#include <cassert>
#include <cstring>
#include "../include/patent.hpp"
#include "../include/database.hpp"
#include "../include/index.hpp"

extern "C" {
    int patx_init();
    int patx_shutdown();
}

void test_patent_struct() {
    std::cout << "测试专利数据结构..." << std::endl;
    
    patx::Patent p;
    memset(&p, 0, sizeof(p));
    
    strcpy(p.geke_code, "GC-0001");
    strcpy(p.title, "测试专利");
    strcpy(p.applicant, "测试公司");
    p.patent_level = 1;
    
    assert(strcmp(p.geke_code, "GC-0001") == 0);
    assert(strcmp(p.title, "测试专利") == 0);
    
    std::cout << "  ✓ 数据结构测试通过" << std::endl;
}

void test_index() {
    std::cout << "测试内存索引..." << std::endl;
    
    patx::PatentIndex index;
    index.Init(1000);
    
    patx::Patent p1;
    memset(&p1, 0, sizeof(p1));
    p1.id = 1;
    strcpy(p1.geke_code, "GC-0001");
    strcpy(p1.title, "专利1");
    
    index.Add(p1);
    
    assert(index.Count() == 1);
    
    patx::Patent* found = index.FindByGekeCode("GC-0001");
    assert(found != nullptr);
    assert(strcmp(found->title, "专利1") == 0);
    
    index.Remove(1);
    assert(index.Count() == 0);
    
    std::cout << "  ✓ 内存索引测试通过" << std::endl;
}

void test_database() {
    std::cout << "测试数据库..." << std::endl;
    
    patx::Database db;
    assert(db.Open(":memory:"));
    
    patx::Patent p;
    memset(&p, 0, sizeof(p));
    strcpy(p.geke_code, "GC-TEST");
    strcpy(p.title, "测试专利");
    strcpy(p.application_no, "CN202410001234.5");
    
    assert(db.InsertPatent(p));
    
    auto stats = db.GetStatistics();
    assert(stats.total_patents == 1);
    
    db.Close();
    
    std::cout << "  ✓ 数据库测试通过" << std::endl;
}

int main() {
    std::cout << "=== patX 单元测试 ===" << std::endl;
    
    patx_init();
    
    test_patent_struct();
    test_index();
    test_database();
    
    patx_shutdown();
    
    std::cout << "\n✅ 所有测试通过!" << std::endl;
    return 0;
}