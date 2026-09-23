// 审查信息同步 GUI - progress dialog, findings list, login flow, history.
//
// The controller owns the webdossier::Manager. All blocking work (sidecar
// RPC round trips) runs on a worker thread; the dialog updates from
// wxThreadEvents. The manager process is started on the MAIN thread
// (wxProcess::Open requirement) before the worker spawns.
#pragma once

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/notifmsg.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "database.hpp"
#include "web_dossier.hpp"

class WebDossierSyncDialog;

class WebDossierController : public wxEvtHandler {
public:
    // on_show_case(geke_code) - jump the main window to that patent.
    WebDossierController(wxWindow* parent, Database& db,
                         std::function<void(const std::string&)> on_show_case);
    ~WebDossierController();

    void SyncPatents(const std::vector<Patent>& patents);
    void SyncAllActive(bool include_granted);
    void Login();
    void ShowHistory();
    // EPO OPS family lookup dialog (needs the sidecar for the REST relay).
    void ShowFamily(const std::string& publication);
    void ShowErrorCases();

    // Silent background sweep of the due queue: no dialog, no popups; a
    // desktop notification appears only when something needs attention.
    void SyncDueInBackground();
    // Check-interval setting (1 / 3 / 7 days).
    void ShowSettings();
    // List window over the last background batch's findings.
    void ShowLastFindings();
    // Opens the sync window with the "登录 CNIPA" button - never opens the
    // login browser by itself.
    void ShowLoginRequired();

    // Called on the main thread after every finished batch (new events may
    // have been inserted); the frame uses it to refresh the OA list.
    void set_on_finished(std::function<void()> fn) { on_finished_ = std::move(fn); }

    bool busy() const { return running_.load(); }
    bool background_run() const { return background_run_; }

private:
    friend class WebDossierSyncDialog;
    bool EnsureManager(std::string& error);
    void StartWorker(const std::vector<Patent>& queue, bool is_login_only,
                     bool show_dialog = true, bool background_run = false);
    void OnBatchDone(wxThreadEvent& event);
    void ShowBatchNotification(const webdossier::BatchSummary& summary);

    wxWindow* parent_;
    Database& db_;
    std::function<void(const std::string&)> on_show_case_;
    std::function<void()> on_finished_;
    std::unique_ptr<webdossier::Manager> manager_;
    WebDossierSyncDialog* active_dialog_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    bool background_run_ = false;
    webdossier::BatchSummary last_summary_;
    std::unique_ptr<wxNotificationMessage> notification_;
};
