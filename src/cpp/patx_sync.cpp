// patx_sync —— 命令行案件巡检（wx 无关，纯核心库）。
//
// 用法: patx_sync <数据库路径> <申请号...> [--due]
//   专利号参数逐件走原生 USPTO Global Dossier 层；
//   --due 按 next_dossier_check_at 到期队列巡检（无 GUI 定时同步）。
//
// 与 GUI 内巡检相同的规则：ORIGINAL 官方事件入库（event_key 去重）、
// 一通/后续分档算绝限、申请人提交晚于发文即填答复日期。
#include "database.hpp"
#include "patx/log.hpp"
#include "web_dossier.hpp"
#include "web_datasource.hpp"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

std::string ExtractAppDigits(const std::string& raw) {
    std::string digits;
    for (char c : raw) {
        if (c == '.') break;
        if (c >= '0' && c <= '9') digits += c;
    }
    if (digits.size() == 13) digits = digits.substr(0, 12);
    return digits.size() == 12 ? digits : "";
}

const webdossier::RemoteDocument* PickLatestOfficialEvent(
    const std::vector<webdossier::RemoteDocument>& docs) {
    static const std::set<std::string> remindable = {
        "OFFICE_ACTION_FIRST", "OFFICE_ACTION_SECOND", "OFFICE_ACTION_NTH",
        "REJECTION_DECISION", "GRANT_NOTICE", "CORRECTION_NOTICE", "OTHER_OFFICIAL"};
    const webdossier::RemoteDocument* best = nullptr;
    for (const auto& d : docs) {
        if (d.direction != "official" || d.official_date.empty()) continue;
        if (d.document_version != "ORIGINAL" && !d.document_version.empty()) continue;
        if (!remindable.count(d.document_type)) continue;
        if (!best ||
            std::tie(d.official_date, d.oa_ordinal, d.remote_document_id) >
            std::tie(best->official_date, best->oa_ordinal, best->remote_document_id)) {
            best = &d;
        }
    }
    return best;
}

int SyncPatent(Database& db, const Patent& p) {
    std::string digits = ExtractAppDigits(p.application_number);
    if (digits.empty()) {
        printf("  %s  跳过（无可识别申请号）\n", p.geke_code.c_str());
        return 1;
    }
    webdossier::uspto::NativeUsptoClient client;
    auto parsed = client.FetchDocuments(digits, p.application_number,
                                        p.publication_number);
    if (parsed.code != "OK") {
        printf("  %s  %s: %s\n", p.geke_code.c_str(), parsed.code.c_str(),
               parsed.message.c_str());
        return 1;
    }

    int created_docs = 0;
    for (const auto& d : parsed.documents) {
        ProsecutionDocumentRecord rec;
        rec.patent_id = p.id;
        rec.jurisdiction = "CN";
        rec.application_number = p.application_number;
        rec.publication_number = p.publication_number;
        rec.source = d.source;
        rec.remote_document_id = d.remote_document_id;
        rec.document_type = d.document_type;
        rec.document_title = d.document_title;
        rec.raw_title = d.raw_title;
        rec.official_date = d.official_date;
        rec.direction = d.direction;
        rec.fingerprint = d.fingerprint;
        rec.document_code = d.document_code;
        rec.document_version = d.document_version;
        rec.event_key = d.event_key;
        rec.source_trace = d.source_trace;
        bool created = false;
        db.UpsertProsecutionDocument(rec, &created);
        if (created) created_docs++;
    }

    std::string summary = "事件 " + std::to_string(parsed.documents.size()) +
                          " 条（新 " + std::to_string(created_docs) + "）";
    const auto* latest = PickLatestOfficialEvent(parsed.documents);
    if (latest) {
        auto merged = webdossier::MergeOfficialEvent(db, p, *latest);
        summary += "，最新: " + merged.canonical_title + " @ " +
                   latest->official_date + " (" +
                   webdossier::ToString(merged.code) + ")";
        if (!parsed.latest_applicant_activity.empty() &&
            parsed.latest_applicant_activity > latest->official_date) {
            int target = merged.oa_created_id;
            if (target == 0) {
                for (const auto& e : db.GetOAsForPatentId(p.id)) {
                    if (e.issue_date == latest->official_date) { target = e.id; break; }
                }
            }
            if (target > 0 &&
                db.FillOAResponseDateIfEmpty(target, parsed.latest_applicant_activity)) {
                summary += "，已答复 " + parsed.latest_applicant_activity;
            }
        }
    }
    long long now = static_cast<long long>(time(nullptr));
    db.UpdatePatentDossierCheck(p.id, now, now + 86400);
    printf("  %s  OK  %s\n", p.geke_code.c_str(), summary.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "用法: patx_sync <数据库路径> <申请号...> [--due]\n");
        return 2;
    }
    patx::InitLogging("patx_sync.log");
    Database db(argv[1]);
    if (!db.IsOpen()) {
        fprintf(stderr, "无法打开数据库: %s (%s)\n", argv[1], db.LastError().c_str());
        return 2;
    }

    std::vector<Patent> targets;
    bool due_mode = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--due") { due_mode = true; continue; }
        Patent p = db.GetPatentByApplicationNumber(argv[i]);
        if (p.id == 0) {
            printf("  %s  未在数据库中找到，跳过\n", argv[i]);
            continue;
        }
        targets.push_back(p);
    }
    if (due_mode) {
        auto due = db.GetPatentsDueForDossierCheck(
            false, static_cast<long long>(time(nullptr)), 0);
        for (const auto& p : due) targets.push_back(p);
        printf("到期队列: %zu 件\n", due.size());
    }

    int failed = 0;
    for (const auto& p : targets) {
        if (SyncPatent(db, p) != 0) failed++;
    }
    printf("完成: %zu 件，失败 %d\n", targets.size(), failed);
    return failed == 0 ? 0 : 1;
}
