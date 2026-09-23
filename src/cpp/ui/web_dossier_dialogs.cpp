#include "ui/web_dossier_dialogs.hpp"
#include "ui/epo_family_dialog.hpp"

#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/thread.h>

#include <sstream>

#ifndef UTF8_STR
#define UTF8_STR(x) wxString::FromUTF8(x)
#endif

// ---------------------------------------------------------------------------
// Progress dialog with live counters, cancel, and the findings list
// ---------------------------------------------------------------------------

class WebDossierSyncDialog : public wxDialog {
public:
    WebDossierSyncDialog(wxWindow* parent, WebDossierController& ctrl)
        : wxDialog(parent, wxID_ANY, UTF8_STR("审查信息同步 / Dossier Sync"),
                   wxDefaultPosition, wxSize(680, 540),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          ctrl_(ctrl) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        stats_ = new wxStaticText(this, wxID_ANY,
                                  UTF8_STR("已检查：0    新官方事件：0    无变化：0\n"
                                           "需人工确认：0    需登录：0    失败：0"));
        stats_->SetFont(stats_->GetFont().Bold());
        sizer->Add(stats_, 0, wxALL | wxEXPAND, 10);

        current_ = new wxStaticText(this, wxID_ANY, UTF8_STR("正在启动查询引擎..."));
        sizer->Add(current_, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

        findings_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                   wxLC_REPORT | wxBORDER_SUNKEN);
        findings_->InsertColumn(0, UTF8_STR("案件"), wxLIST_FORMAT_LEFT, 110);
        findings_->InsertColumn(1, UTF8_STR("审查意见"), wxLIST_FORMAT_LEFT, 230);
        findings_->InsertColumn(2, UTF8_STR("官文日"), wxLIST_FORMAT_LEFT, 110);
        findings_->InsertColumn(3, UTF8_STR("状态"), wxLIST_FORMAT_LEFT, 140);
        sizer->Add(findings_, 1, wxALL | wxEXPAND, 5);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        view_btn_ = new wxButton(this, wxID_ANY, UTF8_STR("查看案件"));
        view_btn_->Enable(false);
        view_btn_->Bind(wxEVT_BUTTON, &WebDossierSyncDialog::OnViewCase, this);
        buttons->Add(view_btn_, 0, wxRIGHT, 8);

        login_btn_ = new wxButton(this, wxID_ANY, UTF8_STR("打开浏览器登录"));
        login_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            login_btn_->Enable(false);
            current_->SetLabel(
                UTF8_STR("已打开浏览器，请在其中完成登录；登录成功后会提示重试同步。"));
            ctrl_.Login();
        });
        login_btn_->Show(false);
        buttons->Add(login_btn_, 0, wxRIGHT, 8);

        buttons->AddStretchSpacer();
        close_btn_ = new wxButton(this, wxID_ANY, UTF8_STR("取消"));
        close_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (ctrl_.busy()) {
                if (wxMessageBox(UTF8_STR("正在同步，确定取消剩余案件吗？"),
                                 UTF8_STR("取消同步"), wxYES_NO | wxICON_QUESTION) != wxYES)
                    return;
                ctrl_.cancel_ = true;
                close_btn_->SetLabel(UTF8_STR("取消中..."));
                close_btn_->Enable(false);
            } else {
                Show(false);
            }
        });
        buttons->Add(close_btn_, 0, 0, 0);
        sizer->Add(buttons, 0, wxALL | wxEXPAND, 8);

        SetSizer(sizer);
    }

    void BeginBatch(int total) {
        checked_ = new_oa_ = no_change_ = review_ = auth_ = failed_ = 0;
        findings_->DeleteAllItems();
        view_btn_->Enable(false);
        login_btn_->Show(false);
        close_btn_->SetLabel(UTF8_STR("取消"));
        close_btn_->Enable(true);
        UpdateStats();
        (void)total;
    }

    void OnCaseStart(int index, int total, const std::string& geke_code) {
        current_->SetLabel(wxString::Format(UTF8_STR("当前(%d/%d)：%s 正在查询官方数据源..."),
                                            index + 1, total,
                                            wxString::FromUTF8(geke_code.c_str())));
    }

    void OnCaseDone(const webdossier::CaseSyncReport& r) {
        checked_++;
        switch (r.code) {
            case webdossier::ResultCode::NewOfficialEvent:
            case webdossier::ResultCode::DateConflict: new_oa_++; break;
            case webdossier::ResultCode::NoChange:
            case webdossier::ResultCode::Ok: no_change_++; break;
            case webdossier::ResultCode::ManualReviewRequired: review_++; break;
            case webdossier::ResultCode::AuthRequired:
            case webdossier::ResultCode::SessionExpired: auth_++; break;
            default: failed_++; break;
        }

        if (r.code == webdossier::ResultCode::NewOfficialEvent ||
            r.code == webdossier::ResultCode::DateConflict ||
            r.code == webdossier::ResultCode::ManualReviewRequired) {
            long idx = findings_->InsertItem(findings_->GetItemCount(),
                                             wxString::FromUTF8(r.geke_code.c_str()));
            findings_->SetItem(idx, 1,
                               wxString::FromUTF8(r.latest_remote_oa_type.empty()
                                                      ? r.message.c_str()
                                                      : r.latest_remote_oa_type.c_str()));
            findings_->SetItem(idx, 2, r.latest_remote_oa_date.empty()
                                           ? "-"
                                           : wxString::FromUTF8(r.latest_remote_oa_date.c_str()));
            findings_->SetItem(idx, 3, r.code == webdossier::ResultCode::NewOfficialEvent
                                           ? UTF8_STR("★ 新发现")
                                           : (r.code == webdossier::ResultCode::DateConflict
                                                  ? UTF8_STR("日期冲突待确认")
                                                  : UTF8_STR("待人工确认")));
            findings_->SetItemData(idx, r.patent_id);
            if (r.code != webdossier::ResultCode::NewOfficialEvent)
                findings_->SetItemBackgroundColour(idx, wxColour(255, 242, 204));
            view_btn_->Enable(true);
        }
        UpdateStats();
    }

    void OnBatchDone(const webdossier::BatchSummary& s) {
        current_->SetLabel(
            wxString::Format(UTF8_STR("完成：检查 %d / %d 件，新官方事件 %d 件，需确认 %d 件，失败 %d 件"),
                             s.checked, s.total, s.new_events, s.manual_review, s.failed));
        close_btn_->SetLabel(UTF8_STR("关闭"));
        close_btn_->Enable(true);
        if (s.auth_required > 0) {
            login_btn_->Show(true);
            login_btn_->Enable(true);
            Layout();
        }
    }

    void OnLoginDone(bool ok) {
        if (ok) {
            login_btn_->Show(false);
            current_->SetLabel(UTF8_STR("登录成功。请重新发起同步。"));
        } else {
            login_btn_->Enable(true);
            current_->SetLabel(UTF8_STR("登录未完成（已取消或失败），可重试。"));
        }
        Layout();
    }

