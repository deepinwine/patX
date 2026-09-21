// EPO OPS 同族查询对话框：输入/带入公开号 → sidecar → EPO DOCDB family。
// 凭据保存在本地 config 表（git-ignored），永不写入日志或仓库。
#include <nlohmann/json.hpp>
#include "wx/wx.h"
#include <wx/listctrl.h>
#include <wx/textdlg.h>

#include <thread>

#include "database.hpp"
#include "web_dossier.hpp"

#ifndef UTF8_STR
#define UTF8_STR(x) wxString::FromUTF8(x)
#endif

namespace {

// Runs the EPO call on a worker thread; the dialog polls a shared state.
struct EpoJob {
    std::string response;
    bool done = false;
    bool ok = false;
};

} // namespace

class EpoFamilyDialog : public wxDialog {
public:
    EpoFamilyDialog(wxWindow* parent, Database& db, webdossier::Manager& manager)
        : wxDialog(parent, wxID_ANY, UTF8_STR("同族查询 / Patent Family (EPO OPS)"),
                   wxDefaultPosition, wxSize(760, 500), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          db_(db), manager_(manager) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        auto* top = new wxBoxSizer(wxHORIZONTAL);
        top->Add(new wxStaticText(this, wxID_ANY, UTF8_STR("公开号:")), 0,
                 wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        input_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(240, -1));
        input_->SetHint(UTF8_STR("如 CN119870049A / US20240270309A1"));
        top->Add(input_, 0, wxRIGHT, 8);
        auto* go = new wxButton(this, wxID_ANY, UTF8_STR("查询同族"));
        go->Bind(wxEVT_BUTTON, &EpoFamilyDialog::OnQuery, this);
        top->Add(go, 0, wxRIGHT, 8);
        auto* cfg = new wxButton(this, wxID_ANY, UTF8_STR("EPO Key..."));
        cfg->Bind(wxEVT_BUTTON, &EpoFamilyDialog::OnConfig, this);
        top->Add(cfg, 0, wxRIGHT, 8);
        status_ = new wxStaticText(this, wxID_ANY, "");
        top->Add(status_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
        sizer->Add(top, 0, wxALL | wxEXPAND, 10);

        list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxLC_REPORT | wxBORDER_SUNKEN);
        list_->InsertColumn(0, UTF8_STR("受理局"), wxLIST_FORMAT_LEFT, 70);
        list_->InsertColumn(1, UTF8_STR("申请号"), wxLIST_FORMAT_LEFT, 150);
        list_->InsertColumn(2, UTF8_STR("申请日"), wxLIST_FORMAT_LEFT, 100);
        list_->InsertColumn(3, UTF8_STR("公开号"), wxLIST_FORMAT_LEFT, 160);
        list_->InsertColumn(4, UTF8_STR("类型"), wxLIST_FORMAT_LEFT, 60);
        sizer->Add(list_, 1, wxALL | wxEXPAND, 5);

        auto* close = new wxButton(this, wxID_OK, UTF8_STR("关闭"));
        sizer->Add(close, 0, wxALL | wxALIGN_RIGHT, 8);
        SetSizer(sizer);

        Bind(wxEVT_IDLE, &EpoFamilyDialog::OnIdle, this);
    }

    void SetPublication(const std::string& pub) { input_->SetValue(wxString::FromUTF8(pub.c_str())); }

private:
    void OnConfig(wxCommandEvent&) {
        wxString key = wxString::FromUTF8(db_.GetConfig("epo_consumer_key").c_str());
        wxString secret = wxString::FromUTF8(db_.GetConfig("epo_consumer_secret").c_str());
        wxTextEntryDialog kd(this, UTF8_STR("EPO consumer key（developers.epo.org → 你的应用）:"),
                             UTF8_STR("EPO OPS 配置"), key);
        if (kd.ShowModal() != wxID_OK) return;
        wxTextEntryDialog sd(this, UTF8_STR("EPO consumer secret:"),
                             UTF8_STR("EPO OPS 配置"), secret);
        if (sd.ShowModal() != wxID_OK) return;
        db_.SetConfig("epo_consumer_key", kd.GetValue().ToStdString());
        db_.SetConfig("epo_consumer_secret", sd.GetValue().ToStdString());
        status_->SetLabel(UTF8_STR("已保存（仅存本地数据库，不进仓库）"));
    }

    void OnQuery(wxCommandEvent&) {
        std::string pub = input_->GetValue().ToStdString();
        if (pub.empty()) return;
        if (db_.GetConfig("epo_consumer_key").empty() ||
            db_.GetConfig("epo_consumer_secret").empty()) {
            wxMessageBox(UTF8_STR("请先点击【EPO Key...】配置 consumer key/secret。\n"
                                  "在 developers.epo.org → 你的账号 → Apps 注册应用后获取。"),
                         UTF8_STR("EPO OPS"), wxOK | wxICON_INFORMATION);
            return;
        }
        list_->DeleteAllItems();
        status_->SetLabel(UTF8_STR("查询中..."));
        job_ = std::make_shared<EpoJob>();
        auto job = job_;
        std::thread([this, pub, job]() {
            std::string response;
            bool ok = manager_.EpoCall("family", pub, response);
            job->response = response;
            job->ok = ok;
            job->done = true;
        }).detach();
    }

    void OnIdle(wxIdleEvent&) {
        if (!job_ || !job_->done || job_handled_) return;
        job_handled_ = true;
        ParseAndShow();
        job_.reset();
        job_handled_ = false;
    }

    void ParseAndShow() {
        if (!job_->ok) {
            status_->SetLabel(UTF8_STR("查询失败（sidecar 通信）"));
            return;
        }
        try {
            auto parsed = nlohmann::json::parse(job_->response);
            if (!parsed.value("ok", false)) {
                status_->SetLabel(wxString::FromUTF8(
                    (parsed.value("message", "查询失败") + " [" +
                     parsed.value("code", "") + "]").c_str()));
                return;
            }
            int row = 0;
            for (const auto& m : parsed.value("members", std::vector<nlohmann::json>{})) {
                long idx = list_->InsertItem(row, wxString::FromUTF8(m.value("authority", "").c_str()));
                list_->SetItem(idx, 1, wxString::FromUTF8(m.value("application_number", "").c_str()));
                list_->SetItem(idx, 2, wxString::FromUTF8(m.value("application_date", "").c_str()));
                list_->SetItem(idx, 3, wxString::FromUTF8(m.value("publication_number", "").c_str()));
                list_->SetItem(idx, 4, wxString::FromUTF8(m.value("kind", "").c_str()));
                row++;
            }
            status_->SetLabel(wxString::FromUTF8(parsed.value("message", "").c_str()));
        } catch (const std::exception&) {
            status_->SetLabel(UTF8_STR("响应解析失败"));
        }
    }

    Database& db_;
    webdossier::Manager& manager_;
    wxTextCtrl* input_ = nullptr;
    wxListCtrl* list_ = nullptr;
    wxStaticText* status_ = nullptr;
    std::shared_ptr<EpoJob> job_;
    bool job_handled_ = false;
};

void ShowEpoFamilyDialog(wxWindow* parent, Database& db, webdossier::Manager& manager,
                         const std::string& publication) {
    EpoFamilyDialog dlg(parent, db, manager);
    if (!publication.empty()) dlg.SetPublication(publication);
    dlg.ShowModal();
}
