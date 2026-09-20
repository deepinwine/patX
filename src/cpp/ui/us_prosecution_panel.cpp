#include "ui/us_prosecution_panel.hpp"

#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/textdlg.h>
#include <wx/spinctrl.h>
#include <wx/scrolwin.h>
#include <wx/filename.h>

#include <filesystem>
#include <sstream>
#include <set>

#define UTF8_STR(s) wxString::FromUTF8(s)
#define DB_STR(s) wxString::FromUTF8(s.c_str())
// wxString -> UTF-8 std::string (wx 3.2/3.3 compatible)
static inline std::string ToStd(const wxString& s) { return std::string(s.utf8_str()); }

#include "patx/claim_diff.hpp"
#include "patx/log.hpp"
#include "patx/timeline_service.hpp"
#include "patx/uspto_sync_service.hpp"
#include "excel_io.hpp"

// ---------------------------------------------------------------------------
// Worker thread: opens its OWN database connection (WAL allows concurrent
// access) and runs one sync job, then posts the result back to the panel.
// ---------------------------------------------------------------------------

namespace {

enum class SyncJob { kAddCase, kSyncCase, kSyncAll, kDownloadDoc, kTestConnection, kBatchAdd };

struct SyncJobSpec {
    SyncJob job = SyncJob::kSyncCase;
    int case_id = 0;
    int document_id = 0;
    int foreign_patent_id = 0;
    std::string app_no;
    std::vector<std::string> identifiers;   // batch import
};

class SyncWorker : public wxThread {
public:
    SyncWorker(wxEvtHandler* sink, const std::string& db_path,
               const patx::UsptoConfig& config, SyncJobSpec spec)
        : wxThread(wxTHREAD_DETACHED), sink_(sink), db_path_(db_path),
          config_(config), spec_(spec) {}

protected:
    ExitCode Entry() override {
        Database db(db_path_);          // thread-private connection
        patx::UsptoSyncService service(db, config_);

        std::string summary;
        bool ok = false;
        switch (spec_.job) {
            case SyncJob::kAddCase: {
                auto r = service.AddCase(spec_.app_no, spec_.foreign_patent_id);
                ok = r.ok;
                if (r.api_key_missing) summary = r.error;
                else if (r.case_not_found) summary = "Application not found: " + r.error;
                else if (r.ok)
                    summary = "Case " + spec_.app_no +
                              (r.resolved_application_number.empty() ||
                                       r.resolved_application_number == spec_.app_no
                                   ? ""
                                   : " (resolved to application " + r.resolved_application_number + ")") +
                              " synced: " +
                              std::to_string(r.documents_total) + " documents (" +
                              std::to_string(r.documents_new) + " new), " +
                              std::to_string(r.claim_versions_built) + " claim versions";
                else summary = "Sync failed: " + r.error + (r.http_status_line.empty() ? "" : " [" + r.http_status_line + "]");
                break;
            }
            case SyncJob::kSyncCase: {
                auto r = service.SyncCase(spec_.case_id);
                ok = r.ok;
                summary = r.ok ? "Sync complete: " + std::to_string(r.documents_new) + " new document(s)"
                               : "Sync failed: " + r.error;
                break;
            }
            case SyncJob::kSyncAll: {
                auto results = service.SyncAllCases();
                int new_docs = 0, failures = 0;
                for (const auto& r : results) {
                    new_docs += r.documents_new;
                    if (!r.ok) failures++;
                }
                ok = failures == 0;
                summary = "Synced " + std::to_string(results.size()) + " case(s), " +
                          std::to_string(new_docs) + " new document(s)" +
                          (failures ? ", " + std::to_string(failures) + " failure(s)" : "");
                break;
            }
            case SyncJob::kDownloadDoc: {
                auto r = service.DownloadAndParseDocument(spec_.document_id);
                ok = r.ok;
                summary = r.ok ? "Document downloaded and parsed" : "Download failed: " + r.error;
                break;
            }
            case SyncJob::kTestConnection: {
                auto r = service.client().TestConnection();
                ok = r.ok;
                summary = r.ok ? "Connected to USPTO Open Data Portal"
                               : "Connection failed: " + r.error;
                break;
            }
            case SyncJob::kBatchAdd: {
                // Dedup against existing cases by every normalized number form
                patx::UsptoRepository repo(db.GetHandle());
                std::set<std::string> known;
                for (const auto& c : repo.GetAllCases()) {
                    known.insert(patx::UsptoClient::NormalizeApplicationNumber(c.application_number));
                    if (!c.publication_number.empty())
                        known.insert(patx::UsptoClient::NormalizePublicationNumber(c.publication_number));
                    if (!c.patent_number.empty())
                        known.insert(patx::UsptoClient::NormalizePatentNumber(c.patent_number));
                }

                int added = 0, skipped = 0, failed = 0;
                std::vector<std::string> failure_notes;
                for (const auto& id : spec_.identifiers) {
                    std::string norm_app = patx::UsptoClient::NormalizeApplicationNumber(id);
                    if (known.count(norm_app)) {
                        skipped++;
                        continue;
                    }
                    auto r = service.AddCase(id);
                    if (r.ok) {
                        added++;
                        known.insert(r.resolved_application_number.empty()
                                         ? norm_app
                                         : patx::UsptoClient::NormalizeApplicationNumber(
                                               r.resolved_application_number));
                    } else {
                        failed++;
                        if (failure_notes.size() < 8) {
                            failure_notes.push_back(id + ": " + (r.error.empty() ? "unknown error" : r.error));
                        }
                    }
                }
                ok = failed == 0;
                summary = "Batch import: " + std::to_string(added) + " added, " +
                          std::to_string(skipped) + " already tracked, " +
                          std::to_string(failed) + " failed";
                for (const auto& note : failure_notes) summary += "\n  " + note;
                break;
            }
        }

        wxThreadEvent* event = new wxThreadEvent(wxEVT_THREAD, kThreadDoneId);
        event->SetPayload(summary);
        event->SetInt(ok ? 1 : 0);
        wxQueueEvent(sink_, event);
        return static_cast<ExitCode>(0);
    }

public:
    static const int kThreadDoneId = wxID_HIGHEST + 7501;

private:
    wxEvtHandler* sink_;
    std::string db_path_;
    patx::UsptoConfig config_;
    SyncJobSpec spec_;
};

} // namespace

// ---------------------------------------------------------------------------
// USPTO settings dialog
// ---------------------------------------------------------------------------

