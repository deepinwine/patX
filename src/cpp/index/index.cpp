/**
 * patX 内存索引实现
 */

#include "index.hpp"
#include <algorithm>

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
    
    // 更新索引
    geke_code_index_[p->geke_code] = id;
    app_no_index_[p->application_no] = id;
    applicant_index_[p->applicant].push_back(id);
    type_index_[p->patent_type].push_back(id);
    status_index_[p->legal_status].push_back(id);
    
    patents_[id] = std::move(p);
}

void PatentIndex::Remove(int32_t id) {
    auto it = patents_.find(id);
    if (it == patents_.end()) return;
    
    // 清理索引
    geke_code_index_.erase(it->second->geke_code);
    app_no_index_.erase(it->second->application_no);
    
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

size_t PatentIndex::Count() const {
    return patents_.size();
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