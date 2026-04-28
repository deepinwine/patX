/**
 * patX - 专利数据管理系统 C++/Rust 极速版
 * 
 * 专利数据结构定义
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace patx {

// 专利数据结构 - 紧凑内存布局
struct Patent {
    int32_t id;
    char geke_code[16];         // 格科编码
    char application_no[32];    // 申请号
    char title[256];            // 发明名称
    char applicant[128];        // 申请人
    char application_date[16];  // 申请日期
    char patent_type[32];       // 专利类型
    char legal_status[32];      // 法律状态
    char inventor[256];         // 发明人
    char classification[64];    // 分类号
    char agent[64];             // 代理人
    char agency[128];           // 代理机构
    char publish_no[32];        // 公开号
    char publish_date[16];      // 公开日期
    char grant_date[16];        // 授权日期
    char expire_date[16];       // 到期日期
    char handler[32];           // 处理人
    char notes[512];            // 备注
    int32_t patent_level;       // 专利等级
    int32_t is_valid;           // 是否有效
    char created_at[32];        // 创建时间
    char updated_at[32];        // 更新时间
};

// OA 记录结构
struct OaRecord {
    int32_t id;
    char geke_code[16];
    char patent_title[256];
    char oa_type[32];
    char issue_date[16];
    char official_deadline[16];
    char handler[32];
    int32_t is_completed;
    char created_at[32];
    char updated_at[32];
};

// PCT 专利结构
struct PctPatent {
    int32_t id;
    char geke_code[16];
    char application_no[32];
    char title[256];
    char country[32];
    char application_status[32];
    char created_at[32];
    char updated_at[32];
};

// 统计信息
struct Statistics {
    int32_t total_patents;
    int32_t pending_oa;
    int32_t urgent_oa;
    int32_t by_type[10];    // 按类型统计
    int32_t by_status[10];  // 按状态统计
};

} // namespace patx