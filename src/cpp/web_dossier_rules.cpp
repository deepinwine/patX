// Pure, GUI-free helpers for the web dossier sync: Chinese OA type
// normalization and result-code mapping. Kept in their own translation unit
// so the offline unit tests link without wxWidgets.
#include "web_dossier.hpp"

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace webdossier {

const char* ToString(ResultCode c) {
    switch (c) {
        case ResultCode::Ok: return "OK";
        case ResultCode::NoChange: return "NO_CHANGE";
        case ResultCode::NewOfficialEvent: return "NEW_OFFICIAL_EVENT";
        case ResultCode::AuthRequired: return "AUTH_REQUIRED";
        case ResultCode::SessionExpired: return "SESSION_EXPIRED";
        case ResultCode::CaseNotFound: return "CASE_NOT_FOUND";
        case ResultCode::PublicationNotAvailable: return "PUBLICATION_NOT_AVAILABLE";
        case ResultCode::AccessDenied: return "ACCESS_DENIED";
        case ResultCode::ResolveFailed: return "RESOLVE_FAILED";
        case ResultCode::PageStructureChanged: return "PAGE_STRUCTURE_CHANGED";
        case ResultCode::RateLimited: return "RATE_LIMITED";
        case ResultCode::TemporaryError: return "TEMPORARY_ERROR";
        case ResultCode::NetworkError: return "NETWORK_ERROR";
        case ResultCode::DateParseFailed: return "DATE_PARSE_FAILED";
        case ResultCode::DateConflict: return "DATE_CONFLICT";
        case ResultCode::UnsupportedJurisdiction: return "UNSUPPORTED_JURISDICTION";
        case ResultCode::ManualReviewRequired: return "MANUAL_REVIEW_REQUIRED";
        case ResultCode::SidecarError: return "SIDECAR_ERROR";
        case ResultCode::Cancelled: return "CANCELLED";
    }
    return "UNKNOWN";
}

ResultCode ResultCodeFromString(const std::string& name) {
    static const std::pair<const char*, ResultCode> map[] = {
        {"OK", ResultCode::Ok},
        {"NO_CHANGE", ResultCode::NoChange},
        {"NEW_OFFICIAL_EVENT", ResultCode::NewOfficialEvent},
        {"NEW_OFFICE_ACTION", ResultCode::NewOfficialEvent},
        {"AUTH_REQUIRED", ResultCode::AuthRequired},
        {"SESSION_EXPIRED", ResultCode::SessionExpired},
        {"CASE_NOT_FOUND", ResultCode::CaseNotFound},
        {"PUBLICATION_NOT_AVAILABLE", ResultCode::PublicationNotAvailable},
        {"ACCESS_DENIED", ResultCode::AccessDenied},
        {"RESOLVE_FAILED", ResultCode::ResolveFailed},
        {"PAGE_STRUCTURE_CHANGED", ResultCode::PageStructureChanged},
        {"RATE_LIMITED", ResultCode::RateLimited},
        {"TEMPORARY_ERROR", ResultCode::TemporaryError},
        {"NETWORK_ERROR", ResultCode::NetworkError},
        {"DATE_PARSE_FAILED", ResultCode::DateParseFailed},
        {"DATE_CONFLICT", ResultCode::DateConflict},
        {"UNSUPPORTED_JURISDICTION", ResultCode::UnsupportedJurisdiction},
        {"MANUAL_REVIEW_REQUIRED", ResultCode::ManualReviewRequired},
    };
    for (const auto& e : map) {
        if (name == e.first) return e.second;
    }
    return ResultCode::SidecarError;
}

namespace {

bool IsValidIsoDate(const std::string& value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (value[i] < '0' || value[i] > '9') return false;
    }
    const int year = atoi(value.substr(0, 4).c_str());
    const int month = atoi(value.substr(5, 2).c_str());
    const int day = atoi(value.substr(8, 2).c_str());
    if (year < 1 || month < 1 || month > 12 || day < 1) return false;
    static const int days_per_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    int last_day = days_per_month[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) last_day = 29;
    return day <= last_day;
}

// 全角 digit -> ASCII
std::string NormalizeFullwidth(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0xEF && i + 2 < s.size()) {
            // U+FF10..FF19 fullwidth digits encode as EF BC 90..EF BC 99
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            if (c1 == 0xBC && c2 >= 0x90 && c2 <= 0x99) {
                out += static_cast<char>('0' + (c2 - 0x90));
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

int ChineseNumberToOrdinal(const std::string& zh) {
    if (zh == "一" || zh == "1") return 1;
    if (zh == "二" || zh == "两" || zh == "2") return 2;
    if (zh == "三" || zh == "3") return 3;
    if (zh == "四" || zh == "4") return 4;
    if (zh == "五" || zh == "5") return 5;
    if (zh == "六" || zh == "6") return 6;
    if (zh == "七" || zh == "7") return 7;
    if (zh == "八" || zh == "8") return 8;
    if (zh == "九" || zh == "9") return 9;
    if (zh == "十") return 10;
    int v = atoi(zh.c_str());
    return v > 0 && v < 100 ? v : 0;
}

std::string OrdinalToChinese(int n) {
    static const char* zh[] = {"", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"};
    if (n >= 1 && n <= 10) return zh[n];
    return std::to_string(n);
}

} // namespace

bool IsOfficeActionTypeCn(const std::string& normalized) {
    return normalized.find("审查意见通知书") != std::string::npos;
}

int OaTypeOrdinalCn(const std::string& raw) {
    std::string s = NormalizeFullwidth(raw);
    size_t pos = s.find("第");
    while (pos != std::string::npos) {
        size_t after = pos + 3;   // "第" is 3 bytes in UTF-8
        if (after < s.size()) {
            if (static_cast<unsigned char>(s[after]) >= '0' &&
                static_cast<unsigned char>(s[after]) <= '9') {
                size_t end = after;
                while (end < s.size() && s[end] >= '0' && s[end] <= '9') end++;
                int v = atoi(s.substr(after, end - after).c_str());
                if (v > 0) return v;
            } else {
                std::string zh = s.substr(after, 3);
                int v = ChineseNumberToOrdinal(zh);
                if (v > 0) return v;
            }
        }
        pos = s.find("第", pos + 3);
    }
    if (s.find("一通") != std::string::npos) return 1;
    if (s.find("二通") != std::string::npos) return 2;
    if (s.find("三通") != std::string::npos) return 3;
    if (s.find("四通") != std::string::npos) return 4;
    return 0;
}

std::string NormalizeOaTypeCn(const std::string& raw) {
    std::string s = NormalizeFullwidth(raw);
    // collapse inner spaces so "第二次 审查意见通知书" still matches
    std::string compact;
    for (char c : s) {
        if (c != ' ' && c != '\t') compact += c;
    }
    if (compact.find("审查意见通知书") == std::string::npos) {
        // internal abbreviations: 一通/二通/三通/四通
        int abbr = 0;
        if (compact.find("一通") != std::string::npos) abbr = 1;
        else if (compact.find("二通") != std::string::npos) abbr = 2;
        else if (compact.find("三通") != std::string::npos) abbr = 3;
        else if (compact.find("四通") != std::string::npos) abbr = 4;
        if (abbr == 0) return raw;   // not OA family
        return "第" + OrdinalToChinese(abbr) + "次审查意见通知书";
    }

    int ordinal = OaTypeOrdinalCn(compact);
    if (ordinal >= 1) return "第" + OrdinalToChinese(ordinal) + "次审查意见通知书";
    return "审查意见通知书";
}

std::string OfficialEventTitleCn(const RemoteDocument& document) {
    if (document.document_type == "OFFICE_ACTION_FIRST") {
        return "第一次审查意见通知书";
    }
    if (document.document_type == "OFFICE_ACTION_SECOND") {
        return "第二次审查意见通知书";
    }
    if (document.document_type == "OFFICE_ACTION_NTH") {
        if (document.oa_ordinal > 0) {
            return "第" + OrdinalToChinese(document.oa_ordinal) + "次审查意见通知书";
        }
        return NormalizeOaTypeCn(document.document_title);
    }
    if (document.document_type == "REJECTION_DECISION") return "驳回决定";
    if (document.document_type == "GRANT_NOTICE") return "授权通知";
    if (document.document_type == "CORRECTION_NOTICE") return "补正通知";
    if (document.document_type == "OTHER_OFFICIAL") {
        if (!document.document_title.empty()) return document.document_title;
        if (!document.raw_title.empty()) return document.raw_title;
        return "其他官方通知";
    }
    return "";
}

bool IsPersistableOfficialEvent(const RemoteDocument& document) {
    return document.direction == "official" &&
           document.document_version == "ORIGINAL" &&
           document.confidence == "HIGH" &&
           !OfficialEventTitleCn(document).empty() &&
           IsValidIsoDate(document.official_date);
}

EventMergeResult MergeOfficialEvent(Database& db, const Patent& patent,
                                    const RemoteDocument& document) {
    EventMergeResult result;
    result.canonical_title = OfficialEventTitleCn(document);

    if (document.direction != "official" || document.document_version != "ORIGINAL" ||
        result.canonical_title.empty()) {
        return result;
    }
    if (document.confidence != "HIGH") {
        result.code = ResultCode::ManualReviewRequired;
        result.message = "置信度 " + document.confidence + "，待人工确认: " +
                         result.canonical_title;
        return result;
    }
    if (!IsValidIsoDate(document.official_date)) {
        result.code = ResultCode::DateParseFailed;
        result.message = "官方事件缺少可解析的官方发文日: " + result.canonical_title;
        return result;
    }

    auto existing = db.GetOAsForPatentId(patent.id);
    const OARecord* same_identity = nullptr;
    int same_identity_source_rank = -1;
    for (const auto& record : existing) {
        const bool same_remote_id = !document.remote_document_id.empty() &&
                                    !record.remote_document_id.empty() &&
                                    record.remote_document_id == document.remote_document_id;
        const bool compatible_source = document.source.empty() || record.source.empty() ||
                                       record.source == document.source;
        if (same_remote_id && compatible_source) {
            const int source_rank = !document.source.empty() &&
                                            record.source == document.source
                                        ? 2
                                        : 1;
            if (source_rank > same_identity_source_rank) {
                same_identity = &record;
                same_identity_source_rank = source_rank;
            }
        }
    }

    if (same_identity) {
        if (same_identity->issue_date.empty()) {
            db.UpdateOASyncFields(same_identity->id, document.official_date,
                                  "auto_filled_date", document.source,
                                  document.remote_document_id);
            result.message = "已补入官方发文日 " + document.official_date;
        } else if (same_identity->issue_date != document.official_date) {
            db.UpdateOASyncFields(same_identity->id, "", "date_conflict",
                                  document.source, document.remote_document_id);
            result.code = ResultCode::DateConflict;
            result.date_conflict = true;
            result.message = "本地 " + result.canonical_title + " 官方发文日 " +
                             same_identity->issue_date + "，官网 " + document.official_date +
                             "，待人工确认";
        } else if (same_identity->source.empty()) {
            db.UpdateOASyncFields(same_identity->id, "", same_identity->sync_flag,
                                  document.source, document.remote_document_id);
        }
        return result;
    }

    const OARecord* adoptable_legacy = nullptr;
    int adoptable_source_rank = -1;
    const OARecord* fallback_duplicate = nullptr;
    for (const auto& record : existing) {
        const std::string local_title = IsOfficeActionTypeCn(record.oa_type)
                                            ? NormalizeOaTypeCn(record.oa_type)
                                            : record.oa_type;
        if (local_title != result.canonical_title) continue;
        const bool sync_managed = !record.source.empty() || !record.sync_flag.empty();
        const bool no_stable_identity = record.remote_document_id.empty();
        const bool compatible_source = document.source.empty() || record.source.empty() ||
                                       record.source == document.source;
        if (!sync_managed || !no_stable_identity || !compatible_source) continue;
        if (record.issue_date.empty()) {
            const int source_rank = !document.source.empty() &&
                                            record.source == document.source
                                        ? 2
                                        : 1;
            if (source_rank > adoptable_source_rank) {
                adoptable_legacy = &record;
                adoptable_source_rank = source_rank;
            }
        } else if (document.remote_document_id.empty() &&
                   record.issue_date == document.official_date && !fallback_duplicate) {
            fallback_duplicate = &record;
        }
    }

    if (adoptable_legacy) {
        db.UpdateOASyncFields(adoptable_legacy->id, document.official_date,
                              "auto_filled_date", document.source,
                              document.remote_document_id);
        result.message = "已补入官方发文日 " + document.official_date;
        return result;
    }
    if (fallback_duplicate) return result;

    OARecord record;
    record.patent_id = patent.id;
    record.geke_code = patent.geke_code;
    record.patent_title = patent.title;
    record.oa_type = result.canonical_title;
    record.issue_date = document.official_date;
    record.jurisdiction = "CN";
    record.source = document.source;
    record.remote_document_id = document.remote_document_id;
    record.sync_flag = "web_new";
    result.oa_created_id = db.InsertOA(record, /*log_undo=*/true);
    if (result.oa_created_id > 0) {
        result.code = ResultCode::NewOfficialEvent;
        result.message = "发现新官方事件: " + result.canonical_title + " @ " +
                         document.official_date;
    }
    return result;
}

int NormalizeCheckIntervalDays(int days) {
    return days == 3 || days == 7 ? days : 1;
}

long long NextDossierCheckAt(ResultCode code, long long now, int interval_days) {
    if (code == ResultCode::NetworkError || code == ResultCode::RateLimited ||
        code == ResultCode::PageStructureChanged) {
        return now + 1800;
    }
    return now + static_cast<long long>(NormalizeCheckIntervalDays(interval_days)) * 86400;
}

bool ParseRemoteCaseResultJson(const std::string& json_body, RemoteCaseResult& out) {
    try {
        auto parsed = json::parse(json_body);
        out = RemoteCaseResult{};
        out.ok = parsed.value("ok", false);
        out.code = parsed.value("code", "SIDECAR_ERROR");
        out.message = parsed.value("message", "");
        out.resolved_application_number = parsed.value("resolved_application_number", "");
        out.auth_state = parsed.value("auth_state", "");
        out.provider_used = parsed.value("provider_used", "");

        auto parse_document = [&](const json& value) {
            RemoteDocument doc;
            doc.source = value.value("source", out.provider_used);
            if (doc.source.empty()) doc.source = out.provider_used;
            doc.document_type = value.value("document_type", "UNKNOWN");
            doc.document_title = value.value("document_title", "");
            doc.raw_title = value.value("raw_title", "");
            doc.document_code = value.value("document_code", "");
            doc.document_version = value.value("document_version", "ORIGINAL");
            doc.official_date = value.value("official_date", "");
            doc.direction = value.value("direction", "");
            doc.remote_document_id = value.value("remote_document_id", "");
            doc.source_url = value.value("source_url", "");
            doc.download_url = value.value("download_url", "");
            doc.download_available = value.value("download_available", false);
            doc.fingerprint = value.value("fingerprint", "");
            doc.event_key = value.value("event_key", "");
            if (value.contains("source_trace") && value["source_trace"].is_array()) {
                for (const auto& source : value["source_trace"]) {
                    if (!source.is_string()) continue;
                    if (!doc.source_trace.empty()) doc.source_trace += ',';
                    doc.source_trace += source.get<std::string>();
                }
            } else {
                doc.source_trace = value.value("source_trace", "");
            }
            doc.raw_metadata = value.dump();
            doc.confidence = value.value("confidence", "HIGH");
            doc.oa_ordinal = value.value("oa_ordinal", 0);
            doc.ds = value.value("ds", "");
            doc.wenjiandm = value.value("wenjiandm", "");
            if (doc.document_code.empty()) doc.document_code = doc.wenjiandm;
            return doc;
        };

        if (parsed.contains("documents") && parsed["documents"].is_array()) {
            for (const auto& value : parsed["documents"]) {
                out.documents.push_back(parse_document(value));
            }
        }
        if (parsed.contains("latest_event") && parsed["latest_event"].is_object()) {
            out.has_latest_event = true;
            out.latest_event = parse_document(parsed["latest_event"]);
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

CaseSyncReport ApplyRemoteCaseResult(Database& db, const Patent& patent,
                                     const RemoteCaseResult& remote,
                                     int interval_days, long long now) {
    CaseSyncReport report;
    report.patent_id = patent.id;
    report.geke_code = patent.geke_code;
    report.code = ResultCodeFromString(remote.code);
    report.message = remote.message;

    DossierSyncState state;
    state.patent_id = patent.id;
    state.provider = remote.provider_used.empty() ? "cnipa" : remote.provider_used;
    state.last_checked_at = now;
    state.auth_state = remote.auth_state;

    const std::string app_no = remote.resolved_application_number.empty()
                                   ? patent.application_number
                                   : remote.resolved_application_number;
    for (const auto& document : remote.documents) {
        if (!IsPersistableOfficialEvent(document)) continue;
        ProsecutionDocumentRecord record;
        record.patent_id = patent.id;
        record.jurisdiction = "CN";
        record.application_number = app_no;
        record.publication_number = patent.publication_number;
        record.source = document.source.empty() ? state.provider : document.source;
        record.remote_document_id = document.remote_document_id;
        record.document_type = document.document_type;
        record.document_title = document.document_title;
        record.raw_title = document.raw_title;
        record.document_code = document.document_code;
        record.document_version = document.document_version;
        record.official_date = document.official_date;
        record.direction = document.direction;
        record.source_url = document.source_url;
        record.download_url = document.download_url;
        record.download_available = document.download_available;
        record.fingerprint = document.fingerprint;
        record.event_key = document.event_key;
        record.source_trace = document.source_trace;
        record.raw_metadata = document.raw_metadata;
        bool created = false;
        db.UpsertProsecutionDocument(record, &created);
        if (created) report.documents_new++;
    }
    report.documents_total = static_cast<int>(remote.documents.size());

    if (remote.has_latest_event) {
        RemoteDocument latest = remote.latest_event;
        if (latest.source.empty()) latest.source = state.provider;
        EventMergeResult merged = MergeOfficialEvent(db, patent, latest);
        report.code = merged.code;
        report.oa_created_id = merged.oa_created_id;
        report.date_conflict = merged.date_conflict;
        report.latest_remote_oa_type = merged.canonical_title;
        report.latest_remote_oa_date = latest.official_date;
        report.message = merged.message;
        state.latest_remote_oa_date = latest.official_date;
        state.latest_remote_oa_type = merged.canonical_title;
    } else if (report.code == ResultCode::Ok || report.code == ResultCode::NoChange) {
        report.code = ResultCode::NoChange;
    }

    if (report.code == ResultCode::Ok || report.code == ResultCode::NewOfficialEvent ||
        report.code == ResultCode::NoChange || report.code == ResultCode::DateConflict) {
        state.last_success_at = now;
    } else {
        state.last_error_at = now;
        state.last_error_code = ToString(report.code);
        state.last_error_message = report.message;
    }
    db.UpsertDossierSyncState(state);
    db.UpdatePatentDossierCheck(
        patent.id, now, NextDossierCheckAt(report.code, now, interval_days));
    return report;
}

void AccumulateBatchSummary(BatchSummary& summary, const CaseSyncReport& report) {
    summary.checked++;
    switch (report.code) {
        case ResultCode::NewOfficialEvent:
            summary.new_events++;
            summary.findings.push_back(report);
            break;
        case ResultCode::DateConflict:
        case ResultCode::ManualReviewRequired:
            summary.manual_review++;
            summary.findings.push_back(report);
            break;
        case ResultCode::NoChange:
        case ResultCode::Ok:
            summary.no_change++;
            break;
        case ResultCode::AuthRequired:
        case ResultCode::SessionExpired:
            summary.auth_required++;
            summary.findings.push_back(report);
            break;
        case ResultCode::Cancelled:
            break;
        default:
            summary.failed++;
            summary.failures.push_back(report);
            break;
    }
}

bool ShouldStartBackgroundSync(bool busy, std::size_t due_count) {
    return !busy && due_count > 0;
}

bool ShouldOpenBackgroundNotification(bool busy) {
    return !busy;
}

bool CanChangeBackgroundSettings(bool busy) {
    return !busy;
}

bool ApplyBackgroundIntervalIfIdle(
    bool busy, int days, const std::function<void(int)>& save_interval) {
    if (!CanChangeBackgroundSettings(busy)) return false;
    if (save_interval) save_interval(NormalizeCheckIntervalDays(days));
    return true;
}

BackgroundUiDecision DecideBackgroundUi(const BatchSummary& summary) {
    BackgroundUiDecision decision;
    decision.new_events = summary.new_events;
    for (const auto& finding : summary.findings) {
        if (finding.code == ResultCode::DateConflict) {
            decision.date_conflicts++;
        }
    }
    decision.manual_reviews = summary.manual_review > decision.date_conflicts
                                  ? summary.manual_review - decision.date_conflicts
                                  : 0;
    decision.refresh_oa = summary.new_events > 0 || summary.manual_review > 0;
    if (summary.auth_required > 0) {
        decision.notification = BackgroundNotificationKind::LoginRequired;
    } else if (summary.new_events > 0 || summary.manual_review > 0) {
        decision.notification = BackgroundNotificationKind::Findings;
    }
    return decision;
}

int BackgroundTimerDelayMs(bool first_run) {
    return first_run ? 30 * 1000 : 30 * 60 * 1000;
}

WorkerLaunchOptions WorkerLaunchOptionsFor(SyncRunKind kind) {
    switch (kind) {
        case SyncRunKind::Background:
            return {false, true};
        case SyncRunKind::ManualBatch:
        case SyncRunKind::Login:
            return {true, false};
    }
    return {};
}

bool BackgroundControllerCore::TryStartBackground(
    bool busy, const DueQueueLoader& load_due,
    const EnsureManager& ensure_manager, const StartWorker& start_worker) {
    if (busy) return false;
    const auto queue = load_due();
    if (queue.empty()) return false;
    if (!ensure_manager()) return false;
    start_worker(queue, WorkerLaunchOptionsFor(SyncRunKind::Background));
    return true;
}

void BackgroundControllerCore::CompleteBackground(
    const BatchSummary& summary, const std::function<void()>& refresh,
    const NotificationFactory& notify, const NotificationRoute& route) {
    const auto decision = DecideBackgroundUi(summary);
    if (decision.refresh_oa && refresh) refresh();

    notification_handle_.reset();
    if (decision.notification == BackgroundNotificationKind::None) return;

    last_summary_ = summary;
    const auto action = decision.notification == BackgroundNotificationKind::LoginRequired
                            ? BackgroundNotificationClick::ShowLoginRequired
                            : BackgroundNotificationClick::ShowFindings;
    if (notify) notification_handle_ = notify(summary, action, route);
}

void BackgroundControllerCore::ClearNotification() {
    notification_handle_.reset();
}

DossierWorkerOwner::~DossierWorkerOwner() {
    CancelAndJoin();
}

void DossierWorkerOwner::Adopt(std::unique_ptr<IJoinableDossierWorker> worker) {
    CancelAndJoin();
    worker_ = std::move(worker);
}

void DossierWorkerOwner::Join() {
    if (!worker_) return;
    worker_->Join();
    worker_.reset();
}

void DossierWorkerOwner::CancelAndJoin() {
    if (!worker_) return;
    worker_->RequestCancel();
    Join();
}

void BindDossierRefresh(DossierCompletionTarget& target,
                        std::function<void()> refresh) {
    target.SetDossierCompletionCallback(std::move(refresh));
}

} // namespace webdossier