private:
    void UpdateStats() {
        stats_->SetLabel(wxString::Format(
            UTF8_STR("已检查：%d    新官方事件：%d    无变化：%d\n"
                     "需人工确认：%d    需登录：%d    失败：%d"),
            checked_, new_oa_, no_change_, review_, auth_, failed_));
    }

    void OnViewCase(wxCommandEvent&) {
        long sel = findings_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (sel < 0) return;
        long patent_id = findings_->GetItemData(sel);
        for (const auto& p : ctrl_.db_.SearchPatents("")) {
            if (p.id == patent_id) {
                if (ctrl_.on_show_case_) ctrl_.on_show_case_(p.geke_code);
                return;
            }
        }
    }

    WebDossierController& ctrl_;
    wxStaticText* stats_ = nullptr;
    wxStaticText* current_ = nullptr;
    wxListCtrl* findings_ = nullptr;
    wxButton* view_btn_ = nullptr;
    wxButton* login_btn_ = nullptr;
    wxButton* close_btn_ = nullptr;

    int checked_ = 0, new_oa_ = 0, no_change_ = 0, review_ = 0, auth_ = 0, failed_ = 0;
};

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

namespace {

const wxEventTypeTag<wxThreadEvent> kEvtDossierCaseStart(wxNewEventType());
const wxEventTypeTag<wxThreadEvent> kEvtDossierCaseDone(wxNewEventType());
const wxEventTypeTag<wxThreadEvent> kEvtDossierBatchDone(wxNewEventType());

class DossierWorker : public wxThread {
public:
    DossierWorker(webdossier::Manager& manager, std::vector<Patent> queue,
                  bool login_only, std::atomic<bool>& cancel, wxEvtHandler* sink)
        : wxThread(wxTHREAD_DETACHED), manager_(manager), queue_(std::move(queue)),
          login_only_(login_only), cancel_(cancel), sink_(sink) {}

protected:
    ExitCode Entry() override {
        if (login_only_) {
            auto code = manager_.Login("cnipa", cancel_);
            auto* ev = new wxThreadEvent(kEvtDossierBatchDone);
            ev->SetExtraLong(1);              // login result marker
            ev->SetInt(code == webdossier::ResultCode::Ok ? 1 : 0);
            wxQueueEvent(sink_, ev);
            return nullptr;
        }

        webdossier::BatchSummary summary;
        summary.total = static_cast<int>(queue_.size());
        int index = 0;
        for (const auto& p : queue_) {
            if (cancel_.load()) break;
            {
                auto* ev = new wxThreadEvent(kEvtDossierCaseStart);
                ev->SetInt(index);
                ev->SetExtraLong(summary.total);
                ev->SetString(wxString::FromUTF8(p.geke_code.c_str()));
                wxQueueEvent(sink_, ev);
            }
            auto report = manager_.SyncCase(p, cancel_);
            auto* ev = new wxThreadEvent(kEvtDossierCaseDone);
            ev->SetPayload(report);
            wxQueueEvent(sink_, ev);

            summary.checked++;
            switch (report.code) {
                case webdossier::ResultCode::NewOfficialEvent:
                case webdossier::ResultCode::DateConflict:
                    summary.new_events++;
                    summary.findings.push_back(report);
                    break;
                case webdossier::ResultCode::ManualReviewRequired:
                    summary.manual_review++;
                    summary.findings.push_back(report);
                    break;
                case webdossier::ResultCode::AuthRequired:
                case webdossier::ResultCode::SessionExpired:
                    summary.auth_required++;
                    summary.findings.push_back(report);
                    // stop the batch; remaining cases stay untouched, not failed
                    index = -1;
                    break;
                case webdossier::ResultCode::NoChange:
                case webdossier::ResultCode::Ok:
                    summary.no_change++;
                    break;
                case webdossier::ResultCode::Cancelled:
                    index = -1;
                    break;
                default:
                    summary.failed++;
                    summary.failures.push_back(report);
                    if (report.code == webdossier::ResultCode::RateLimited ||
                        report.code == webdossier::ResultCode::PageStructureChanged ||
                        report.code == webdossier::ResultCode::NetworkError ||
                        report.code == webdossier::ResultCode::AccessDenied)
                        index = -1;
                    break;
            }
            if (index < 0) break;
            index++;
        }

        auto* done = new wxThreadEvent(kEvtDossierBatchDone);
        done->SetExtraLong(2);              // batch result marker
        done->SetPayload(summary);
        wxQueueEvent(sink_, done);
        return nullptr;
    }

private:
    webdossier::Manager& manager_;
    std::vector<Patent> queue_;
    bool login_only_ = false;
    std::atomic<bool>& cancel_;
    wxEvtHandler* sink_;
};

} // namespace

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