class UsptoSettingsDialog : public wxDialog {
public:
    UsptoSettingsDialog(wxWindow* parent, const std::string& db_path)
        : wxDialog(parent, wxID_ANY, UTF8_STR("USPTO 设置 / USPTO Settings"),
                   wxDefaultPosition, wxSize(560, 520)),
          db_path_(db_path) {
        // Load persisted settings (API key lives in the local, git-ignored
        // database - never in source control; USPTO_API_KEY env also works).
        Database db(db_path_);
        base_url_value_ = db.GetConfig("uspto_api_base_url");
        if (base_url_value_.empty()) base_url_value_ = "https://api.uspto.gov";
        api_key_value_ = db.GetConfig("uspto_api_key");
        timeout_ = std::atoi(db.GetConfig("uspto_timeout").c_str());
        if (timeout_ <= 0) timeout_ = 30;
        retries_ = std::atoi(db.GetConfig("uspto_max_retries").c_str());
        if (retries_ <= 0) retries_ = 3;
        auto_sync_value_ = db.GetConfig("uspto_auto_sync_interval");
        if (auto_sync_value_.empty()) auto_sync_value_ = "manual";
        auto_dl_oa_ = db.GetConfig("uspto_auto_download_oa") != "0";
        auto_dl_amend_ = db.GetConfig("uspto_auto_download_amendments") == "1";
        doc_folder_value_ = db.GetConfig("uspto_document_folder");
        if (doc_folder_value_.empty()) doc_folder_value_ = "data/documents/uspto";

        BuildUI();
    }

    patx::UsptoConfig Config() const {
        patx::UsptoConfig config;
        config.api_base_url = ToStd(base_url_->GetValue());
        config.api_key = ToStd(api_key_->GetValue());
        config.timeout_seconds = timeout_spin_->GetValue();
        config.max_retries = retries_spin_->GetValue();
        config.auto_sync_interval = ToStd(interval_choice_->GetStringSelection());
        config.auto_download_oa = oa_check_->GetValue();
        config.auto_download_amendments = amend_check_->GetValue();
        config.document_folder = ToStd(folder_field_->GetValue());
        return config;
    }

private:
    void BuildUI() {
        wxBoxSizer* main = new wxBoxSizer(wxVERTICAL);
        wxScrolledWindow* panel = new wxScrolledWindow(this);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        auto add_row = [&](const wxString& label, wxWindow* control) {
            wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(panel, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            sizer->Add(row, 0, wxEXPAND | wxALL, 4);
            row->Add(control, 1, wxEXPAND);
        };

        base_url_ = new wxTextCtrl(panel, wxID_ANY, base_url_value_);
        add_row("API Base URL:", base_url_);

        api_key_ = new wxTextCtrl(panel, wxID_ANY, api_key_value_, wxDefaultPosition,
                                  wxDefaultSize, wxTE_PASSWORD);
        add_row(UTF8_STR("API Key (或设置环境变量 USPTO_API_KEY):"), api_key_);

        timeout_spin_ = new wxSpinCtrl(panel, wxID_ANY, std::to_string(timeout_), wxDefaultPosition,
                                       wxDefaultSize, wxSP_ARROW_KEYS, 5, 300, timeout_);
        add_row("Request Timeout (s):", timeout_spin_);

        retries_spin_ = new wxSpinCtrl(panel, wxID_ANY, std::to_string(retries_), wxDefaultPosition,
                                       wxDefaultSize, wxSP_ARROW_KEYS, 1, 6, retries_);
        add_row("Max Retries:", retries_spin_);

        interval_choice_ = new wxChoice(panel, wxID_ANY);
        interval_choice_->Append("manual");
        interval_choice_->Append("12h");
        interval_choice_->Append("24h");
        int sel = interval_choice_->FindString(auto_sync_value_);
        interval_choice_->SetSelection(sel == wxNOT_FOUND ? 0 : sel);
        add_row("Auto Sync:", interval_choice_);

        oa_check_ = new wxCheckBox(panel, wxID_ANY,
                                   UTF8_STR("自动下载 Office Action 与审查文件"));
        oa_check_->SetValue(auto_dl_oa_);
        sizer->Add(oa_check_, 0, wxALL, 4);
        amend_check_ = new wxCheckBox(panel, wxID_ANY,
                                      UTF8_STR("自动下载申请人 Amendment/答复"));
        amend_check_->SetValue(auto_dl_amend_);
        sizer->Add(amend_check_, 0, wxALL, 4);

        folder_field_ = new wxTextCtrl(panel, wxID_ANY, doc_folder_value_);
        add_row(UTF8_STR("文档目录 (相对数据库):"), folder_field_);

        auto* hint = new wxStaticText(
            panel, wxID_ANY,
            UTF8_STR("API Key 只保存在本地数据库（不入 Git、不写日志）。\n"
                     "也可使用环境变量 USPTO_API_KEY，优先级低于此处配置。\n"
                     "Key 需在 USPTO Open Data Portal (data.uspto.gov) 申请。"));
        hint->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_ITALIC, wxFONTWEIGHT_NORMAL));
        sizer->Add(hint, 0, wxALL, 8);

        test_btn_ = new wxButton(panel, wxID_ANY, UTF8_STR("测试连接 / Test Connection"));
        test_btn_->Bind(wxEVT_BUTTON, &UsptoSettingsDialog::OnTestConnection, this);
        sizer->Add(test_btn_, 0, wxALL, 4);
        test_result_ = new wxStaticText(panel, wxID_ANY, "");
        sizer->Add(test_result_, 0, wxALL, 4);

        panel->SetSizer(sizer);
        panel->SetScrollRate(5, 5);
        main->Add(panel, 1, wxEXPAND | wxALL, 10);

        wxBoxSizer* btns = new wxBoxSizer(wxHORIZONTAL);
        btns->AddStretchSpacer();
        btns->Add(new wxButton(this, wxID_OK, "Save"), 0, wxRIGHT, 8);
        btns->Add(new wxButton(this, wxID_CANCEL, "Cancel"), 0);
        main->Add(btns, 0, wxEXPAND | wxALL, 10);
        SetSizer(main);
        Centre();

