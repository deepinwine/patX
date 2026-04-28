/**
 * patX 内存索引模块
 * 
 * 基于 flat_hash_map 的极速内存索引
 */

#pragma once

#include <absl/container/flat_hash_map.h>
#include <vector>
#include <string_view>
#include <memory>
#include "patent.hpp"

namespace patx {

// 内存索引类
class PatentIndex {
public:
    PatentIndex();
    ~PatentIndex();

    // 初始化索引
    void Init(size_t capacity);
    
    // 添加专利到索引
    void Add(const Patent& patent);
    
    // 移除专利
    void Remove(int32_t id);
    
    // 精准检索 - O(1)
    Patent* FindByGekeCode(std::string_view geke_code);
    Patent* FindByApplicationNo(std::string_view app_no);
    Patent* FindById(int32_t id);
    
    // 组合检索 - 返回匹配结果
    std::vector<Patent*> SearchMulti(
        std::string_view applicant,
        std::string_view patent_type,
        std::string_view date_from,
        std::string_view date_to,
        std::string_view legal_status
    );
    
    // 统计
    size_t Count() const;
    Statistics GetStatistics() const;
    
    // 批量操作
    void AddBatch(const std::vector<Patent>& patents);
    std::vector<Patent*> GetAll();

private:
    // 主存储 (id -> patent)
    absl::flat_hash_map<int32_t, std::unique_ptr<Patent>> patents_;
    
    // 索引
    absl::flat_hash_map<std::string, int32_t> geke_code_index_;
    absl::flat_hash_map<std::string, int32_t> app_no_index_;
    
    // 分类索引 (用于组合检索)
    absl::flat_hash_map<std::string, std::vector<int32_t>> applicant_index_;
    absl::flat_hash_map<std::string, std::vector<int32_t>> type_index_;
    absl::flat_hash_map<std::string, std::vector<int32_t>> status_index_;
    
    size_t capacity_;
};

// 全局索引实例
PatentIndex& GetGlobalIndex();

} // namespace patx