namespace {
std::string ResolveScriptDir() {
    wxString exe = wxStandardPaths::Get().GetExecutablePath();
    wxFileName fn(exe);
    std::string dir = fn.GetPath().ToStdString();
    const char* candidates[] = {"tools/web_dossier", "../tools/web_dossier",
                                "../../tools/web_dossier"};
    for (const char* c : candidates) {
        wxFileName full(wxString(dir) + "/" + c);
        full.Normalize();
        if (wxFileExists(full.GetFullPath() + "/service.py")) {
            return full.GetFullPath().ToStdString();
        }
    }
    return "tools/web_dossier";   // last resort: relative to the cwd
}
} // namespace

WebDossierController::WebDossierController(
    wxWindow* parent, Database& db, std::function<void(const std::string&)> on_show_case)
    : parent_(parent), db_(db), on_show_case_(std::move(on_show_case)) {
    Bind(kEvtDossierCaseStart, [this](wxThreadEvent& e) {
        if (active_dialog_)
            active_dialog_->OnCaseStart(e.GetInt(), static_cast<int>(e.GetExtraLong()),
                                        e.GetString().ToStdString());
    });
    Bind(kEvtDossierCaseDone, [this](wxThreadEvent& e) {
        if (active_dialog_) active_dialog_->OnCaseDone(e.GetPayload<webdossier::CaseSyncReport>());
    });
    Bind(kEvtDossierBatchDone, &WebDossierController::OnBatchDone, this);
}

