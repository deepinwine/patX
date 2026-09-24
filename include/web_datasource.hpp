// USPTO Global Dossier 原生 C++ 数据源（免 Python / 免浏览器）。
//
// 该层是三层降级链的第一层，且是日常巡检的实际承载：纯 HTTPS + JSON
// （cloudfront 公开服务），因此直接用 libcurl + nlohmann 在 C++ 内实现。
// Python 侧车仅在需要 CNIPA（浏览器接管）或 EPO 时按需启动。
//
// 与 Python 实现（tools/web_dossier/providers/uspto_global_dossier.py）
// 保持行为一致：同样的请求头、节流、TRANSLATED 丢弃、event_key 算法，
// 保证两条路径写入的 event_key 完全一致（跨来源去重依赖于此）。
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace webdossier {

struct RemoteDocument;

namespace uspto {

// ---- 英文官方发文分类（与 Python document_classifier 对齐）----
// 返回 (document_type 字符串, oa_ordinal, confidence)；申请人提交文件
// 返回 direction=false。
struct EnClassification {
    std::string document_type;   // OFFICE_ACTION_FIRST / REJECTION_DECISION / ...
    int oa_ordinal = 0;
    std::string confidence;      // HIGH / MEDIUM / LOW
    bool is_official = true;     // false = 申请人提交文件，直接丢弃
    bool is_remindable = false;  // OA/驳回/授权/补正/其他官方
};

EnClassification ClassifyOfficialDocumentEn(const std::string& raw_title,
                                             const std::string& document_code);

// ---- 跨来源事件键（与 Python ProsecutionDocument.event_key 一致）----
// sha256("CN|申请号|类型|次数|官文日|规范化标题") 的十六进制。
std::string ComputeEventKey(const std::string& jurisdiction,
                            const std::string& application_number,
                            const std::string& document_type,
                            int oa_ordinal,
                            const std::string& official_date,
                            const std::string& document_title);

// ---- family JSON 解析（纯函数，测试可注入）----
// 输入 GET /patent-family/svc/family/application/CN/{12位} 的响应体，
// 输出可入库的官方事件列表。错误码沿用 sidecar 协议字符串：
// RATE_LIMITED / CASE_NOT_FOUND / PAGE_STRUCTURE_CHANGED / TEMPORARY_ERROR。
struct ParseResult {
    std::string code;            // "OK" 或错误码
    std::string message;
    std::vector<RemoteDocument> documents;
};

ParseResult ParseFamilyJson(const std::string& body,
                            const std::string& application_number,
                            const std::string& publication_number);

// ---- 原生客户端 ----
// fetch_json 可注入（测试用）；默认实现走 libcurl（浏览器 UA + Referer +
// 最小 2 秒节流，与 Python 版一致）。
using FetchJson = std::function<bool(const std::string& url, std::string& body)>;

class NativeUsptoClient {
public:
    explicit NativeUsptoClient(FetchJson fetch = nullptr);

    // 返回 ParseResult；网络失败时 code=NETWORK_ERROR。
    ParseResult FetchDocuments(const std::string& application_number_12_digits,
                               const std::string& application_number,
                               const std::string& publication_number);

private:
    FetchJson fetch_;
};

} // namespace uspto
} // namespace webdossier
