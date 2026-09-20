// US Prosecution tab: case list + detail notebook (Overview / Timeline /
// Claims with version diff / Office Actions / Documents / Sync Log).
//
// All USPTO network work runs on a worker thread (never the wx main loop);
// results come back via wxThreadEvent. Each worker opens its own SQLite
// connection (WAL) so the GUI connection stays responsive.
#pragma once

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/splitter.h>
#include <wx/thread.h>

#include <atomic>
#include <memory>
#include <string>

#include "database.hpp"
#include "patx/claim_diff.hpp"
#include "patx/uspto_models.hpp"
#include "patx/uspto_repository.hpp"

class UsptoSettingsDialog;

class UsProsecutionPanel : public wxPanel {
public:
    UsProsecutionPanel(wxWindow* parent, const std::string& db_path);

    void RefreshCases();
    // Called by the frame's auto-sync timer; starts an all-cases sync when
    // the configured interval has elapsed.
    void CheckAutoSync();
    // Toolbar actions (also triggered from the main frame's New button)
    void OnAddCase(wxCommandEvent&);
    void OnSyncSelected(wxCommandEvent&);
    void OnSyncAll(wxCommandEvent&);
    void OnSettings(wxCommandEvent&);

    patx::UsptoConfig CurrentConfig() const;

private:
    // UI construction
    void BuildToolbar(wxSizer* parent_sizer);
    void BuildCaseList(wxSplitterWindow* splitter);
    void BuildDetailBook(wxSplitterWindow* splitter);

    // Data loading (main thread only)
    void LoadCaseDetail();
    void LoadOverview();
    void LoadTimeline();
    void LoadClaims();
    void LoadOaList();
    void LoadDocuments();
    void LoadSyncLog();

    // Actions
    void OnBatchImport(wxCommandEvent&);
    void OnDownloadAll(wxCommandEvent&);
    void OnDownloadDocument(wxCommandEvent&);
    void OnOpenDocument(wxCommandEvent&);
    void OnReparseDocument(wxCommandEvent&);
    void OnTimelineDirection(wxCommandEvent&);
    void OnCompareVersions(wxCommandEvent&);
    void OnCaseSelected(wxListEvent&);
    void OnWorkerDone(wxThreadEvent&);

    // Sync plumbing
    void StartSyncWorker(int case_id /*0 = all cases*/, const std::string& new_app_no = "",
                         int foreign_patent_id = 0);
    // Batch add: identifiers come from a spreadsheet (US publication/
    // application/patent numbers); each is resolved and synced in sequence.
    void StartBatchWorker(const std::vector<std::string>& identifiers);
    void ShowClaimDiff(int version_a, int version_b);
    void AppendDiffText(class wxTextCtrl* out, const std::vector<patx::ClaimDiff>& diffs);

    std::string db_path_;
    std::unique_ptr<Database> db_;        // UI-thread connection (read-mostly)
    patx::UsptoRepository* repo_ = nullptr;

    wxListCtrl* case_list_ = nullptr;
    wxNotebook* detail_book_ = nullptr;

    // Overview
    wxTextCtrl* overview_text_ = nullptr;
    // Timeline
    wxListCtrl* timeline_list_ = nullptr;
    wxButton* timeline_dir_btn_ = nullptr;
    bool timeline_ascending_ = true;
    // Claims
    wxChoice* version_a_choice_ = nullptr;
    wxChoice* version_b_choice_ = nullptr;
    wxTextCtrl* claim_diff_text_ = nullptr;
    wxStaticText* claims_summary_ = nullptr;
    // OA
    wxListCtrl* oa_doc_list_ = nullptr;
    // Documents
    wxListCtrl* doc_list_ = nullptr;
    // Sync log
    wxListCtrl* sync_log_list_ = nullptr;

    int selected_case_id_ = 0;
    std::atomic<int> active_workers_{0};

    friend class UsptoSettingsDialog;
};