WebDossierController::~WebDossierController() {
    if (active_dialog_) {
        active_dialog_->Destroy();
        active_dialog_ = nullptr;
    }
}

bool WebDossierController::EnsureManager(std::string& error) {
    if (!manager_) manager_ = std::make_unique<webdossier::Manager>(db_, ResolveScriptDir());
    return manager_->EnsureRunning(error);
}

void WebDossierController::SyncPatents(const std::vector<Patent>& patents) {
    if (patents.empty()) return;
    std::string error;
    if (!EnsureManager(error)) {
        wxMessageBox(wxString::FromUTF8(error.c_str()), UTF8_STR("审查信息同步"),
                     wxOK | wxICON_ERROR, parent_);
        return;
    }
    StartWorker(patents, false);
}

void WebDossierController::SyncAllActive(bool include_granted) {
    std::string error;
    if (!EnsureManager(error)) {
        wxMessageBox(wxString::FromUTF8(error.c_str()), UTF8_STR("审查信息同步"),
                     wxOK | wxICON_ERROR, parent_);
        return;
    }
    // Queue built on the main thread, executed one-by-one on the worker -
    // the C++-generates-queue / Python-executes split. Never concurrent.
    StartWorker(db_.GetPatentsForDossierCheck(include_granted, 0), false);
}

void WebDossierController::Login() {
    std::string error;
    if (!EnsureManager(error)) {
        wxMessageBox(wxString::FromUTF8(error.c_str()), UTF8_STR("审查信息同步"),
                     wxOK | wxICON_ERROR, parent_);
        return;
    }
    StartWorker({}, true);
}

void WebDossierController::StartWorker(const std::vector<Patent>& queue, bool is_login_only) {
    if (running_.load()) {
        wxMessageBox(UTF8_STR("已有同步任务在进行中"), UTF8_STR("审查信息同步"),
                     wxOK | wxICON_INFORMATION, parent_);
        return;
    }
    cancel_ = false;
    running_ = true;

    if (!active_dialog_) active_dialog_ = new WebDossierSyncDialog(parent_, *this);
    if (is_login_only) {
        active_dialog_->Show(true);
        active_dialog_->Raise();
    } else {
        active_dialog_->BeginBatch(static_cast<int>(queue.size()));
        active_dialog_->Show(true);
        active_dialog_->Raise();
    }

    auto* worker = new DossierWorker(*manager_, queue, is_login_only, cancel_, this);
    if (worker->Create() != wxTHREAD_NO_ERROR) {
        delete worker;
        running_ = false;
        wxMessageBox("failed to start worker", "Error", wxOK | wxICON_ERROR, parent_);
    } else {
        worker->Run();
    }
}

void WebDossierController::OnBatchDone(wxThreadEvent& event) {
    long kind = event.GetExtraLong();
    if (kind == 1) {   // login result
        running_ = false;
        bool ok = event.GetInt() == 1;
        if (active_dialog_) active_dialog_->OnLoginDone(ok);
        return;
    }
    running_ = false;
    auto summary = event.GetPayload<webdossier::BatchSummary>();
    if (active_dialog_) active_dialog_->OnBatchDone(summary);
    if (on_finished_) on_finished_();
    if (summary.auth_required > 0) {
        wxMessageBox(
            UTF8_STR("CNIPA 登录状态已失效。\n点击对话框中的【打开浏览器登录】完成登录后重试。"),
            UTF8_STR("需要登录"), wxOK | wxICON_WARNING, parent_);
    }
}

