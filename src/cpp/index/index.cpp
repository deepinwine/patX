/**
 * patX 内存索引实现
 */

#include "index.hpp"
#include <algorithm>
#include <cstring>

namespace patx {

PatentIndex::PatentIndex() : capacity_(0) {}

PatentIndex::~PatentIndex() = default;

void PatentIndex::Init(size_t capacity) {
    capacity_ = capacity;
    patents_.reserve(capacity);
    geke_code_index_.reserve(capacity);
    app_no_index_.reserve(capacity);
}

void PatentIndex::Add(const Patent& patent) {
    auto p = std::make_unique<Patent>(patent);
    int32_t id = patent.id;
    
    // 如果已存在，先删除
    if (patents_.find(id) != patents_.end()) {
        Remove(id);
    }
    
    // 更新索引
    geke_code_index_[p->geke_code] = id;
    app_no_index_[p->application_no] = id;
    
    // 只有非空值才添加到分类索引
    if (p->applicant[0] != '\0') {
        applicant_index_[p->applicant].push_back(id);
    }
    if (p->patent_type[0] != '\0') {
        type_index_[p->patent_type].push_back(id);
    }
    if (p->legal_status[0] != '\0') {
        status_index_[p->legal_status].push_back(id);
    }
    
    patents_[id] = std::move(p);
}

void PatentIndex::Remove(int32_t id) {
    auto it = patents_.find(id);
    if (it == patents_.end()) return;
    
    // 清理索引
    geke_code_index_.erase(it->second->geke_code);
    app_no_index_.erase(it->second->application_no);
    
    // 从分类索引中移除
    {
        auto& vec = applicant_index_[it->second->applicant];
        vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
    }
    {
        auto& vec = type_index_[it->second->patent_type];
        vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
    }
    {
        auto& vec = status_index_[it->second->legal_status];
        vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
    }
    
    patents_.erase(it);
}

Patent* PatentIndex::FindByGekeCode(std::string_view geke_code) {
    auto it = geke_code_index_.find(std::string(geke_code));
    if (it == geke_code_index_.end()) return nullptr;
    return FindById(it->second);
}

Patent* PatentIndex::FindByApplicationNo(std::string_view app_no) {
    auto it = app_no_index_.find(std::string(app_no));
    if (it == app_no_index_.end()) return nullptr;
    return FindById(it->second);
}

Patent* PatentIndex::FindById(int32_t id) {
    auto it = patents_.find(id);
    return it != patents_.end() ? it->second.get() : nullptr;
}

std::vector<Patent*> PatentIndex::SearchMulti(
    std::string_view applicant,
    std::string_view patent_type,
    std::string_view date_from,
    std::string_view date_to,
    std::string_view legal_status
) {
    std::vector<Patent*> results;
    std::vector<int32_t> candidate_ids;
    
    // 根据条件收集候选 ID
    bool has_filter = false;
    
    if (!applicant.empty()) {
        std::string app_str(applicant);
        // 支持模糊匹配
        for (const auto& [key, ids] : applicant_index_) {
            if (key.find(app_str) != std::string::npos) {
                candidate_ids.insert(candidate_ids.end(), ids.begin(), ids.end());
            }
        }
        has_filter = true;
    }
    
    if (!patent_type.empty()) {
        std::string type_str(patent_type);
        auto it = type_index_.find(type_str);
        if (it != type_index_.end()) {
            if (has_filter) {
                // 取交集
                std::vector<int32_t> intersection;
                std::sort(candidate_ids.begin(), candidate_ids.end());
                auto type_ids = it->second;
                std::sort(type_ids.begin(), type_ids.end());
                std::set_intersection(
                    candidate_ids.begin(), candidate_ids.end(),
                    type_ids.begin(), type_ids.end(),
                    std::back_inserter(intersection)
                );
                candidate_ids = std::move(intersection);
            } else {
                candidate_ids = it->second;
            }
        }
        has_filter = true;
    }
    
    if (!legal_status.empty()) {
        std::string status_str(legal_status);
        auto it = status_index_.find(status_str);
        if (it != status_index_.end()) {
            if (has_filter) {
                // 取交集
                std::vector<int32_t> intersection;
                std::sort(candidate_ids.begin(), candidate_ids.end());
                auto status_ids = it->second;
                std::sort(status_ids.begin(), status_ids.end());
                std::set_intersection(
                    candidate_ids.begin(), candidate_ids.end(),
                    status_ids.begin(), status_ids.end(),
                    std::back_inserter(intersection)
                );
                candidate_ids = std::move(intersection);
            } else {
                candidate_ids = it->second;
            }
        }
        has_filter = true;
    }
    
    // 如果没有任何过滤条件，返回所有专利
    if (!has_filter) {
        for (auto& [id, p] : patents_) {
            candidate_ids.push_back(id);
        }
    }
    
    // 过滤日期范围
    for (int32_t id : candidate_ids) {
        Patent* p = FindById(id);
        if (!p) continue;
        
        // 日期过滤
        if (!date_from.empty()) {
            if (strcmp(p->application_date, std::string(date_from).c_str()) < 0) {
                continue;
            }
        }
        if (!date_to.empty()) {
            if (strcmp(p->application_date, std::string(date_to).c_str()) > 0) {
                continue;
            }
        }
        
        results.push_back(p);
    }
    
    return results;
}

size_t PatentIndex::Count() const {
    return patents_.size();
}

Statistics PatentIndex::GetStatistics() const {
    Statistics stats;
    memset(&stats, 0, sizeof(stats));
    
    stats.total_patents = static_cast<int32_t>(patents_.size());
    
    // 按类型统计
    for (const auto& [id, p] : patents_) {
        if (strcmp(p->patent_type, "发明") == 0) {
            stats.by_type[0]++;
        } else if (strcmp(p->patent_type, "实用新型") == 0) {
            stats.by_type[1]++;
        } else if (strcmp(p->patent_type, "外观设计") == 0) {
            stats.by_type[2]++;
        }
        
        if (strcmp(p->legal_status, "有效") == 0) {
            stats.by_status[0]++;
        } else if (strcmp(p->legal_status, "失效") == 0) {
            stats.by_status[1]++;
        } else if (strcmp(p->legal_status, "pending") == 0) {
            stats.by_status[2]++;
        }
    }
    
    return stats;
}

void PatentIndex::AddBatch(const std::vector<Patent>& patents) {
    for (const auto& p : patents) {
        Add(p);
    }
}

std::vector<Patent*> PatentIndex::GetAll() {
    std::vector<Patent*> result;
    result.reserve(patents_.size());
    for (auto& [id, p] : patents_) {
        result.push_back(p.get());
    }
    return result;
}

// 全局索引
PatentIndex& GetGlobalIndex() {
    static PatentIndex instance;
    return instance;
}

} // namespace patx