        Bind(wxEVT_BUTTON, &UsptoSettingsDialog::OnSave, this, wxID_OK);
        Bind(wxEVT_THREAD, &UsptoSettingsDialog::OnTestDone, this, SyncWorker::kThreadDoneId);
    }

    void OnTestConnection(wxCommandEvent&) {
        test_result_->SetLabel(UTF8_STR("正在连接 USPTO... (后台执行)"));
        test_btn_->Enable(false);

        patx::UsptoConfig config = Config();
        if (config.api_key.empty()) {
            const char* env = std::getenv("USPTO_API_KEY");
            if (env) config.api_key = env;
        }
        SyncJobSpec spec;
        spec.job = SyncJob::kTestConnection;
        SyncWorker* worker = new SyncWorker(GetEventHandler(), db_path_, config, spec);
        if (worker->Create() != wxTHREAD_NO_ERROR) {
            test_result_->SetLabel("failed to start worker thread");
            test_btn_->Enable(true);
            delete worker;
        } else {
            worker->Run();
        }
    }

    void OnTestDone(wxThreadEvent& event) {
        test_btn_->Enable(true);
        bool ok = event.GetInt() == 1;
        std::string summary = event.GetPayload<std::string>();
        test_result_->SetLabel((ok ? wxString("✅ ") : wxString("❐ ")) +
                               wxString::FromUTF8(summary.c_str()));
    }

    void OnSave(wxCommandEvent&) {
        // Persist to the local DB config table (git-ignored file)
        Database db(db_path_);
        auto set = [&](const char* key, const std::string& value) { db.SetConfig(key, value); };
        set("uspto_api_base_url", ToStd(base_url_->GetValue()));
        set("uspto_api_key", ToStd(api_key_->GetValue()));
        set("uspto_timeout", std::to_string(timeout_spin_->GetValue()));
        set("uspto_max_retries", std::to_string(retries_spin_->GetValue()));
        set("uspto_auto_sync_interval", ToStd(interval_choice_->GetStringSelection()));
        set("uspto_auto_download_oa", oa_check_->GetValue() ? "1" : "0");
        set("uspto_auto_download_amendments", amend_check_->GetValue() ? "1" : "0");
        set("uspto_document_folder", ToStd(folder_field_->GetValue()));
        if (!api_key_->GetValue().empty()) {
            PATX_LOG_INFO("USPTO API key updated (stored locally): " +
                          patx::MaskSecret(ToStd(api_key_->GetValue())));
        }
        EndModal(wxID_OK);
    }

    std::string db_path_;
    std::string base_url_value_, api_key_value_, auto_sync_value_, doc_folder_value_;
    int timeout_ = 30, retries_ = 3;
    bool auto_dl_oa_ = true, auto_dl_amend_ = false;

    wxTextCtrl* base_url_ = nullptr;
    wxTextCtrl* api_key_ = nullptr;
    wxSpinCtrl* timeout_spin_ = nullptr;
    wxSpinCtrl* retries_spin_ = nullptr;
    wxChoice* interval_choice_ = nullptr;
    wxCheckBox* oa_check_ = nullptr;
    wxCheckBox* amend_check_ = nullptr;
    wxTextCtrl* folder_field_ = nullptr;
    wxButton* test_btn_ = nullptr;
    wxStaticText* test_result_ = nullptr;
};

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------

UsProsecutionPanel::UsProsecutionPanel(wxWindow* parent, const std::string& db_path)
    : wxPanel(parent), db_path_(db_path) {
    Bind(wxEVT_THREAD, &UsProsecutionPanel::OnWorkerDone, this, SyncWorker::kThreadDoneId);
    db_ = std::make_unique<Database>(db_path);
    repo_ = new patx::UsptoRepository(db_->GetHandle());
    repo_->EnsureTables();

    wxBoxSizer* top = new wxBoxSizer(wxVERTICAL);
    BuildToolbar(top);

    wxSplitterWindow* splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition,
                                                      wxDefaultSize, wxSP_LIVE_UPDATE);
    BuildCaseList(splitter);
    BuildDetailBook(splitter);
    splitter->SplitVertically(case_list_, detail_book_, 620);
    top->Add(splitter, 1, wxEXPAND | wxALL, 4);
    SetSizer(top);

    RefreshCases();
}

patx::UsptoConfig UsProsecutionPanel::CurrentConfig() const {
    patx::UsptoConfig config;
    config.api_base_url = db_->GetConfig("uspto_api_base_url");
    if (config.api_base_url.empty()) config.api_base_url = "https://api.uspto.gov";
    config.api_key = db_->GetConfig("uspto_api_key");
    config.timeout_seconds = std::max(1, std::atoi(db_->GetConfig("uspto_timeout").c_str()));
    config.max_retries = std::max(1, std::atoi(db_->GetConfig("uspto_max_retries").c_str()));
    config.auto_download_oa = db_->GetConfig("uspto_auto_download_oa") != "0";
    config.auto_download_amendments = db_->GetConfig("uspto_auto_download_amendments") == "1";
    config.document_folder = db_->GetConfig("uspto_document_folder");
    if (config.document_folder.empty()) config.document_folder = "data/documents/uspto";
    return config;
}

void UsProsecutionPanel::BuildToolbar(wxSizer* parent_sizer) {
    wxBoxSizer* tb = new wxBoxSizer(wxHORIZONTAL);

    auto add_btn = [&](const wxString& label, void (UsProsecutionPanel::*handler)(wxCommandEvent&)) {
        wxButton* btn = new wxButton(this, wxID_ANY, label);
        btn->Bind(wxEVT_BUTTON, handler, this);
        tb->Add(btn, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);
        return btn;
    };

    add_btn(UTF8_STR("添加案件 / Add Case"), &UsProsecutionPanel::OnAddCase);
    add_btn(UTF8_STR("批量导入表格 / Batch Import"), &UsProsecutionPanel::OnBatchImport);
    add_btn(UTF8_STR("同步选中 / Sync"), &UsProsecutionPanel::OnSyncSelected);
    add_btn(UTF8_STR("同步全部 / Sync All"), &UsProsecutionPanel::OnSyncAll);
    add_btn(UTF8_STR("下载全部文档 / Download All"), &UsProsecutionPanel::OnDownloadAll);
    add_btn(UTF8_STR("设置 / Settings"), &UsProsecutionPanel::OnSettings);

    parent_sizer->Add(tb, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);
}

void UsProsecutionPanel::BuildCaseList(wxSplitterWindow* splitter) {
    case_list_ = new wxListCtrl(splitter, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
    case_list_->AppendColumn("App No", wxLIST_FORMAT_LEFT, 90);
    case_list_->AppendColumn(UTF8_STR("内部案号"), wxLIST_FORMAT_LEFT, 90);
    case_list_->AppendColumn("Patent No", wxLIST_FORMAT_LEFT, 90);
    case_list_->AppendColumn(UTF8_STR("标题"), wxLIST_FORMAT_LEFT, 210);
    case_list_->AppendColumn(UTF8_STR("状态"), wxLIST_FORMAT_LEFT, 110);
    case_list_->AppendColumn(UTF8_STR("最新事件"), wxLIST_FORMAT_LEFT, 150);
    case_list_->AppendColumn("Last Sync", wxLIST_FORMAT_LEFT, 140);
    case_list_->AppendColumn(UTF8_STR("新文档"), wxLIST_FORMAT_LEFT, 60);
    case_list_->AppendColumn("Sync", wxLIST_FORMAT_LEFT, 70);

    case_list_->Bind(wxEVT_LIST_ITEM_SELECTED,
                     [this](wxListEvent& e) {
                         selected_case_id_ = static_cast<int>(case_list_->GetItemData(e.GetIndex()));
                         repo_->MarkDocumentsSeen(selected_case_id_);
                         LoadCaseDetail();
                         e.Skip();
                     });
    case_list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, &UsProsecutionPanel::OnSyncSelected, this);
}