void WebDossierController::ShowFamily(const std::string& publication) {
    std::string error;
    if (!EnsureManager(error)) {
        wxMessageBox(wxString::FromUTF8(error.c_str()), UTF8_STR("EPO OPS"),
                     wxOK | wxICON_ERROR, parent_);
        return;
    }
    ShowEpoFamilyDialog(parent_, db_, *manager_, publication);
}

void WebDossierController::ShowHistory() {
    auto states = db_.GetDossierSyncStates(300);
    wxDialog dlg(parent_, wxID_ANY, UTF8_STR("同步历史 / Dossier Sync History"),
                 wxDefaultPosition, wxSize(760, 460),
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* list = new wxListCtrl(&dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxLC_REPORT | wxBORDER_SUNKEN);
    list->InsertColumn(0, UTF8_STR("案件ID"), wxLIST_FORMAT_LEFT, 70);
    list->InsertColumn(1, UTF8_STR("数据源"), wxLIST_FORMAT_LEFT, 90);
    list->InsertColumn(2, UTF8_STR("最近检查"), wxLIST_FORMAT_LEFT, 150);
    list->InsertColumn(3, UTF8_STR("最新远程OA"), wxLIST_FORMAT_LEFT, 210);
    list->InsertColumn(4, UTF8_STR("状态/错误"), wxLIST_FORMAT_LEFT, 210);
    int row = 0;
    char buf[40];
    for (const auto& s : states) {
        long idx = list->InsertItem(row, wxString::Format("%d", s.patent_id));
        list->SetItem(idx, 1, wxString::FromUTF8(s.provider.c_str()));
        if (s.last_checked_at > 0) {
            time_t t = static_cast<time_t>(s.last_checked_at);
            struct tm tmv;
#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900,
                     tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
            list->SetItem(idx, 2, buf);
        } else {
            list->SetItem(idx, 2, "-");
        }
        std::string oa = s.latest_remote_oa_type + " @ " + s.latest_remote_oa_date;
        list->SetItem(idx, 3, wxString::FromUTF8(oa.c_str()));
        std::string status = s.last_error_code.empty()
                                 ? (s.last_success_at > 0 ? "OK" : s.auth_state)
                                 : s.last_error_code + " " + s.last_error_message.substr(0, 60);
        list->SetItem(idx, 4, wxString::FromUTF8(status.c_str()));
        row++;
    }
    sizer->Add(list, 1, wxALL | wxEXPAND, 10);
    auto* ok = new wxButton(&dlg, wxID_OK, UTF8_STR("关闭"));
    sizer->Add(ok, 0, wxALL | wxALIGN_RIGHT, 8);
    dlg.SetSizer(sizer);
    dlg.ShowModal();
}

void WebDossierController::ShowErrorCases() {
    auto states = db_.GetDossierSyncStates(300);
    wxDialog dlg(parent_, wxID_ANY, UTF8_STR("异常案件 / Problem Cases"),
                 wxDefaultPosition, wxSize(760, 460),
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* list = new wxListCtrl(&dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxLC_REPORT | wxBORDER_SUNKEN);
    list->InsertColumn(0, UTF8_STR("案件ID"), wxLIST_FORMAT_LEFT, 70);
    list->InsertColumn(1, UTF8_STR("错误码"), wxLIST_FORMAT_LEFT, 190);
    list->InsertColumn(2, UTF8_STR("说明"), wxLIST_FORMAT_LEFT, 470);
    int row = 0;
    for (const auto& s : states) {
        if (s.last_error_code.empty()) continue;
        long idx = list->InsertItem(row, wxString::Format("%d", s.patent_id));
        list->SetItem(idx, 1, wxString::FromUTF8(s.last_error_code.c_str()));
        list->SetItem(idx, 2, wxString::FromUTF8(s.last_error_message.c_str()));
        if (s.last_error_code == "DATE_CONFLICT" || s.last_error_code == "MANUAL_REVIEW_REQUIRED")
            list->SetItemBackgroundColour(idx, wxColour(255, 242, 204));
        row++;
    }
    sizer->Add(list, 1, wxALL | wxEXPAND, 10);
    auto* ok = new wxButton(&dlg, wxID_OK, UTF8_STR("关闭"));
    sizer->Add(ok, 0, wxALL | wxALIGN_RIGHT, 8);
    dlg.SetSizer(sizer);
    dlg.ShowModal();
}
