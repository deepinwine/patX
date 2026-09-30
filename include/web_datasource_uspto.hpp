// USPTO Open Data Portal（ODP）原生 C++ 客户端。
//
// 数据源：https://api.uspto.gov/api/v1/patent/applications/{申请号}
// 认证：X-API-Key 头（key 仅存本地 git-ignored config，绝不进源码/日志）。
// 响应含 applicationMetaData（状态/审查员/日期）与 eventDataBag（完整
// 审查事件时间线：OA 发出日、答复、修改、NOA、放弃…）。
//
// 事件映射到统一模型（webdossier::RemoteDocument）：
//   Mail Non-Final/Final Rejection、Restriction、Advisory → OA 类事件
//   Notice of Allowance → 授权类事件
//   Notice of Abandonment → 视为放弃（对应恢复/终结判断）
//   申请人事件（Response after Non-Final Action / RCE / IDS / 修改）
//     → 不生成事件，但日期保留为“已答复”证据
// 优先取 Mail* 事件日期（MCTNF 等）——期限从发文日起算。
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace webdossier {

struct RemoteDocument;

namespace uspto_odp {

// 事件分类（纯函数，测试可注入 fixture）
struct UsEventClass {
    bool is_official = false;      // 官方发文（需要行动/记录）
    bool is_remindable = false;    // 建提醒事件（OA/NOA/放弃通知）
    std::string document_type;     // 统一模型类型字符串
    std::string title;             // 展示标题（英文原名）
    bool is_mail_variant = false;  // Mail* 变体（期限起算日）
};

UsEventClass ClassifyUsEvent(const std::string& event_code,
                             const std::string& description);

// 申请号规范：US 17/469,033 / 17469033 → 8 位数字
std::string NormalizeUsAppNumber(const std::string& raw);

struct ParseResult {
    std::string code;                  // OK / NOT_FOUND / AUTH_ERROR / ...
    std::string message;
    std::string status_description;    // 官方状态（Patented Case / Docketed…）
    std::vector<RemoteDocument> documents;
    std::string latest_applicant_activity;  // 已答复证据
};

ParseResult ParseApplicationJson(const std::string& body,
                                 const std::string& application_number);

using FetchJson = std::function<int(const std::string& url,
                                     const std::string& api_key,
                                     std::string& body)>;

class NativeUsptoOdpClient {
public:
    NativeUsptoOdpClient(std::string api_key, FetchJson fetch = nullptr);

    ParseResult FetchCase(const std::string& application_number);

private:
    std::string api_key_;
    FetchJson fetch_;
};

} // namespace uspto_odp
} // namespace webdossier