void UsProsecutionPanel::BuildDetailBook(wxSplitterWindow* splitter) {
    detail_book_ = new wxNotebook(splitter, wxID_ANY);

    // Overview
    {
        wxPanel* page = new wxPanel(detail_book_);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        overview_text_ = new wxTextCtrl(page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                        wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
        overview_text_->SetFont(wxFont(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
        sizer->Add(overview_text_, 1, wxEXPAND);
        page->SetSizer(sizer);
        detail_book_->AddPage(page, UTF8_STR("概览 Overview"));
    }
    // Timeline
    {
        wxPanel* page = new wxPanel(detail_book_);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        wxBoxSizer* tb = new wxBoxSizer(wxHORIZONTAL);
        timeline_dir_btn_ = new wxButton(page, wxID_ANY, UTF8_STR("正序 / Ascending"));
        timeline_dir_btn_->Bind(wxEVT_BUTTON, &UsProsecutionPanel::OnTimelineDirection, this);
        tb->Add(timeline_dir_btn_, 0, wxALL, 3);
        tb->AddStretchSpacer();
        sizer->Add(tb, 0, wxEXPAND);

        timeline_list_ = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        timeline_list_->AppendColumn(UTF8_STR("日期"), wxLIST_FORMAT_LEFT, 90);
        timeline_list_->AppendColumn(UTF8_STR("类别"), wxLIST_FORMAT_LEFT, 170);
        timeline_list_->AppendColumn(UTF8_STR("描述"), wxLIST_FORMAT_LEFT, 260);
        timeline_list_->AppendColumn(UTF8_STR("提交方"), wxLIST_FORMAT_LEFT, 80);
        timeline_list_->AppendColumn("OA", wxLIST_FORMAT_LEFT, 70);
        timeline_list_->AppendColumn(UTF8_STR("涉及权项"), wxLIST_FORMAT_LEFT, 90);
        timeline_list_->AppendColumn(UTF8_STR("驳回依据"), wxLIST_FORMAT_LEFT, 220);
        timeline_list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, &UsProsecutionPanel::OnOpenDocument, this);
        sizer->Add(timeline_list_, 1, wxEXPAND | wxALL, 3);
        page->SetSizer(sizer);
        detail_book_->AddPage(page, UTF8_STR("审查时间线 Timeline"));
    }
    // Claims + diff viewer
    {
        wxPanel* page = new wxPanel(detail_book_);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        wxBoxSizer* tb = new wxBoxSizer(wxHORIZONTAL);
        tb->Add(new wxStaticText(page, wxID_ANY, UTF8_STR("版本对比:")), 0,
                wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        version_a_choice_ = new wxChoice(page, wxID_ANY, wxDefaultPosition, wxSize(170, -1));
        version_b_choice_ = new wxChoice(page, wxID_ANY, wxDefaultPosition, wxSize(170, -1));
        tb->Add(version_a_choice_, 0, wxRIGHT, 8);
        tb->Add(new wxStaticText(page, wxID_ANY, "vs"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        tb->Add(version_b_choice_, 0, wxRIGHT, 8);
        wxButton* compare_btn = new wxButton(page, wxID_ANY, UTF8_STR("比较 / Compare"));
        compare_btn->Bind(wxEVT_BUTTON, &UsProsecutionPanel::OnCompareVersions, this);
        tb->Add(compare_btn, 0, wxRIGHT, 8);
        tb->AddStretchSpacer();
        claims_summary_ = new wxStaticText(page, wxID_ANY, "");
        tb->Add(claims_summary_, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(tb, 0, wxEXPAND | wxALL, 3);

        claim_diff_text_ = new wxTextCtrl(page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                          wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_NO_VSCROLL);
        claim_diff_text_->SetFont(wxFont(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
        sizer->Add(claim_diff_text_, 1, wxEXPAND | wxALL, 3);
        page->SetSizer(sizer);
        detail_book_->AddPage(page, UTF8_STR("权利要求 Claims"));
    }
    // Office Actions
    {
        wxPanel* page = new wxPanel(detail_book_);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        oa_doc_list_ = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        oa_doc_list_->AppendColumn(UTF8_STR("日期"), wxLIST_FORMAT_LEFT, 90);
        oa_doc_list_->AppendColumn(UTF8_STR("类型"), wxLIST_FORMAT_LEFT, 150);
        oa_doc_list_->AppendColumn(UTF8_STR("驳回依据"), wxLIST_FORMAT_LEFT, 130);
        oa_doc_list_->AppendColumn(UTF8_STR("涉及权项"), wxLIST_FORMAT_LEFT, 110);
        oa_doc_list_->AppendColumn(UTF8_STR("主要对比文件"), wxLIST_FORMAT_LEFT, 170);
        oa_doc_list_->AppendColumn(UTF8_STR("解析置信度"), wxLIST_FORMAT_LEFT, 90);
        oa_doc_list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, &UsProsecutionPanel::OnOpenDocument, this);
        sizer->Add(oa_doc_list_, 1, wxEXPAND | wxALL, 3);
        page->SetSizer(sizer);
        detail_book_->AddPage(page, UTF8_STR("Office Actions"));
    }
    // Documents
    {
        wxPanel* page = new wxPanel(detail_book_);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        wxBoxSizer* tb = new wxBoxSizer(wxHORIZONTAL);
        auto add_btn = [&](const wxString& label, void (UsProsecutionPanel::*h)(wxCommandEvent&)) {
            wxButton* btn = new wxButton(page, wxID_ANY, label);
            btn->Bind(wxEVT_BUTTON, h, this);
            tb->Add(btn, 0, wxRIGHT, 4);
        };
        add_btn(UTF8_STR("下载 / Download"), &UsProsecutionPanel::OnDownloadDocument);
        add_btn(UTF8_STR("下载全部 / Download All"), &UsProsecutionPanel::OnDownloadAll);
        add_btn(UTF8_STR("打开原文 / Open"), &UsProsecutionPanel::OnOpenDocument);
        add_btn(UTF8_STR("重新解析 / Reparse"), &UsProsecutionPanel::OnReparseDocument);
        sizer->Add(tb, 0, wxEXPAND | wxALL, 3);

        doc_list_ = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        doc_list_->AppendColumn(UTF8_STR("日期"), wxLIST_FORMAT_LEFT, 85);
        doc_list_->AppendColumn("Code", wxLIST_FORMAT_LEFT, 70);
        doc_list_->AppendColumn(UTF8_STR("描述"), wxLIST_FORMAT_LEFT, 240);
        doc_list_->AppendColumn(UTF8_STR("类别"), wxLIST_FORMAT_LEFT, 160);
        doc_list_->AppendColumn(UTF8_STR("提交方"), wxLIST_FORMAT_LEFT, 80);
        doc_list_->AppendColumn(UTF8_STR("页数"), wxLIST_FORMAT_LEFT, 50);
        doc_list_->AppendColumn(UTF8_STR("已下载"), wxLIST_FORMAT_LEFT, 70);
        doc_list_->AppendColumn(UTF8_STR("解析"), wxLIST_FORMAT_LEFT, 90);
        doc_list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, &UsProsecutionPanel::OnOpenDocument, this);
        sizer->Add(doc_list_, 1, wxEXPAND | wxALL, 3);
        page->SetSizer(sizer);
        detail_book_->AddPage(page, UTF8_STR("文档 Documents"));
    }
    // Sync log
    {
        wxPanel* page = new wxPanel(detail_book_);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        sync_log_list_ = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        sync_log_list_->AppendColumn(UTF8_STR("时间"), wxLIST_FORMAT_LEFT, 160);
        sync_log_list_->AppendColumn(UTF8_STR("事件"), wxLIST_FORMAT_LEFT, 140);
        sync_log_list_->AppendColumn(UTF8_STR("详情"), wxLIST_FORMAT_LEFT, 560);
        sizer->Add(sync_log_list_, 1, wxEXPAND | wxALL, 3);
        page->SetSizer(sizer);
        detail_book_->AddPage(page, UTF8_STR("同步日志 Sync Log"));
    }
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void UsProsecutionPanel::RefreshCases() {
    case_list_->DeleteAllItems();
    auto cases = repo_->GetAllCases();
    int row = 0;
    for (const auto& c : cases) {
        long idx = case_list_->InsertItem(row, DB_STR(c.application_number));
        case_list_->SetItem(idx, 1, DB_STR(c.internal_case_no));
        case_list_->SetItem(idx, 2, DB_STR(c.patent_number));
        case_list_->SetItem(idx, 3, DB_STR(c.title));
        case_list_->SetItem(idx, 4, DB_STR(c.application_status));
        // Latest event: most recent document description
        auto docs = repo_->GetDocumentsForCaseOrdered(c.id, false);
        if (!docs.empty()) {
            case_list_->SetItem(idx, 5, DB_STR(docs.front().document_description));
        }
        case_list_->SetItem(idx, 6, DB_STR(c.last_synced_at));
        case_list_->SetItem(idx, 7, std::to_string(c.new_document_count));
        case_list_->SetItem(idx, 8, DB_STR(c.sync_status));
        case_list_->SetItemData(idx, c.id);
        if (c.new_document_count > 0) {
            case_list_->SetItemBackgroundColour(idx, wxColour(255, 240, 200));
        }
        row++;
    }
}

void UsProsecutionPanel::LoadCaseDetail() {
    if (selected_case_id_ <= 0) return;
    LoadOverview();
    LoadTimeline();
    LoadClaims();
    LoadOaList();
    LoadDocuments();
    LoadSyncLog();
}

void UsProsecutionPanel::LoadOverview() {
    auto c = repo_->GetCaseById(selected_case_id_);
    auto versions = repo_->GetClaimVersions(selected_case_id_);

    int total_claims = 0, independent_claims = 0;
    if (!versions.empty()) {
        auto claims = repo_->GetClaims(versions.back().id);
        total_claims = static_cast<int>(claims.size());
        for (const auto& claim : claims) {
            if (claim.is_independent && claim.status != patx::ClaimStatus::Canceled) independent_claims++;
        }
    }

    // Latest OA + suggested deadline
    std::string latest_oa, deadline, deadline_source;
    QueryFilter f;
    auto oa_records = db_->GetOARecords(f);
    for (const auto& oa : oa_records) {
        if (oa.external_case_id == std::to_string(selected_case_id_) &&
            oa.jurisdiction == "US" && !oa.is_completed) {
            latest_oa = oa.oa_type + " @ " + oa.issue_date;
            deadline = oa.official_deadline;
            deadline_source = oa.deadline_source;
            break;
        }
    }

    std::ostringstream out;
    out << "Application No : " << c.application_number
        << (c.patent_number.empty() ? "" : "   (Patent No " + c.patent_number + ")") << "\n"
        << "Title          : " << c.title << "\n"
        << "Internal Case  : " << (c.internal_case_no.empty() ? "-" : c.internal_case_no)
        << "   ForeignPatentID: " << (c.foreign_patent_id ? std::to_string(c.foreign_patent_id) : "-") << "\n"
        << "Status         : " << c.application_status << "  (" << c.status_date << ")\n"
        << "Filing         : " << c.filing_date
        << "   Priority: " << c.priority_date
        << "   Grant: " << (c.grant_date.empty() ? "-" : c.grant_date) << "\n"
        << "Publication    : " << (c.publication_number.empty() ? "-" : c.publication_number) << "\n"
        << "Inventor       : " << c.first_named_inventor << "\n"
        << "Applicant      : " << c.applicant_name
        << "   Assignee: " << (c.assignee_name.empty() ? "-" : c.assignee_name) << "\n"
        << "Examiner       : " << c.examiner_name
        << "   Art Unit: " << c.art_unit
        << "   TC: " << c.technology_center << "\n"
        << "Docket         : " << c.attorney_docket_number
        << "   Entity: " << c.entity_status << "\n"
        << "Claims         : " << total_claims << " total, " << independent_claims << " independent"
        << "   Versions: " << versions.size() << "\n"
        << "Latest OA      : " << (latest_oa.empty() ? "-" : latest_oa) << "\n"
        << "Resp. deadline : " << (deadline.empty() ? "-" : deadline)
        << (deadline_source == "calculated" ? "  [Calculated - needs confirmation]"
                                            : (deadline_source == "manual" ? "  [Manual]" : ""))
        << "\n";

    // Official Office Action dataset summary (oa_actions / oa_rejections)
    auto facts = repo_->GetCaseFacts(selected_case_id_);
    for (const auto& [key, value] : facts) {
        if (key == "official_oa_summary" && !value.empty()) {
            out << "Official OA    : " << value << "\n";
        }
    }

    out << "Last sync      : " << c.last_synced_at
        << "   Status: " << c.sync_status
        << (c.sync_error.empty() ? "" : "   Error: " + c.sync_error) << "\n";
    overview_text_->SetValue(wxString::FromUTF8(out.str().c_str()));
}

void UsProsecutionPanel::LoadTimeline() {
    timeline_list_->DeleteAllItems();
    if (selected_case_id_ <= 0) return;
    patx::TimelineService timeline(*repo_);
    auto events = timeline.BuildTimeline(selected_case_id_, timeline_ascending_);
    int row = 0;
    for (const auto& e : events) {
        long idx = timeline_list_->InsertItem(row, DB_STR(e.date));
        timeline_list_->SetItem(idx, 1, DB_STR(e.event_category));
        timeline_list_->SetItem(idx, 2, DB_STR(e.document_description));
        timeline_list_->SetItem(idx, 3, patx::ToString(e.party));
        timeline_list_->SetItem(idx, 4, DB_STR(e.oa_type));
        timeline_list_->SetItem(idx, 5, DB_STR(e.claims_affected));
        timeline_list_->SetItem(idx, 6, DB_STR(e.rejection_summary));
        timeline_list_->SetItemData(idx, e.document_id);
        if (e.party == patx::FilingParty::Examiner) {
            timeline_list_->SetItemBackgroundColour(row, wxColour(232, 240, 254));
        } else if (e.party == patx::FilingParty::Applicant) {
            timeline_list_->SetItemBackgroundColour(row, wxColour(235, 250, 235));
        }
        row++;
    }
}

void UsProsecutionPanel::LoadClaims() {
    version_a_choice_->Clear();
    version_b_choice_->Clear();
    claim_diff_text_->Clear();
    auto versions = repo_->GetClaimVersions(selected_case_id_);
    for (const auto& v : versions) {
        wxString label = wxString::Format("v%d %s%s (%s)",
            v.version_no,
            v.is_initial ? "Original" : (v.is_current ? "Current" : ""),
            v.is_initial || v.is_current ? " " : "",
            v.effective_date.empty() ? "?" : v.effective_date);
        if (!v.version_type.empty() && !v.is_initial && !v.is_current) {
            label += wxString::Format(" [%s]", v.version_type);
        }
        version_a_choice_->Append(label);
        version_b_choice_->Append(label);
    }
    if (versions.size() >= 1) version_b_choice_->SetSelection(versions.size() - 1);
    if (versions.size() >= 2) version_a_choice_->SetSelection(versions.size() - 2);
    else if (!versions.empty()) version_a_choice_->SetSelection(0);

    // Summary line
    int total = 0, indep = 0;
    if (!versions.empty()) {
        for (const auto& claim : repo_->GetClaims(versions.back().id)) {
            if (claim.status == patx::ClaimStatus::Canceled) continue;
            total++;
            if (claim.is_independent) indep++;
        }
    }
    claims_summary_->SetLabel(wxString::Format(UTF8_STR("当前版本: %d 项权利要求（%d 独立）"),
                                               total, indep));
}

void UsProsecutionPanel::LoadOaList() {
    oa_doc_list_->DeleteAllItems();
    auto docs = repo_->GetDocumentsForCaseOrdered(selected_case_id_, false);
    int row = 0;
    for (const auto& doc : docs) {
        bool is_oa = doc.category == patx::UsptoDocumentCategory::NonFinalOfficeAction ||
                     doc.category == patx::UsptoDocumentCategory::FinalOfficeAction ||
                     doc.category == patx::UsptoDocumentCategory::RestrictionRequirement ||
                     doc.category == patx::UsptoDocumentCategory::AdvisoryAction;
        if (!is_oa) continue;
        long idx = oa_doc_list_->InsertItem(row, DB_STR(doc.filing_date));
        oa_doc_list_->SetItem(idx, 1, DB_STR(doc.document_category));

        auto rejections = repo_->GetRejectionsForDocument(doc.id);
        std::string statutes, claims, primary;
        for (const auto& r : rejections) {
            if (!statutes.empty()) statutes += "; ";
            statutes += r.statute;
            if (r.claim_numbers.size() > claims.size()) claims = r.claim_numbers;   // richest set
            if (primary.empty()) primary = r.primary_reference;
        }
        oa_doc_list_->SetItem(idx, 2, DB_STR(statutes));
        oa_doc_list_->SetItem(idx, 3, DB_STR(claims));
        oa_doc_list_->SetItem(idx, 4, DB_STR(primary));
        std::string conf = doc.parse_confidence >= 0.75
                               ? wxString::Format("%.0f%%", doc.parse_confidence * 100).utf8_str()
                               : "Needs Review";
        oa_doc_list_->SetItem(idx, 5, conf);
        oa_doc_list_->SetItemData(idx, doc.id);
        row++;
    }
}

void UsProsecutionPanel::LoadDocuments() {
    doc_list_->DeleteAllItems();
    auto docs = repo_->GetDocumentsForCaseOrdered(selected_case_id_, true);
    int row = 0;
    for (const auto& doc : docs) {
        long idx = doc_list_->InsertItem(row, DB_STR(doc.filing_date));
        doc_list_->SetItem(idx, 1, DB_STR(doc.document_code));
        doc_list_->SetItem(idx, 2, DB_STR(doc.document_description));
        doc_list_->SetItem(idx, 3, DB_STR(doc.document_category));
        doc_list_->SetItem(idx, 4, patx::ToString(doc.party));
        doc_list_->SetItem(idx, 5, doc.page_count ? std::to_string(doc.page_count) : "-");
        doc_list_->SetItem(idx, 6, doc.download_status == "downloaded" ? UTF8_STR("是") : "no");
        std::string parse = doc.parse_status;
        if (parse == "needs_review") parse = "Needs Review";
        if (!doc.extracted_text.empty() && parse == "pending") parse = "text ok";
        doc_list_->SetItem(idx, 7, parse);
        doc_list_->SetItemData(idx, doc.id);
        row++;
    }
}

void UsProsecutionPanel::LoadSyncLog() {
    sync_log_list_->DeleteAllItems();
    auto entries = repo_->GetSyncLog(selected_case_id_, 200);
    int row = 0;
    for (const auto& e : entries) {
        long idx = sync_log_list_->InsertItem(row, DB_STR(e.created_at));
        sync_log_list_->SetItem(idx, 1, DB_STR(e.event));
        sync_log_list_->SetItem(idx, 2, DB_STR(e.detail));
        if (e.is_error) sync_log_list_->SetItemBackgroundColour(row, wxColour(255, 230, 230));
        row++;
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void UsProsecutionPanel::OnAddCase(wxCommandEvent&) {
    wxTextEntryDialog dlg(this,
        UTF8_STR("输入美国申请号 / 公开号 / 专利号\n"
                 "例如 17248024、US 2021/0210819 A1 或 11,646,472\n\n"
                 "公开号/专利号会通过 USPTO 官方搜索端点解析为申请号；\n"
                 "命中多条时会要求改用申请号确认，不会自动猜测。"),
        UTF8_STR("添加 USPTO 案件"));
    if (dlg.ShowModal() != wxID_OK) return;
    std::string app_no = ToStd(dlg.GetValue());
    if (app_no.empty()) return;

    // Auto-link only an UNAMBIGUOUS match by normalized application number.
    // Ambiguous or missing matches stay unlinked - the user links them
    // explicitly; never merge on title similarity.
    int foreign_id = db_->FindUSCaseCandidate(app_no);

    StartSyncWorker(0, app_no, foreign_id);
}

void UsProsecutionPanel::OnBatchImport(wxCommandEvent&) {
    wxFileDialog dlg(this, UTF8_STR("选择含美国案号的表格 / Select spreadsheet"),
                     "", "", UTF8_STR("表格文件|*.xlsx;*.csv"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    std::string path = ToStd(dlg.GetPath());

    std::vector<std::string> ids;
    int skipped_non_us = 0;
    std::string error;
    if (!ExcelIO::ReadIdentifiers(path, ids, &skipped_non_us, error)) {
        wxMessageBox(wxString::FromUTF8(error.c_str()), UTF8_STR("批量导入"),
                     wxOK | wxICON_ERROR);
        return;
    }
    if (ids.empty()) {
        wxMessageBox(UTF8_STR("表格中没有识别到美国申请号/公开号/专利号。\n"
                              "(CN 等非美国号码无法在 USPTO 查询，会被跳过)"),
                     UTF8_STR("批量导入"), wxOK | wxICON_INFORMATION);
        return;
    }

    wxString msg = wxString::Format(
        UTF8_STR("从表格识别到 %zu 个美国案号%s。\n"
                 "将依次解析并同步每个案件（含审查意见下载），可能需要几分钟。继续？"),
        ids.size(),
        skipped_non_us > 0
            ? wxString::Format(UTF8_STR("（另有 %d 个非美国号码已跳过）"), skipped_non_us).utf8_str()
            : "");
    if (wxMessageBox(msg, UTF8_STR("批量导入 / Batch Import"),
                     wxYES_NO | wxICON_QUESTION) != wxYES)
        return;

    StartBatchWorker(ids);
}

void UsProsecutionPanel::OnSyncSelected(wxCommandEvent&) {    if (selected_case_id_ <= 0) {
        wxMessageBox(UTF8_STR("请先选择一个案件"), UTF8_STR("提示"), wxOK | wxICON_INFORMATION);
        return;
    }
    StartSyncWorker(selected_case_id_);
}

void UsProsecutionPanel::OnSyncAll(wxCommandEvent&) {
    StartSyncWorker(0);
}

void UsProsecutionPanel::OnSettings(wxCommandEvent&) {
    UsptoSettingsDialog dlg(this, db_path_);
    dlg.ShowModal();
}

void UsProsecutionPanel::OnDownloadAll(wxCommandEvent&) {
    if (selected_case_id_ <= 0) return;
    // Queue one download job per undownloaded document (workers run serially
    // enough in practice; each job posts back when done)
    auto docs = repo_->GetDocumentsForCase(selected_case_id_);
    int queued = 0;
    for (const auto& doc : docs) {
        if (doc.download_status != "downloaded" && !doc.file_download_uri.empty()) {
            SyncJobSpec spec;
            spec.job = SyncJob::kDownloadDoc;
            spec.document_id = doc.id;
            SyncWorker* worker = new SyncWorker(GetEventHandler(), db_path_, CurrentConfig(), spec);
            if (worker->Create() == wxTHREAD_NO_ERROR) {
                worker->Run();
                active_workers_++;
                queued++;
            }
        }
    }
    if (queued == 0) {
        wxMessageBox(UTF8_STR("所有文档均已下载（或无下载地址）"), UTF8_STR("提示"), wxOK);
    }
}

void UsProsecutionPanel::OnDownloadDocument(wxCommandEvent&) {
    long idx = doc_list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (idx < 0) return;
    int doc_id = static_cast<int>(doc_list_->GetItemData(idx));

    SyncJobSpec spec;
    spec.job = SyncJob::kDownloadDoc;
    spec.document_id = doc_id;
    SyncWorker* worker = new SyncWorker(GetEventHandler(), db_path_, CurrentConfig(), spec);
    if (worker->Create() == wxTHREAD_NO_ERROR) {
        worker->Run();
        active_workers_++;
    }
}

void UsProsecutionPanel::OnOpenDocument(wxCommandEvent&) {
    wxListCtrl* list = dynamic_cast<wxListCtrl*>(FindFocus());
    if (!list) list = doc_list_;
    long idx = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (idx < 0) return;
    int doc_id = static_cast<int>(list->GetItemData(idx));

    auto doc = repo_->GetDocumentById(doc_id);
    if (doc.download_status != "downloaded" || doc.local_file_path.empty()) {
        if (wxMessageBox(UTF8_STR("该文档尚未下载，现在下载吗？"), UTF8_STR("下载"),
                     wxYES_NO | wxICON_QUESTION) == wxYES) {
            wxCommandEvent download_evt;
            OnDownloadDocument(download_evt);
        }
        return;
    }
    std::string folder = CurrentConfig().document_folder;
    std::string abs = (std::filesystem::path(folder) / doc.local_file_path).string();
    if (!std::filesystem::exists(abs)) {
        wxMessageBox(UTF8_STR("本地文件不存在（目录被移动？）:\n") + abs, UTF8_STR("错误"), wxOK | wxICON_ERROR);
        return;
    }
    wxLaunchDefaultApplication(abs);
}

void UsProsecutionPanel::OnReparseDocument(wxCommandEvent&) {
    // Reparse = re-download text extraction + rebuild claim history for the case
    wxCommandEvent download_evt;
    OnDownloadDocument(download_evt);
}

void UsProsecutionPanel::OnTimelineDirection(wxCommandEvent&) {
    timeline_ascending_ = !timeline_ascending_;
    timeline_dir_btn_->SetLabel(timeline_ascending_ ? UTF8_STR("正序 / Ascending")
                                                    : UTF8_STR("倒序 / Descending"));
    LoadTimeline();
}

void UsProsecutionPanel::OnCompareVersions(wxCommandEvent&) {
    int a = version_a_choice_->GetSelection();
    int b = version_b_choice_->GetSelection();
    auto versions = repo_->GetClaimVersions(selected_case_id_);
    if (a == wxNOT_FOUND || b == wxNOT_FOUND || a >= (int)versions.size() || b >= (int)versions.size()) {
        return;
    }
    ShowClaimDiff(versions[a].id, versions[b].id);
}

void UsProsecutionPanel::ShowClaimDiff(int version_a, int version_b) {
    claim_diff_text_->Clear();
    auto claims_a = repo_->GetClaims(version_a);
    auto claims_b = repo_->GetClaims(version_b);
    auto diffs = patx::ClaimDiffEngine::DiffClaimSets(claims_a, claims_b);
    AppendDiffText(claim_diff_text_, diffs);
}

void UsProsecutionPanel::AppendDiffText(wxTextCtrl* out, const std::vector<patx::ClaimDiff>& diffs) {
    *out << wxString::Format(UTF8_STR("== 权利要求对比（%zu 项） ==\n"), diffs.size());
    for (const auto& d : diffs) {
        wxString header = wxString::Format("\n--- Claim %d ", d.claim_number);
        if (d.removed_claim) header += UTF8_STR("[仅存在于旧版本 → 已删除/CANCELED]");
        else if (d.added_claim) header += "[NEW]";
        else if (d.current_status == patx::ClaimStatus::Canceled) header += "[CANCELED]";
        else if (d.current_status == patx::ClaimStatus::CurrentlyAmended) header += "[CURRENTLY AMENDED]";
        else if (d.current_status == patx::ClaimStatus::New) header += "[NEW]";
        header += wxString::Format(" (%s → %s)\n", patx::ToString(d.previous_status),
                                   patx::ToString(d.current_status));

        // Header + status always visible; color code the changed words
        wxTextAttr base = out->GetDefaultStyle();
        wxTextAttr removed_attr(*wxRED);
        wxTextAttr added_attr(wxColour(0, 128, 0));
        wxTextAttr changed_header(d.HasChanges() ? *wxBLUE : *wxBLACK);

        out->SetDefaultStyle(changed_header);
        *out << header;
        if (d.removed_claim || d.added_claim) {
            for (const auto& seg : d.segments) {
                out->SetDefaultStyle(seg.op == patx::DiffOp::Removed ? removed_attr : added_attr);
                *out << seg.text << " ";
            }
            *out << "\n";
        } else {
            bool has_word_changes = d.added_words > 0 || d.removed_words > 0;
            if (!has_word_changes && d.previous_status == d.current_status) {
                out->SetDefaultStyle(base);
                *out << UTF8_STR("(无变化)\n");
            }
            for (const auto& seg : d.segments) {
                if (seg.op == patx::DiffOp::Removed) {
                    out->SetDefaultStyle(removed_attr);
                    *out << "[" << seg.text << "]";
                } else if (seg.op == patx::DiffOp::Added) {
                    out->SetDefaultStyle(added_attr);
                    *out << "{" << seg.text << "}";
                } else {
                    out->SetDefaultStyle(base);
                    *out << seg.text << " ";
                }
            }
            if (has_word_changes) {
                out->SetDefaultStyle(changed_header);
                *out << wxString::Format("\n    (+%d / -%d words)\n", d.added_words, d.removed_words);
            } else {
                *out << "\n";
            }
        }
    }
    out->SetDefaultStyle(out->GetDefaultStyle());
}

void UsProsecutionPanel::OnCaseSelected(wxListEvent& event) {
    selected_case_id_ = static_cast<int>(case_list_->GetItemData(event.GetIndex()));
    LoadCaseDetail();
}

// ---------------------------------------------------------------------------
// Worker management
// ---------------------------------------------------------------------------

void UsProsecutionPanel::StartSyncWorker(int case_id, const std::string& new_app_no,
                                         int foreign_patent_id) {
    patx::UsptoConfig config = CurrentConfig();
    if (config.api_key.empty()) {
        const char* env = std::getenv("USPTO_API_KEY");
        if (env && *env) config.api_key = env;
    }
    if (config.api_key.empty() && new_app_no.empty()) {
        // Syncing an existing case without a key still parses nothing new;
        // be explicit instead of failing with 401 later.
        if (wxMessageBox(UTF8_STR("未配置 USPTO API Key。\n打开设置现在配置吗？\n(没有 Key 时其他功能不受影响)"),
                         UTF8_STR("USPTO"), wxYES_NO | wxICON_QUESTION) == wxYES) {
            UsptoSettingsDialog dlg(this, db_path_);
            dlg.ShowModal();
        }
        return;
    }

    SyncJobSpec spec;
    if (!new_app_no.empty()) {
        spec.job = SyncJob::kAddCase;
        spec.app_no = new_app_no;
        spec.foreign_patent_id = foreign_patent_id;
    } else if (case_id > 0) {
        spec.job = SyncJob::kSyncCase;
        spec.case_id = case_id;
    } else {
        spec.job = SyncJob::kSyncAll;
    }

    wxBusyCursor busy;
    SyncWorker* worker = new SyncWorker(GetEventHandler(), db_path_, config, spec);
    if (worker->Create() != wxTHREAD_NO_ERROR) {
        wxMessageBox("failed to start sync worker", "Error", wxOK | wxICON_ERROR);
        delete worker;
        return;
    }
    active_workers_++;
    worker->Run();
}

void UsProsecutionPanel::StartBatchWorker(const std::vector<std::string>& identifiers) {
    patx::UsptoConfig config = CurrentConfig();
    if (config.api_key.empty()) {
        const char* env = std::getenv("USPTO_API_KEY");
        if (env && *env) config.api_key = env;
    }
    if (config.api_key.empty()) {
        wxMessageBox(UTF8_STR("批量导入需要 USPTO API Key。\n打开设置现在配置吗？"),
                     UTF8_STR("USPTO"), wxYES_NO | wxICON_QUESTION);
        UsptoSettingsDialog dlg(this, db_path_);
        dlg.ShowModal();
        return;
    }

    SyncJobSpec spec;
    spec.job = SyncJob::kBatchAdd;
    spec.identifiers = identifiers;

    wxBusyCursor busy;
    SyncWorker* worker = new SyncWorker(GetEventHandler(), db_path_, config, spec);
    if (worker->Create() != wxTHREAD_NO_ERROR) {
        wxMessageBox("failed to start batch worker", "Error", wxOK | wxICON_ERROR);
        delete worker;
        return;
    }
    active_workers_++;
    worker->Run();
}

void UsProsecutionPanel::OnWorkerDone(wxThreadEvent& event) {
    active_workers_--;
    bool ok = event.GetInt() == 1;
    std::string summary = event.GetPayload<std::string>();
    PATX_LOG_INFO(std::string("USPTO worker finished: ") + summary);

    RefreshCases();
    if (selected_case_id_ > 0) LoadCaseDetail();

    if (summary.find("new document") != std::string::npos) {
        wxMessageBox(wxString::FromUTF8(summary.c_str()),
                     UTF8_STR("USPTO 同步"), wxOK | wxICON_INFORMATION);
    } else if (!ok) {
        wxMessageBox(wxString::FromUTF8(summary.c_str()),
                     UTF8_STR("USPTO 同步失败"), wxOK | wxICON_WARNING);
    }
    GetGrandParent()->GetEventHandler()->SetEvtHandlerEnabled(true);
}

void UsProsecutionPanel::CheckAutoSync() {
    std::string interval = db_->GetConfig("uspto_auto_sync_interval");
    if (interval.empty() || interval == "manual") return;
    // Timer context: no modal dialogs allowed; without a key there is
    // nothing to sync.
    if (db_->GetConfig("uspto_api_key").empty() && !std::getenv("USPTO_API_KEY")) return;

    long long interval_secs = interval == "12h" ? 12LL * 3600 : 24LL * 3600;
    long long last = std::atoll(db_->GetConfig("uspto_last_auto_sync").c_str());
    long long now = static_cast<long long>(time(nullptr));
    if (last != 0 && now - last < interval_secs) return;

    if (active_workers_ > 0) return;   // user-initiated work in flight
    auto cases = repo_->GetAllCases();
    if (cases.empty()) return;

    db_->SetConfig("uspto_last_auto_sync", std::to_string(now));
    PATX_LOG_INFO("Auto sync started (" + interval + ")");
    StartSyncWorker(0);
}
