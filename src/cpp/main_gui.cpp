// patX GUI - Patent Manager frame.
//
// Edit dialogs live in ui/edit_dialogs.*; the web dossier sync UI lives in
// ui/web_dossier_dialogs.*. This file is the frame: menus, tabs, filtering,
// import/export, backup/NAS, theme/language.
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/splitter.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/textdlg.h>
#include <wx/choicdlg.h>
#include <wx/progdlg.h>
#include <wx/aui/auibook.h>
#include <wx/textfile.h>
#include <wx/file.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/spinctrl.h>
#include <wx/filename.h>
#include <wx/regex.h>
#include <wx/timer.h>
#include <memory>
#include <fstream>
#include <set>
#include <filesystem>

#include "patx/version.h"
#include "patx/log.hpp"
#include "database.hpp"
#include "excel_io.hpp"
#include "pdf_parser.hpp"
#include "ui/edit_dialogs.hpp"
#include "ui/web_dossier_dialogs.hpp"

#define UTF8_STR(s) wxString::FromUTF8(s)
// wxString -> UTF-8 std::string helper (wx 3.2/3.3 compatible)
static inline std::string ToStd(const wxString& s) { return std::string(s.utf8_str()); }
#define LANG_STR(en, zh) (current_lang == 0 ? wxString(en) : wxString::FromUTF8(zh))
#define LANG_STR_L(lang, en, zh) (lang == 0 ? wxString(en) : wxString::FromUTF8(zh))

using patx::PdfParser;
using patx::GetPdfParser;
using patx::ParsedOaInfo;
using patx::OaType;

// Natural sort comparison: splits into letter + digit segments,
// sorts letter parts lexicographically, digit parts numerically.
static int NaturalCompare(const wxString& a, const wxString& b) {
    if (a.IsEmpty() && b.IsEmpty()) return 0;
    if (a.IsEmpty()) return 1;
    if (b.IsEmpty()) return -1;

    size_t ia = 0, ib = 0;
    while (ia < (size_t)a.Len() && ib < (size_t)b.Len()) {
        wxChar ca = a[ia], cb = b[ib];
        bool a_digit = (ca >= '0' && ca <= '9');
        bool b_digit = (cb >= '0' && cb <= '9');

        if (a_digit && b_digit) {
            long na = 0, nb = 0;
            while (ia < (size_t)a.Len() && a[ia] >= '0' && a[ia] <= '9')
                na = na * 10 + (a[ia++] - '0');
            while (ib < (size_t)b.Len() && b[ib] >= '0' && b[ib] <= '9')
                nb = nb * 10 + (b[ib++] - '0');
            if (na != nb) return na < nb ? -1 : 1;
        } else if (a_digit != b_digit) {
            return b_digit ? 1 : -1;
        } else {
            if (ca != cb) {
                wxChar la = wxTolower(ca), lb = wxTolower(cb);
                if (la != lb) return la < lb ? -1 : 1;
            }
            ia++; ib++;
        }
    }
    return (ia >= (size_t)a.Len() && ib >= (size_t)b.Len()) ? 0 :
           (ia >= (size_t)a.Len()) ? -1 : 1;
}

static int SmartCompare(const wxString& a, const wxString& b, bool ascending) {
    int cmp = NaturalCompare(a, b);
    return ascending ? cmp : -cmp;
}

static void SortListCtrl(wxListCtrl* list, int col, bool asc) {
    int count = list->GetItemCount();
    if (count <= 1) return;
    struct RowData { int orig_row; wxString text; wxIntPtr data; };
    std::vector<RowData> rows(count);
    int ncols = list->GetColumnCount();
    for (int i = 0; i < count; i++) {
        rows[i].orig_row = i;
        rows[i].text = list->GetItemText(i, col);
        rows[i].data = list->GetItemData(i);
    }
    std::stable_sort(rows.begin(), rows.end(), [asc](const RowData& a, const RowData& b) {
        return SmartCompare(a.text, b.text, asc) < 0;
    });
    std::vector<std::vector<wxString>> texts(count, std::vector<wxString>(ncols));
    for (int i = 0; i < count; i++)
        for (int c = 0; c < ncols; c++)
            texts[i][c] = list->GetItemText(i, c);
    list->DeleteAllItems();
    for (int i = 0; i < count; i++) {
        int orig = rows[i].orig_row;
        long idx = list->InsertItem(i, texts[orig][0]);
        for (int c = 1; c < ncols; c++)
            list->SetItem(idx, c, texts[orig][c]);
        list->SetItemData(idx, rows[i].data);
    }
}

static std::map<wxListCtrl*, std::map<int, std::string>> g_column_filters;

// ============== NAS Configuration ==============
struct NASConfig {
    std::string nas_path;
    std::string username;
    int auto_sync_minutes = 0;
    bool enabled = false;

    void Save() {
        std::ofstream f("nas_config.txt");
        if (f.is_open()) {
            f << nas_path << "\n" << username << "\n" << auto_sync_minutes << "\n"
              << (enabled ? "1" : "0") << "\n";
            f.close();
        }
    }

    void Load() {
        std::ifstream f("nas_config.txt");
        if (f.is_open()) {
            std::getline(f, nas_path);
            std::getline(f, username);
            std::string line;
            std::getline(f, line);
            auto_sync_minutes = std::atoi(line.c_str());
            std::getline(f, line);
            enabled = (line == "1");
            f.close();
        }
    }

    static NASConfig& Get() {
        static NASConfig instance;
        static bool loaded = false;
        if (!loaded) {
            instance.Load();
            loaded = true;
        }
        return instance;
    }
};

// ============== Main Frame ==============
class PatXFrame : public wxFrame {
public:
    PatXFrame()
        : wxFrame(nullptr, wxID_ANY, wxString("patX - Patent Manager v") + PATX_VERSION,
                  wxDefaultPosition, wxSize(1600, 900)) {
        db = std::make_unique<Database>("patents.db");
        patx::InitLogging("patx.log");
        PATX_LOG_INFO(std::string("patX v") + PATX_VERSION + " started (schema v" +
                      std::to_string(db->SchemaVersion()) + ")");
        dossier_controller = std::make_unique<WebDossierController>(
            this, *db, [this](const std::string& geke_code) { ShowPatentByCode(geke_code); });
        dossier_controller->set_on_finished([this]() { LoadOA(); });
        SetupMenu();
        SetupUI();
        LoadAllData();

        // Auto-sync check timer (fires every 30 min; the panel decides
        // whether the configured interval has elapsed)
    }

private:
    std::unique_ptr<Database> db;
    std::unique_ptr<WebDossierController> dossier_controller;
    wxAuiNotebook* notebook;
    wxStatusBar* status_bar;
    wxTimer* auto_sync_timer_ = nullptr;

    int current_theme = 0;
    int current_lang = 0;

    // Patent tab
    wxListCtrl* patent_list;
    wxTextCtrl* common_search;
    wxComboBox* common_status_filter;
    wxComboBox* common_handler_filter;
    wxComboBox* common_level_filter;
    wxStaticText* lbl_level;
    wxTextCtrl* patent_detail;

    // OA tab
    wxListCtrl* oa_list;
    wxComboBox* oa_filter;

    // Other tabs
    wxListCtrl* pct_list;
    wxListCtrl* sw_list;
    wxListCtrl* ic_list;
    wxListCtrl* foreign_list;
    wxListCtrl* fee_list;
    wxListCtrl* rule_list;

    std::map<wxListCtrl*, std::pair<int, bool>> sort_state;

    enum {
        ID_SYNC = wxID_HIGHEST + 1,
        ID_NAS_CONFIG,
        ID_BACKUP,
        ID_RESTORE,
        ID_THEME_LIGHT,
        ID_THEME_DARK,
        ID_THEME_EYE,
        ID_SEARCH_PATENTS,
        ID_COPY_CODE,
        ID_COPY_TITLE,
        ID_SET_LEVEL,
        ID_SWITCH_DB,
        ID_AUTO_BACKUP,
        ID_PRINT,
        ID_EXPORT_PDF,
        ID_LANG_EN,
        ID_LANG_ZH,
        ID_VALIDATE_DATA,
        ID_STATISTICS,
        ID_RULE_ADD,
        ID_RULE_EDIT,
        ID_RULE_DELETE,
        ID_RULE_TOGGLE,
        ID_FEE_GENERATE,
        ID_FEE_MARK_PAID,
        ID_DOSSIER_SYNC_SELECTED,
        ID_DOSSIER_SYNC_ALL,
        ID_DOSSIER_SYNC_GRANTED,
        ID_DOSSIER_LOGIN,
        ID_DOSSIER_HISTORY,
        ID_DOSSIER_ERRORS
    };

    // Toolbar buttons stored for language switching
    std::vector<wxButton*> toolbar_btns;
    wxButton* btn_new = nullptr;
    wxButton* btn_edit = nullptr;
    wxButton* btn_delete = nullptr;
    wxButton* btn_batch = nullptr;
    wxStaticText* lbl_search = nullptr;
    wxStaticText* lbl_status = nullptr;
    wxStaticText* lbl_handler = nullptr;

    void SetupMenu() {
        wxMenuBar* mb = new wxMenuBar;

        wxMenu* file_menu = new wxMenu;
        file_menu->Append(wxID_NEW, LANG_STR("&New\tCtrl+N", "新建(&N)\tCtrl+N"));
        file_menu->Append(wxID_OPEN, LANG_STR("&Import...", "导入(&I)..."));
        file_menu->Append(wxID_SAVE, LANG_STR("&Export...", "导出(&E)..."));
        file_menu->AppendSeparator();
        file_menu->Append(ID_SWITCH_DB, LANG_STR("Switch Database...", "切换数据库..."));
        file_menu->AppendSeparator();
        file_menu->Append(wxID_EXIT, LANG_STR("E&xit\tAlt+F4", "退出(&X)\tAlt+F4"));
        mb->Append(file_menu, LANG_STR("&File", "文件(&F)"));

        wxMenu* edit_menu = new wxMenu;
        // Redo is intentionally absent: the undo manager has no redo, and a
        // clickable-but-dead menu item is worse than none.
        edit_menu->Append(wxID_UNDO, LANG_STR("&Undo\tCtrl+Z", "撤销(&U)\tCtrl+Z"));
        edit_menu->AppendSeparator();
        edit_menu->Append(wxID_EDIT, LANG_STR("&Edit Selected\tEnter", "编辑(&E)\tEnter"));
        edit_menu->Append(wxID_DELETE, LANG_STR("&Delete\tDel", "删除(&D)\tDel"));
        mb->Append(edit_menu, LANG_STR("&Edit", "编辑(&E)"));

        wxMenu* view_menu = new wxMenu;
        view_menu->Append(wxID_REFRESH, LANG_STR("&Refresh\tF5", "刷新(&R)\tF5"));
        view_menu->AppendSeparator();
        view_menu->Append(ID_THEME_LIGHT, LANG_STR("Light Theme", "浅色主题"));
        view_menu->Append(ID_THEME_DARK, LANG_STR("Dark Theme", "深色主题"));
        view_menu->Append(ID_THEME_EYE, LANG_STR("Eye Protection Theme", "护眼主题"));
        view_menu->AppendSeparator();
        view_menu->Append(ID_LANG_EN, "English");
        view_menu->Append(ID_LANG_ZH, UTF8_STR("中文"));
        mb->Append(view_menu, LANG_STR("&View", "视图(&V)"));

        wxMenu* tools_menu = new wxMenu;
        tools_menu->Append(ID_STATISTICS, LANG_STR("&Statistics...", "统计(&S)..."));
        tools_menu->Append(ID_VALIDATE_DATA, LANG_STR("Validate Data", "数据验证"));
        tools_menu->AppendSeparator();
        tools_menu->Append(ID_SYNC, LANG_STR("&Sync with NAS", "NAS同步(&N)"));
        tools_menu->Append(ID_NAS_CONFIG, LANG_STR("NAS &Configuration...", "NAS配置(&C)..."));
        tools_menu->AppendSeparator();
        tools_menu->Append(ID_BACKUP, LANG_STR("&Backup Database", "备份数据库(&B)"));
        tools_menu->Append(ID_RESTORE, LANG_STR("&Restore Backup...", "恢复备份(&R)..."));
        tools_menu->AppendSeparator();
        tools_menu->Append(wxID_VIEW_LIST, LANG_STR("Open Log Folder", "打开日志文件夹"));
        mb->Append(tools_menu, LANG_STR("&Tools", "工具(&T)"));

        wxMenu* dossier_menu = new wxMenu;
        dossier_menu->Append(ID_DOSSIER_SYNC_SELECTED,
                             LANG_STR("Update &Selected Case", "更新选中案件审查信息(&S)"));
        dossier_menu->Append(ID_DOSSIER_SYNC_ALL,
                             LANG_STR("Update All &Active Cases", "更新全部活跃案件(&A)"));
        dossier_menu->Append(ID_DOSSIER_SYNC_GRANTED,
                             LANG_STR("Update All (incl. Granted)", "更新全部（含已授权）"));
        dossier_menu->AppendSeparator();
        dossier_menu->Append(ID_DOSSIER_LOGIN, LANG_STR("CNIPA &Login...", "CNIPA 登录(&L)..."));
        dossier_menu->Append(ID_DOSSIER_HISTORY, LANG_STR("Sync &History", "同步历史(&H)"));
        dossier_menu->Append(ID_DOSSIER_ERRORS, LANG_STR("&Problem Cases", "异常案件(&P)"));
        mb->Append(dossier_menu, LANG_STR("&Dossier Sync", "审查信息同步(&D)"));

        wxMenu* help_menu = new wxMenu;
        help_menu->Append(wxID_ABOUT, LANG_STR("&About", "关于(&A)"));
        mb->Append(help_menu, LANG_STR("&Help", "帮助(&H)"));

        SetMenuBar(mb);

        Bind(wxEVT_MENU, &PatXFrame::OnExit, this, wxID_EXIT);
        Bind(wxEVT_MENU, &PatXFrame::OnNewByCurrentTab, this, wxID_NEW);
        Bind(wxEVT_MENU, &PatXFrame::OnEditByCurrentTab, this, wxID_EDIT);
        Bind(wxEVT_MENU, &PatXFrame::OnDeleteByCurrentTab, this, wxID_DELETE);
        Bind(wxEVT_MENU, &PatXFrame::OnRefresh, this, wxID_REFRESH);
        Bind(wxEVT_MENU, &PatXFrame::OnImport, this, wxID_OPEN);
        Bind(wxEVT_MENU, &PatXFrame::OnExport, this, wxID_SAVE);
        Bind(wxEVT_MENU, &PatXFrame::OnUndo, this, wxID_UNDO);
        Bind(wxEVT_MENU, &PatXFrame::OnSync, this, ID_SYNC);
        Bind(wxEVT_MENU, &PatXFrame::OnStatistics, this, ID_STATISTICS);
        Bind(wxEVT_MENU, &PatXFrame::OnNasConfig, this, ID_NAS_CONFIG);
        Bind(wxEVT_MENU, &PatXFrame::OnBackup, this, ID_BACKUP);
        Bind(wxEVT_MENU, &PatXFrame::OnRestore, this, ID_RESTORE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            wxLaunchDefaultApplication(wxFileName("patx.log").GetAbsolutePath());
        }, wxID_VIEW_LIST);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { OnValidateData(); }, ID_VALIDATE_DATA);
        Bind(wxEVT_MENU, &PatXFrame::OnSwitchDatabase, this, ID_SWITCH_DB);
        Bind(wxEVT_MENU, &PatXFrame::OnAbout, this, wxID_ABOUT);
        Bind(wxEVT_MENU, &PatXFrame::OnDossierSyncSelected, this, ID_DOSSIER_SYNC_SELECTED);
        Bind(wxEVT_MENU, &PatXFrame::OnDossierSyncAll, this, ID_DOSSIER_SYNC_ALL);
        Bind(wxEVT_MENU, &PatXFrame::OnDossierSyncGranted, this, ID_DOSSIER_SYNC_GRANTED);
        Bind(wxEVT_MENU, &PatXFrame::OnDossierLogin, this, ID_DOSSIER_LOGIN);
        Bind(wxEVT_MENU, &PatXFrame::OnDossierHistory, this, ID_DOSSIER_HISTORY);
        Bind(wxEVT_MENU, &PatXFrame::OnDossierErrors, this, ID_DOSSIER_ERRORS);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { SetTheme(0); }, ID_THEME_LIGHT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { SetTheme(1); }, ID_THEME_DARK);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { SetTheme(2); }, ID_THEME_EYE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { SetLanguage(0); }, ID_LANG_EN);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { SetLanguage(1); }, ID_LANG_ZH);
    }

    void SetupUI() {
        wxPanel* main_panel = new wxPanel(this);
        wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* top_bar = new wxBoxSizer(wxHORIZONTAL);
        wxStaticText* title = new wxStaticText(main_panel, wxID_ANY, "Patent Data Management");
        title->SetFont(wxFont(16, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
        top_bar->Add(title, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 20);
        top_bar->Add(new wxStaticText(main_panel, wxID_ANY, "Database: patents.db"),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 20);
        top_bar->AddStretchSpacer();

        auto add_btn = [&](const wxString& label, void (PatXFrame::*handler)(wxCommandEvent&)) {
            wxButton* btn = new wxButton(main_panel, wxID_ANY, label);
            btn->Bind(wxEVT_BUTTON, handler, this);
            top_bar->Add(btn, 0, wxRIGHT, 5);
            return btn;
        };
        toolbar_btns.push_back(add_btn(LANG_STR("Undo", "撤销"), &PatXFrame::OnUndo));
        toolbar_btns.push_back(add_btn(LANG_STR("Sync", "同步"), &PatXFrame::OnSync));
        toolbar_btns.push_back(add_btn(LANG_STR("Export", "导出"), &PatXFrame::OnExport));
        toolbar_btns.push_back(add_btn(LANG_STR("Import", "导入"), &PatXFrame::OnImport));
        wxButton* refresh_btn = add_btn(LANG_STR("Refresh", "刷新"), &PatXFrame::OnRefresh);
        toolbar_btns.push_back(refresh_btn);
        refresh_btn->SetToolTip("F5");

        main_sizer->Add(top_bar, 0, wxALL, 5);

        notebook = new wxAuiNotebook(main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                     wxAUI_NB_TOP | wxAUI_NB_TAB_SPLIT | wxAUI_NB_TAB_MOVE);
        notebook->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
            RefreshCommonFiltersForTab(e.GetSelection(), true);
            e.Skip();
        });
        notebook->SetMinSize(wxSize(800, 600));

        wxPanel* toolbar_panel = new wxPanel(main_panel);
        wxBoxSizer* toolbar_sizer = new wxBoxSizer(wxHORIZONTAL);

        btn_new = new wxButton(toolbar_panel, wxID_ANY, LANG_STR("New", "新建"));
        btn_new->Bind(wxEVT_BUTTON, &PatXFrame::OnNewByCurrentTab, this);
        toolbar_sizer->Add(btn_new, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        btn_edit = new wxButton(toolbar_panel, wxID_ANY, LANG_STR("Edit", "编辑"));
        btn_edit->Bind(wxEVT_BUTTON, &PatXFrame::OnEditByCurrentTab, this);
        toolbar_sizer->Add(btn_edit, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        btn_delete = new wxButton(toolbar_panel, wxID_ANY, LANG_STR("Delete", "删除"));
        btn_delete->Bind(wxEVT_BUTTON, &PatXFrame::OnDeleteByCurrentTab, this);
        toolbar_sizer->Add(btn_delete, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        btn_batch = new wxButton(toolbar_panel, wxID_ANY, LANG_STR("Batch", "批量"));
        btn_batch->Bind(wxEVT_BUTTON, &PatXFrame::OnBatchByCurrentTab, this);
        toolbar_sizer->Add(btn_batch, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        toolbar_sizer->AddSpacer(15);

        lbl_search = new wxStaticText(toolbar_panel, wxID_ANY, LANG_STR("Search:", "搜索:"));
        toolbar_sizer->Add(lbl_search, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        common_search = new wxTextCtrl(toolbar_panel, wxID_ANY, "", wxDefaultPosition,
                                       wxSize(200, -1), wxTE_PROCESS_ENTER);
        common_search->Bind(wxEVT_TEXT_ENTER, &PatXFrame::OnSearchByCurrentTab, this);
        toolbar_sizer->Add(common_search, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        lbl_status = new wxStaticText(toolbar_panel, wxID_ANY, LANG_STR("Status:", "状态:"));
        toolbar_sizer->Add(lbl_status, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        common_status_filter = new wxComboBox(toolbar_panel, wxID_ANY, LANG_STR("All", "全部"),
                                              wxDefaultPosition, wxSize(110, -1));
        common_status_filter->Append(LANG_STR("All", "全部"));
        common_status_filter->Bind(wxEVT_COMBOBOX, &PatXFrame::OnFilterByCurrentTab, this);
        toolbar_sizer->Add(common_status_filter, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        lbl_handler = new wxStaticText(toolbar_panel, wxID_ANY, LANG_STR("Handler:", "处理人:"));
        toolbar_sizer->Add(lbl_handler, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        common_handler_filter = new wxComboBox(toolbar_panel, wxID_ANY, LANG_STR("All", "全部"),
                                               wxDefaultPosition, wxSize(100, -1));
        common_handler_filter->Bind(wxEVT_COMBOBOX, &PatXFrame::OnFilterByCurrentTab, this);
        toolbar_sizer->Add(common_handler_filter, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        lbl_level = new wxStaticText(toolbar_panel, wxID_ANY, LANG_STR("Level:", "等级:"));
        toolbar_sizer->Add(lbl_level, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        common_level_filter = new wxComboBox(toolbar_panel, wxID_ANY, LANG_STR("All", "全部"),
                                             wxDefaultPosition, wxSize(90, -1));
        common_level_filter->Bind(wxEVT_COMBOBOX, &PatXFrame::OnFilterByCurrentTab, this);
        toolbar_sizer->Add(common_level_filter, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        toolbar_panel->SetSizer(toolbar_sizer);
        main_sizer->Add(toolbar_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

        SetupPatentTab();
        SetupOATab();
        SetupPCTTab();
        SetupSoftwareTab();
        SetupICTab();
        SetupForeignTab();
        SetupUSTab();
        SetupAnnualFeeTab();
        SetupDeadlineRulesTab();

        main_sizer->Add(notebook, 1, wxEXPAND | wxALL, 5);

        status_bar = CreateStatusBar();
        status_bar->SetStatusText(wxString("patX v") + PATX_VERSION +
                                  " | Database: patents.db | schema v" +
                                  wxString::Format("%d", db->SchemaVersion()));

        main_panel->SetSizer(main_sizer);
        main_sizer->SetSizeHints(this);
        SetSize(wxSize(1500, 940));
        Centre();
    }

    // ============== Patent Tab ==============
    void SetupPatentTab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* tb1 = new wxBoxSizer(wxHORIZONTAL);
        tb1->Add(new wxStaticText(panel, wxID_ANY, UTF8_STR("发明人:")),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        inventor_filter_ = new wxComboBox(panel, wxID_ANY, UTF8_STR("全部"),
                                          wxDefaultPosition, wxSize(120, -1));
        inventor_filter_->Append(UTF8_STR("全部"));
        for (const auto& inv : db->GetDistinctValues("patents", "inventor")) {
            inventor_filter_->Append(DB_STR(inv));
        }
        inventor_filter_->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { LoadPatents(); });
        tb1->Add(inventor_filter_, 0, wxRIGHT, 10);

        wxButton* export_btn = new wxButton(panel, wxID_ANY, LANG_STR("Export", "导出"));
        export_btn->Bind(wxEVT_BUTTON, &PatXFrame::OnExport, this);
        tb1->Add(export_btn, 0, wxRIGHT, 5);

        wxButton* report_btn = new wxButton(panel, wxID_ANY, "Report");
        report_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ExportReport(); });
        tb1->Add(report_btn, 0, wxRIGHT, 5);
        tb1->AddStretchSpacer();
        sizer->Add(tb1, 0, wxALL, 5);

        wxBoxSizer* tb2 = new wxBoxSizer(wxHORIZONTAL);
        tb2->Add(new wxStaticText(panel, wxID_ANY, "Batch:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
        wxButton* batch_level_btn = new wxButton(panel, wxID_ANY, "Set Level");
        batch_level_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { BatchSetLevel(); });
        tb2->Add(batch_level_btn, 0, wxRIGHT, 5);
        wxButton* batch_status_btn = new wxButton(panel, wxID_ANY, "Set Status");
        batch_status_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { BatchSetStatus(); });
        tb2->Add(batch_status_btn, 0, wxRIGHT, 5);
        wxButton* calc_exp_btn = new wxButton(panel, wxID_ANY, "Calc Expiration");
        calc_exp_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CalcExpiration(); });
        tb2->Add(calc_exp_btn, 0);
        sizer->Add(tb2, 0, wxALL, 5);

        wxSplitterWindow* splitter = new wxSplitterWindow(panel, wxID_ANY);
        patent_list = new wxListCtrl(splitter, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        patent_list->AppendColumn(UTF8_STR("编号"), wxLIST_FORMAT_LEFT, 90);
        patent_list->AppendColumn(UTF8_STR("申请号"), wxLIST_FORMAT_LEFT, 130);
        patent_list->AppendColumn(UTF8_STR("提案名称"), wxLIST_FORMAT_LEFT, 160);
        patent_list->AppendColumn(UTF8_STR("发明名称"), wxLIST_FORMAT_LEFT, 220);
        patent_list->AppendColumn(UTF8_STR("技术路线"), wxLIST_FORMAT_LEFT, 90);
        patent_list->AppendColumn(UTF8_STR("标签"), wxLIST_FORMAT_LEFT, 90);
        patent_list->AppendColumn(UTF8_STR("原申请人"), wxLIST_FORMAT_LEFT, 130);
        patent_list->AppendColumn(UTF8_STR("现申请人"), wxLIST_FORMAT_LEFT, 130);
        patent_list->AppendColumn(UTF8_STR("类型"), wxLIST_FORMAT_LEFT, 70);
        patent_list->AppendColumn(UTF8_STR("专利等级"), wxLIST_FORMAT_LEFT, 90);
        patent_list->AppendColumn(UTF8_STR("申请状态"), wxLIST_FORMAT_LEFT, 80);
        patent_list->AppendColumn(UTF8_STR("一级分类"), wxLIST_FORMAT_LEFT, 70);
        patent_list->AppendColumn(UTF8_STR("二级分类"), wxLIST_FORMAT_LEFT, 70);
        patent_list->AppendColumn(UTF8_STR("三级分类"), wxLIST_FORMAT_LEFT, 70);
        patent_list->AppendColumn(UTF8_STR("处理人"), wxLIST_FORMAT_LEFT, 70);
        patent_list->AppendColumn(UTF8_STR("发明人"), wxLIST_FORMAT_LEFT, 100);
        patent_list->AppendColumn(UTF8_STR("申请日"), wxLIST_FORMAT_LEFT, 90);
        patent_list->AppendColumn(UTF8_STR("授权日"), wxLIST_FORMAT_LEFT, 90);
        patent_list->AppendColumn(UTF8_STR("到期日"), wxLIST_FORMAT_LEFT, 90);
        patent_list->AppendColumn(UTF8_STR("事务所"), wxLIST_FORMAT_LEFT, 80);
        patent_list->AppendColumn(UTF8_STR("备注"), wxLIST_FORMAT_LEFT, 150);

        patent_list->Bind(wxEVT_LIST_ITEM_SELECTED, &PatXFrame::OnPatentSelected, this);
        patent_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
            long idx = patent_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (idx >= 0) {
                PatentEditDialog dlg(this, db.get(), static_cast<int>(patent_list->GetItemData(idx)));
                if (dlg.ShowModal() == wxID_OK) LoadPatents();
            }
        });
        patent_list->Bind(wxEVT_LIST_COL_CLICK, [this](wxListEvent& e) {
            int col = e.GetColumn();
            auto& state = sort_state[patent_list];
            if (state.first == col) state.second = !state.second;
            else { state.first = col; state.second = true; }
            SortListCtrl(patent_list, col, state.second);
        });
        patent_list->Bind(wxEVT_LIST_COL_RIGHT_CLICK, [this](wxListEvent& e) {
            ShowColumnFilterPopup(patent_list, e.GetColumn());
        });
        patent_list->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent&) {
            long idx = patent_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            wxMenu menu;
            menu.Append(wxID_EDIT, LANG_STR("Edit", "编辑"));
            menu.Append(wxID_DELETE, LANG_STR("Delete", "删除"));
            menu.AppendSeparator();
            menu.Append(wxID_NEW, LANG_STR("New", "新建"));
            menu.AppendSeparator();
            menu.Append(ID_SET_LEVEL, UTF8_STR("设置等级"));
            menu.Append(ID_COPY_CODE, UTF8_STR("复制编号"));
            menu.Append(ID_COPY_TITLE, UTF8_STR("复制标题"));
            menu.Bind(wxEVT_MENU, &PatXFrame::OnEditByCurrentTab, this, wxID_EDIT);
            menu.Bind(wxEVT_MENU, &PatXFrame::OnDeleteByCurrentTab, this, wxID_DELETE);
            menu.Bind(wxEVT_MENU, &PatXFrame::OnNewByCurrentTab, this, wxID_NEW);
            menu.Bind(wxEVT_MENU, [this, idx](wxCommandEvent&) {
                if (idx < 0) return;
                wxArrayString levels;
                levels.Add(UTF8_STR("核心专利"));
                levels.Add(UTF8_STR("重要专利"));
                levels.Add(UTF8_STR("一般专利"));
                int choice = wxGetSingleChoiceIndex(UTF8_STR("选择专利等级:"),
                                                    UTF8_STR("设置等级"), levels, this);
                if (choice >= 0) {
                    int id = static_cast<int>(patent_list->GetItemData(idx));
                    Patent p = db->GetPatentById(id);
                    p.patent_level = ToStd(levels[choice]);
                    db->UpdatePatent(id, p);
                    LoadPatents();
                }
            }, ID_SET_LEVEL);
            menu.Bind(wxEVT_MENU, [this, idx](wxCommandEvent&) {
                if (idx >= 0 && wxClipboard::Get()->Open()) {
                    wxClipboard::Get()->SetData(
                        new wxTextDataObject(patent_list->GetItemText(idx, 0)));
                    wxClipboard::Get()->Close();
                }
            }, ID_COPY_CODE);
            menu.Bind(wxEVT_MENU, [this, idx](wxCommandEvent&) {
                if (idx >= 0 && wxClipboard::Get()->Open()) {
                    wxClipboard::Get()->SetData(
                        new wxTextDataObject(patent_list->GetItemText(idx, 3)));
                    wxClipboard::Get()->Close();
                }
            }, ID_COPY_TITLE);
            PopupMenu(&menu);
        });

        wxPanel* detail_panel = new wxPanel(splitter);
        wxBoxSizer* detail_sizer = new wxBoxSizer(wxVERTICAL);
        detail_sizer->Add(new wxStaticText(detail_panel, wxID_ANY, UTF8_STR("专利详情")),
                          0, wxBOTTOM, 5);
        patent_detail = new wxTextCtrl(detail_panel, wxID_ANY, "", wxDefaultPosition,
                                       wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH);
        patent_detail->SetFont(wxFont(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
        detail_sizer->Add(patent_detail, 1, wxEXPAND);
        detail_panel->SetSizer(detail_sizer);

        splitter->SplitHorizontally(patent_list, detail_panel, 500);
        sizer->Add(splitter, 1, wxEXPAND | wxALL, 5);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, LANG_STR("Domestic Patents", "国内专利"));
    }

    void ShowColumnFilterPopup(wxListCtrl* list, int col) {
        static const std::vector<std::string> patent_cols = {
            "geke_code", "application_number", "proposal_name", "title",
            "technology_route", "tags", "original_applicant", "current_applicant",
            "patent_type", "patent_level", "application_status", "class_level1",
            "class_level2", "class_level3", "geke_handler", "inventor",
            "application_date", "authorization_date", "expiration_date",
            "agency_firm", "notes"};
        if (col < 0 || col >= (int)patent_cols.size() || GetCurrentTab() != 0) return;

        wxMenu menu;
        menu.Append(10000, UTF8_STR("(全部)"));
        menu.AppendSeparator();
        auto values = db->GetDistinctValues("patents", patent_cols[col]);
        for (size_t i = 0; i < values.size() && i < 30; i++) {
            if (!values[i].empty()) menu.Append(10001 + i, DB_STR(values[i]));
        }
        menu.Bind(wxEVT_MENU, [this, col, values](wxCommandEvent& ev) {
            int id = ev.GetId();
            if (id == 10000) g_column_filters[patent_list][col] = "";
            else {
                size_t idx = id - 10001;
                if (idx < values.size()) g_column_filters[patent_list][col] = values[idx];
            }
            LoadPatents();
        });
        PopupMenu(&menu);
    }

    // Shared: build the QueryFilter from the common toolbar controls.
    QueryFilter CurrentQueryFilter() const {
        QueryFilter f;
        auto clean = [&](const wxString& s) -> std::string {
            if (s.IsEmpty() || s == "All" || s == UTF8_STR("全部")) return "";
            return ToStd(s);
        };
        f.keyword = ToStd(common_search->GetValue());
        f.status = clean(common_status_filter->GetValue());
        f.handler = clean(common_handler_filter->GetValue());
        f.level = clean(common_level_filter->GetValue());
        return f;
    }

    void LoadPatents() {
        if (!db || !db->IsOpen()) return;
        patent_list->DeleteAllItems();

        QueryFilter f = CurrentQueryFilter();
        // Column-header filters
        const auto& col_filters = g_column_filters[patent_list];
        auto patents = db->GetPatents(f);

        int row = 0;
        for (const auto& p : patents) {
            if (!col_filters.empty()) {
                static const std::vector<std::string> cols = {
                    "geke_code", "application_number", "proposal_name", "title",
                    "technology_route", "tags", "original_applicant", "current_applicant",
                    "patent_type", "patent_level", "application_status", "class_level1",
                    "class_level2", "class_level3", "geke_handler", "inventor",
                    "application_date", "authorization_date", "expiration_date",
                    "agency_firm", "notes"};
                if (!PassesColumnFilters(p, col_filters, cols)) continue;
            }
            if (inventor_filter_ && inventor_filter_->GetValue() != UTF8_STR("全部")) {
                if (p.inventor.find(ToStd(inventor_filter_->GetValue())) ==
                    std::string::npos) {
                    continue;
                }
            }

            long idx = patent_list->InsertItem(row, DB_STR(p.geke_code));
            patent_list->SetItem(idx, 1, DB_STR(p.application_number));
            patent_list->SetItem(idx, 2, DB_STR(p.proposal_name));
            patent_list->SetItem(idx, 3, DB_STR(p.title));
            patent_list->SetItem(idx, 4, DB_STR(p.technology_route));
            patent_list->SetItem(idx, 5, DB_STR(p.tags));
            patent_list->SetItem(idx, 6, DB_STR(p.original_applicant));
            patent_list->SetItem(idx, 7, DB_STR(p.current_applicant));
            patent_list->SetItem(idx, 8, DB_STR(p.patent_type));
            patent_list->SetItem(idx, 9, DB_STR(p.patent_level));
            patent_list->SetItem(idx, 10, DB_STR(p.application_status));
            patent_list->SetItem(idx, 11, DB_STR(p.class_level1));
            patent_list->SetItem(idx, 12, DB_STR(p.class_level2));
            patent_list->SetItem(idx, 13, DB_STR(p.class_level3));
            patent_list->SetItem(idx, 14, DB_STR(p.geke_handler));
            patent_list->SetItem(idx, 15, DB_STR(p.inventor));
            patent_list->SetItem(idx, 16, DB_STR(p.application_date));
            patent_list->SetItem(idx, 17, DB_STR(p.authorization_date));
            patent_list->SetItem(idx, 18, DB_STR(p.expiration_date));
            patent_list->SetItem(idx, 19, DB_STR(p.agency_firm));
            patent_list->SetItem(idx, 20, DB_STR(p.notes));
            patent_list->SetItemData(idx, p.id);

            if (p.patent_level == "core" || p.patent_level.find("核心") != std::string::npos) {
                patent_list->SetItemBackgroundColour(idx, wxColour(255, 100, 100));
            } else if (p.patent_level == "important" || p.patent_level.find("重要") != std::string::npos) {
                patent_list->SetItemBackgroundColour(idx, wxColour(255, 230, 100));
            }
            row++;
        }
        sort_state[patent_list] = {0, true};
        SortListCtrl(patent_list, 0, true);
        status_bar->SetStatusText(wxString::Format("Domestic Patents: %d records", row));
    }

    static bool PassesColumnFilters(
            const Patent& p,
            const std::map<int, std::string>& col_filters,
            const std::vector<std::string>& cols) {
        for (const auto& [col, value] : col_filters) {
            if (value.empty() || col >= (int)cols.size()) continue;
            const std::string& name = cols[col];
            std::string actual;
            if (name == "geke_code") actual = p.geke_code;
            else if (name == "application_number") actual = p.application_number;
            else if (name == "proposal_name") actual = p.proposal_name;
            else if (name == "title") actual = p.title;
            else if (name == "technology_route") actual = p.technology_route;
            else if (name == "tags") actual = p.tags;
            else if (name == "original_applicant") actual = p.original_applicant;
            else if (name == "current_applicant") actual = p.current_applicant;
            else if (name == "patent_type") actual = p.patent_type;
            else if (name == "patent_level") actual = p.patent_level;
            else if (name == "application_status") actual = p.application_status;
            else if (name == "class_level1") actual = p.class_level1;
            else if (name == "class_level2") actual = p.class_level2;
            else if (name == "class_level3") actual = p.class_level3;
            else if (name == "geke_handler") actual = p.geke_handler;
            else if (name == "inventor") actual = p.inventor;
            else if (name == "application_date") actual = p.application_date;
            else if (name == "authorization_date") actual = p.authorization_date;
            else if (name == "expiration_date") actual = p.expiration_date;
            else if (name == "agency_firm") actual = p.agency_firm;
            else if (name == "notes") actual = p.notes;
            if (actual != value) return false;
        }
        return true;
    }

    void OnPatentSelected(wxListEvent& event) {
        long idx = event.GetIndex();
        int id = static_cast<int>(patent_list->GetItemData(idx));
        Patent p = db->GetPatentById(id);
        auto oas = db->GetOAByPatent(p.geke_code);

        wxString oa_info;
        for (const auto& oa : oas) {
            oa_info += "\n  [" + DB_STR(oa.oa_type) + "] " + DB_STR(oa.progress) +
                       UTF8_STR(" - 期限: ") + DB_STR(oa.official_deadline) + " - " +
                       DB_STR(oa.handler) + (oa.is_completed ? UTF8_STR(" [已完成]") : "");
        }

        wxString info;
        info << UTF8_STR("编号: ") << DB_STR(p.geke_code) << "\n"
             << UTF8_STR("申请号: ") << DB_STR(p.application_number) << "\n"
             << UTF8_STR("提案名称: ") << DB_STR(p.proposal_name) << "\n"
             << UTF8_STR("发明名称: ") << DB_STR(p.title) << "\n\n"
             << DB_STR(p.patent_type) << " | " << DB_STR(p.patent_level) << " | "
             << DB_STR(p.application_status) << "\n\n"
             << UTF8_STR("处理人: ") << DB_STR(p.geke_handler) << " | "
             << UTF8_STR("发明人: ") << DB_STR(p.inventor) << "\n"
             << UTF8_STR("研发部门: ") << DB_STR(p.rd_department) << " | "
             << UTF8_STR("事务所: ") << DB_STR(p.agency_firm)
             << (p.agent_name.empty() ? "" : " / " + DB_STR(p.agent_name)) << "\n\n"
             << UTF8_STR("申请日: ") << DB_STR(p.application_date) << "\n"
             << UTF8_STR("授权日: ") << DB_STR(p.authorization_date) << "\n"
             << UTF8_STR("到期日: ") << DB_STR(p.expiration_date) << "\n\n"
             << UTF8_STR("分类: ") << DB_STR(p.class_level1) << " / " << DB_STR(p.class_level2)
             << " / " << DB_STR(p.class_level3)
             << (p.class_level4.empty() ? "" : " / " + DB_STR(p.class_level4)) << "\n"
             << UTF8_STR("技术路线: ") << DB_STR(p.technology_route)
             << UTF8_STR(" | 研发项目: ") << DB_STR(p.rd_project) << "\n"
             << UTF8_STR("标签: ") << DB_STR(p.tags) << "\n\n"
             << UTF8_STR("备注: ") << (p.notes.empty() ? UTF8_STR("无") : DB_STR(p.notes)) << "\n\n"
             << "---------------------------------\n"
             << UTF8_STR("OA记录: ") << (oa_info.IsEmpty() ? UTF8_STR("无") : oa_info);
        patent_detail->SetValue(info);
    }

    void BatchSetLevel() {
        std::vector<long> selected = SelectedRows(patent_list);
        if (selected.empty()) { wxMessageBox("Select patents first", "Info", wxOK); return; }
        wxArrayString levels;
        levels.Add("core"); levels.Add("important"); levels.Add("normal");
        int choice = wxGetSingleChoiceIndex("Select level:", "Batch Set Level", levels, this);
        if (choice >= 0) {
            db->BeginBatch();
            for (long i : selected) {
                int id = static_cast<int>(patent_list->GetItemData(i));
                Patent p = db->GetPatentById(id);
                p.patent_level = ToStd(levels[choice]);
                db->UpdatePatent(id, p);
            }
            LoadPatents();
        }
    }

    void BatchSetStatus() {
        std::vector<long> selected = SelectedRows(patent_list);
        if (selected.empty()) { wxMessageBox("Select patents first", "Info", wxOK); return; }
        wxArrayString statuses;
        statuses.Add("pending"); statuses.Add("granted"); statuses.Add("rejected");
        int choice = wxGetSingleChoiceIndex("Select status:", "Batch Set Status", statuses, this);
        if (choice >= 0) {
            db->BeginBatch();
            for (long i : selected) {
                int id = static_cast<int>(patent_list->GetItemData(i));
                Patent p = db->GetPatentById(id);
                p.application_status = ToStd(statuses[choice]);
                db->UpdatePatent(id, p);
            }
            LoadPatents();
        }
    }

    void CalcExpiration() {
        auto patents = db->GetPatents();
        int updated = 0;
        for (const auto& p : patents) {
            if (p.application_status == "granted" && !p.application_date.empty() &&
                p.expiration_date.empty()) {
                Patent up = p;
                int years = (p.patent_type == "invention") ? 20 : 10;
                int app_year = atoi(p.application_date.substr(0, 4).c_str());
                up.expiration_date = ToStd(wxString::Format("%d-%s", app_year + years,
                                                      p.application_date.substr(5)));
                db->UpdatePatent(p.id, up);
                updated++;
            }
        }
        LoadPatents();
        wxMessageBox(wxString::Format("Calculated %d expiration dates\nInvention: 20y, Utility/Design: 10y", updated), "Done", wxOK);
    }

    void ExportReport() {
        wxFileDialog dlg(this, "Export Report", "", "patent_report.txt",
                         "Text files (*.txt)|*.txt|CSV (*.csv)|*.csv", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK) return;
        auto patents = db->GetPatents();
        wxTextFile file(dlg.GetPath());
        file.Create();
        file.AddLine("=== Patent Management Report ===");
        file.AddLine(wxString::Format("Generated: %s (patX v%s)", wxDateTime::Now().FormatDate(), PATX_VERSION));
        file.AddLine(wxString::Format("Total Patents: %zu", patents.size()));
        file.AddLine("");
        int pending = 0, granted = 0, rejected = 0, core = 0;
        for (const auto& p : patents) {
            if (p.application_status == "pending" || p.application_status.find("审查") != std::string::npos) pending++;
            else if (p.application_status == "granted" || p.application_status.find("授权") != std::string::npos) granted++;
            else if (p.application_status == "rejected" || p.application_status.find("驳回") != std::string::npos) rejected++;
            if (p.patent_level == "core" || p.patent_level.find("核心") != std::string::npos) core++;
        }
        file.AddLine("--- Summary ---");
        file.AddLine(wxString::Format("Pending: %d", pending));
        file.AddLine(wxString::Format("Granted: %d", granted));
        file.AddLine(wxString::Format("Rejected: %d", rejected));
        file.AddLine(wxString::Format("Core Patents: %d", core));
        file.AddLine("");
        file.AddLine("--- Patent List ---");
        for (const auto& p : patents) {
            file.AddLine(DB_STR(p.geke_code) + " | " + DB_STR(p.title) + " | " +
                         DB_STR(p.patent_type) + " | " + DB_STR(p.patent_level) + " | " +
                         DB_STR(p.application_status));
        }
        file.Write();
        file.Close();
        wxMessageBox("Report exported!", "Done", wxOK | wxICON_INFORMATION);
    }

    // ============== Common Tab Actions ==============
    int GetCurrentTab() const { return notebook->GetSelection(); }

    wxListCtrl* GetCurrentList() {
        std::vector<wxListCtrl*> lists = {patent_list, oa_list, pct_list, sw_list,
                                          ic_list, foreign_list, nullptr /*US*/,
                                          fee_list, rule_list};
        int tab = GetCurrentTab();
        if (tab >= 0 && tab < (int)lists.size()) return lists[tab];
        return nullptr;
    }

    static std::vector<long> SelectedRows(wxListCtrl* list) {
        std::vector<long> selected;
        long idx = -1;
        while ((idx = list->GetNextItem(idx, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) >= 0) {
            selected.push_back(idx);
        }
        return selected;
    }

    wxString AllFilterLabel() const { return LANG_STR("All", "全部"); }

    bool IsAllFilterValue(const wxString& value) const {
        return value.IsEmpty() || value == "All" || value == UTF8_STR("全部");
    }

    void SetComboValueIfPresent(wxComboBox* combo, const wxString& wanted) {
        wxString all = AllFilterLabel();
        if (!combo || IsAllFilterValue(wanted)) {
            if (combo) combo->SetValue(all);
            return;
        }
        for (unsigned int i = 0; i < combo->GetCount(); ++i) {
            if (combo->GetString(i) == wanted) {
                combo->SetValue(wanted);
                return;
            }
        }
        combo->SetValue(all);
    }

    void GetTabFilterSources(int tab, std::string& status_table, std::string& status_col,
                             std::string& handler_table, std::string& handler_col) {
        switch (tab) {
            case 0: status_table = "patents"; status_col = "application_status";
                    handler_table = "patents"; handler_col = "geke_handler"; break;
            case 1: status_table = "oa_records"; status_col = "oa_type";
                    handler_table = "oa_records"; handler_col = "handler"; break;
            case 2: status_table = "pct_patents"; status_col = "application_status";
                    handler_table = "pct_patents"; handler_col = "handler"; break;
            case 3: status_table = "software_copyrights"; status_col = "application_status";
                    handler_table = "software_copyrights"; handler_col = "handler"; break;
            case 4: status_table = "ic_layouts"; status_col = "application_status";
                    handler_table = "ic_layouts"; handler_col = "handler"; break;
            case 5: status_table = "foreign_patents"; status_col = "patent_status";
                    handler_table = "foreign_patents"; handler_col = "handler"; break;
            default:
                status_table = "patents"; status_col = "application_status";
                handler_table = "patents"; handler_col = "geke_handler"; break;
        }
    }

    void RefreshCommonFiltersForTab(int tab, bool preserve_values) {
        if (!common_status_filter || !common_handler_filter || !common_level_filter) return;
        if (tab == 6) {   // US Prosecution tab has its own filters
            lbl_level->Show(false);
            common_level_filter->Show(false);
            return;
        }

        wxString old_status = preserve_values ? common_status_filter->GetValue() : AllFilterLabel();
        wxString old_handler = preserve_values ? common_handler_filter->GetValue() : AllFilterLabel();
        wxString old_level = preserve_values ? common_level_filter->GetValue() : AllFilterLabel();

        std::string status_table, status_col, handler_table, handler_col;
        GetTabFilterSources(tab, status_table, status_col, handler_table, handler_col);

        common_status_filter->Clear();
        common_status_filter->Append(AllFilterLabel());
        for (const auto& s : db->GetDistinctValues(status_table, status_col)) {
            if (!s.empty()) common_status_filter->Append(DB_STR(s));
        }
        SetComboValueIfPresent(common_status_filter, old_status);

        common_handler_filter->Clear();
        common_handler_filter->Append(AllFilterLabel());
        for (const auto& h : db->GetDistinctValues(handler_table, handler_col)) {
            if (!h.empty()) common_handler_filter->Append(DB_STR(h));
        }
        SetComboValueIfPresent(common_handler_filter, old_handler);

        common_level_filter->Clear();
        common_level_filter->Append(AllFilterLabel());
        if (tab == 0) {
            common_level_filter->Append(LANG_STR("Core", "核心"));
            common_level_filter->Append(LANG_STR("Important", "重要"));
            common_level_filter->Append(LANG_STR("Normal", "一般"));
        }
        SetComboValueIfPresent(common_level_filter, old_level);
        lbl_level->Show(tab == 0);
        common_level_filter->Show(tab == 0);
    }

    // Every tab's New button now opens the real dialog for that module.
    void OnNewByCurrentTab(wxCommandEvent&) {
        switch (GetCurrentTab()) {
            case 0: { PatentEditDialog dlg(this, db.get()); if (dlg.ShowModal() == wxID_OK) LoadPatents(); break; }
            case 1: { OAEditDialog dlg(this, db.get()); if (dlg.ShowModal() == wxID_OK) LoadOA(); break; }
            case 2: { PCTEditDialog dlg(this, db.get()); if (dlg.ShowModal() == wxID_OK) LoadPCT(); break; }
            case 3: { SoftwareEditDialog dlg(this, db.get()); if (dlg.ShowModal() == wxID_OK) LoadSoftware(); break; }
            case 4: { ICEditDialog dlg(this, db.get()); if (dlg.ShowModal() == wxID_OK) LoadIC(); break; }
            case 5: { ForeignEditDialog dlg(this, db.get()); if (dlg.ShowModal() == wxID_OK) LoadForeign(); break; }
            default: break;
        }
    }

    void OnEditByCurrentTab(wxCommandEvent&) {
        int tab = GetCurrentTab();
        wxListCtrl* list = GetCurrentList();
        if (!list) {
            if (tab == 6) return;   // US tab edits inside its own pages
            return;
        }
        long idx = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (idx < 0) {
            wxMessageBox(LANG_STR("Please select an item", "请先选择一条记录"), "Info", wxOK);
            return;
        }
        int id = static_cast<int>(list->GetItemData(idx));
        switch (tab) {
            case 0: { PatentEditDialog dlg(this, db.get(), id); if (dlg.ShowModal() == wxID_OK) LoadPatents(); break; }
            case 1: { OAEditDialog dlg(this, db.get(), id); if (dlg.ShowModal() == wxID_OK) LoadOA(); break; }
            case 2: { PCTEditDialog dlg(this, db.get(), id); if (dlg.ShowModal() == wxID_OK) LoadPCT(); break; }
            case 3: { SoftwareEditDialog dlg(this, db.get(), id); if (dlg.ShowModal() == wxID_OK) LoadSoftware(); break; }
            case 4: { ICEditDialog dlg(this, db.get(), id); if (dlg.ShowModal() == wxID_OK) LoadIC(); break; }
            case 5: { ForeignEditDialog dlg(this, db.get(), id); if (dlg.ShowModal() == wxID_OK) LoadForeign(); break; }
        }
    }

    void OnDeleteByCurrentTab(wxCommandEvent&) {
        int tab = GetCurrentTab();
        if (tab == 6) return;   // US tab deletion via its own controls
        wxListCtrl* list = GetCurrentList();
        if (!list) return;

        std::vector<long> selected = SelectedRows(list);
        if (selected.empty()) {
            wxMessageBox(LANG_STR("Please select item(s) to delete", "请先选择要删除的记录"), "Info", wxOK);
            return;
        }
        if (wxMessageBox(wxString::Format("Delete %zu item(s)?", selected.size()),
                         LANG_STR("Confirm", "确认"), wxYES_NO) != wxYES)
            return;

        db->BeginBatch();
        for (long i : selected) {
            int id = static_cast<int>(list->GetItemData(i));
            switch (tab) {
                case 0: db->DeletePatent(id); break;
                case 1: db->DeleteOA(id); break;
                case 2: db->DeletePCT(id); break;
                case 3: db->DeleteSoftware(id); break;
                case 4: db->DeleteIC(id); break;
                case 5: db->DeleteForeign(id); break;
            }
        }
        switch (tab) {
            case 0: LoadPatents(); break;
            case 1: LoadOA(); break;
            case 2: LoadPCT(); break;
            case 3: LoadSoftware(); break;
            case 4: LoadIC(); break;
            case 5: LoadForeign(); break;
        }
    }

    void OnSearchByCurrentTab(wxCommandEvent&) {
        switch (GetCurrentTab()) {
            case 0: LoadPatents(); break;
            case 1: LoadOA(); break;
            case 2: LoadPCT(); break;
            case 3: LoadSoftware(); break;
            case 4: LoadIC(); break;
            case 5: LoadForeign(); break;
        }
    }

    void OnFilterByCurrentTab(wxCommandEvent&) {
        switch (GetCurrentTab()) {
            case 0: LoadPatents(); break;
            case 1: LoadOA(); break;
            case 2: LoadPCT(); break;
            case 3: LoadSoftware(); break;
            case 4: LoadIC(); break;
            case 5: LoadForeign(); break;
        }
    }

    void OnValidateData() {
        int issues = 0;
        wxString report;
        for (const auto& p : db->GetPatents()) {
            if (p.geke_code.empty()) {
                report += UTF8_STR("专利ID ") + std::to_string(p.id) + UTF8_STR(" 编号为空\n");
                issues++;
            }
            if (p.title.empty()) {
                report += UTF8_STR("专利 ") + DB_STR(p.geke_code) + UTF8_STR(" 名称为空\n");
                issues++;
            }
        }
        for (const auto& oa : db->GetOARecords()) {
            if (oa.official_deadline.empty() && !oa.is_completed) {
                report += UTF8_STR("OA缺少绝限日期: ") + DB_STR(oa.geke_code) + " - " +
                          DB_STR(oa.oa_type) + "\n";
                issues++;
            }
        }
        if (issues == 0) {
            wxMessageBox(current_lang == 0 ? "All data validated successfully!\nNo issues found."
                                           : UTF8_STR("数据验证成功！\n未发现问题。"),
                         current_lang == 0 ? "Data Validation" : UTF8_STR("数据验证"),
                         wxOK | wxICON_INFORMATION);
        } else {
            wxMessageBox(report, wxString::Format(current_lang == 0 ? "Found %d issues:"
                                            : UTF8_STR("发现 %d 个问题:"), issues),
                         wxOK | wxICON_WARNING);
        }
    }

    void OnStatistics(wxCommandEvent&) {
        auto patents = db->GetPatents();
        auto oas = db->GetOARecords();
        std::map<std::string, int> status_map, type_map, level_map;
        for (const auto& p : patents) {
            status_map[p.application_status.empty() ? "unknown" : p.application_status]++;
            type_map[p.patent_type.empty() ? "unknown" : p.patent_type]++;
            level_map[p.patent_level.empty() ? "unknown" : p.patent_level]++;
        }
        int pending_oa = 0, completed_oa = 0;
        for (const auto& oa : oas) {
            if (oa.is_completed) completed_oa++;
            else pending_oa++;
        }

        wxString msg;
        msg += LANG_STR("=== Patent Statistics ===\n\n", "=== 专利统计 ===\n\n");
        msg += wxString::Format(LANG_STR("Total Patents: %zu\n\n", "国内专利总数: %zu\n\n"), patents.size());
        msg += LANG_STR("--- By Status ---\n", "--- 按状态 ---\n");
        for (const auto& [k, v] : status_map) msg += wxString::Format("  %s: %d\n", DB_STR(k), v);
        msg += "\n" + LANG_STR("--- By Type ---\n", "--- 按类型 ---\n");
        for (const auto& [k, v] : type_map) msg += wxString::Format("  %s: %d\n", DB_STR(k), v);
        msg += "\n" + LANG_STR("--- By Level ---\n", "--- 按等级 ---\n");
        for (const auto& [k, v] : level_map) msg += wxString::Format("  %s: %d\n", DB_STR(k), v);
        msg += "\n" + LANG_STR("--- OA Records ---\n", "--- OA记录 ---\n");
        msg += wxString::Format(LANG_STR("  Pending: %d\n  Completed: %d\n",
                                          "  待处理: %d\n  已完成: %d\n"), pending_oa, completed_oa);
        wxMessageBox(msg, LANG_STR("Statistics", "统计"), wxOK | wxICON_INFORMATION);
    }

    void OnBatchByCurrentTab(wxCommandEvent&) {
        if (GetCurrentTab() == 0) BatchSetStatus();
        else wxMessageBox(LANG_STR("Batch operation not supported for this tab yet",
                                   "此页面暂不支持批量操作"), "Info", wxOK);
    }

    // ============== OA Tab ==============
    void SetupOATab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* tb = new wxBoxSizer(wxHORIZONTAL);
        tb->Add(new wxStaticText(panel, wxID_ANY, UTF8_STR("筛选:")),
                0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        oa_filter = new wxComboBox(panel, wxID_ANY, UTF8_STR("全部未完成"),
                                   wxDefaultPosition, wxSize(150, -1));
        oa_filter->Append(UTF8_STR("全部未完成"));
        oa_filter->Append(UTF8_STR("5天内到期"));
        oa_filter->Append(UTF8_STR("30天内到期"));
        oa_filter->Append(UTF8_STR("全部"));
        oa_filter->Append(UTF8_STR("已完成"));
        oa_filter->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { LoadOA(); });
        tb->Add(oa_filter, 0, wxRIGHT, 15);
        tb->AddStretchSpacer();

        auto oa_btn = [&](const wxString& label, auto&& fn) {
            wxButton* btn = new wxButton(panel, wxID_ANY, label);
            btn->Bind(wxEVT_BUTTON, fn);
            tb->Add(btn, 0, wxRIGHT, 5);
        };
        oa_btn("Mark Complete", [this](wxCommandEvent&) {
            long idx = oa_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (idx >= 0) {
                db->MarkOACompleted(static_cast<int>(oa_list->GetItemData(idx)));
                LoadOA();
            }
        });
        oa_btn("Show Urgent", [this](wxCommandEvent&) {
            oa_filter->SetValue(UTF8_STR("5天内到期"));
            LoadOA();
                        QueryFilter urgent_filter;
            urgent_filter.deadline_state = "due5";
            auto records = db->GetOARecords(urgent_filter);
            int urgent = 0;
            for (const auto& oa : records) if (!oa.is_completed) urgent++;
            wxMessageBox(urgent > 0
                ? wxString::Format("WARNING: %d OA records need urgent attention!", urgent)
                : wxString("No urgent OA records. Good job!"),
                "Status", wxOK | (urgent ? wxICON_WARNING : wxICON_INFORMATION));
        });
        // 查询选中案件的最新审查意见：CN 案走 CNIPA 网页同步，结果按保护
        // 规则写入 OA 列表（新 OA 新增、空日期补入、日期冲突只标记）。
        oa_btn(LANG_STR("Check Latest OA", "查询最新审查意见"), [this](wxCommandEvent&) {
            std::vector<int> oa_ids;
            long idx = -1;
            while ((idx = oa_list->GetNextItem(idx, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) >= 0)
                oa_ids.push_back(static_cast<int>(oa_list->GetItemData(idx)));
            if (oa_ids.empty()) {
                wxMessageBox(UTF8_STR("请先在列表中选择要查询的 OA 记录（可多选）"),
                             UTF8_STR("查询最新审查意见"), wxOK | wxICON_INFORMATION);
                return;
            }
            auto join = [](const std::vector<std::string>& v) {
                wxString out;
                for (const auto& s : v) {
                    if (!out.empty()) out += ", ";
                    out += wxString::FromUTF8(s.c_str());
                }
                return out;
            };
            std::vector<Patent> targets;
            std::vector<std::string> us_linked, no_number;
            std::set<std::string> seen;
            for (int id : oa_ids) {
                OARecord oa = db->GetOAById(id);
                if (oa.id == 0) continue;
                if (oa.jurisdiction == "US" || oa.source == "USPTO") {
                    us_linked.push_back(oa.geke_code);
                    continue;
                }
                if (!seen.insert(oa.geke_code).second) continue;
                Patent p = db->GetPatentByCode(oa.geke_code);
                if (p.id == 0) continue;
                if (p.application_number.empty() && p.publication_number.empty()) {
                    no_number.push_back(oa.geke_code);
                    continue;
                }
                targets.push_back(p);
            }
            if (!targets.empty()) {
                dossier_controller->SyncPatents(targets);
            } else {
                wxString msg = UTF8_STR("所选记录没有可自动查询的 CN 案件。");
                if (!us_linked.empty())
                    msg += UTF8_STR("\nUS 来源记录（USPTO 同步已停用）跳过：") + join(us_linked);
                if (!no_number.empty())
                    msg += UTF8_STR("\n缺少申请号和公开号，无法查询：") + join(no_number);
                wxMessageBox(msg, UTF8_STR("查询最新审查意见"), wxOK | wxICON_INFORMATION);
            }
        });
        sizer->Add(tb, 0, wxALL, 5);

        oa_list = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        oa_list->AppendColumn(UTF8_STR("编号"), wxLIST_FORMAT_LEFT, 90);
        oa_list->AppendColumn(UTF8_STR("发明名称"), wxLIST_FORMAT_LEFT, 220);
        oa_list->AppendColumn(UTF8_STR("OA类型"), wxLIST_FORMAT_LEFT, 110);
        oa_list->AppendColumn(UTF8_STR("发文日"), wxLIST_FORMAT_LEFT, 90);
        oa_list->AppendColumn(UTF8_STR("截止日"), wxLIST_FORMAT_LEFT, 100);
        oa_list->AppendColumn(UTF8_STR("剩余天数"), wxLIST_FORMAT_LEFT, 80);
        oa_list->AppendColumn(UTF8_STR("处理人"), wxLIST_FORMAT_LEFT, 80);
        oa_list->AppendColumn(UTF8_STR("撰写人"), wxLIST_FORMAT_LEFT, 80);
        oa_list->AppendColumn(UTF8_STR("进度"), wxLIST_FORMAT_LEFT, 100);
        oa_list->AppendColumn(UTF8_STR("等级"), wxLIST_FORMAT_LEFT, 80);
        oa_list->AppendColumn(UTF8_STR("来源"), wxLIST_FORMAT_LEFT, 70);

        oa_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
            long idx = oa_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (idx >= 0) {
                OAEditDialog dlg(this, db.get(), static_cast<int>(oa_list->GetItemData(idx)));
                if (dlg.ShowModal() == wxID_OK) LoadOA();
            }
        });
        oa_list->Bind(wxEVT_LIST_COL_CLICK, [this](wxListEvent& e) {
            int col = e.GetColumn();
            auto& state = sort_state[oa_list];
            if (state.first == col) state.second = !state.second;
            else { state.first = col; state.second = true; }
            SortListCtrl(oa_list, col, state.second);
        });

        sizer->Add(oa_list, 1, wxEXPAND | wxALL, 5);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, LANG_STR("OA Processing", "OA处理"));
    }

    void LoadOA() {
        if (!db || !db->IsOpen()) return;
        oa_list->DeleteAllItems();

        QueryFilter f = CurrentQueryFilter();
        // OA-type filter doubles as the status filter column on this tab
        if (!f.status.empty()) f.oa_type = f.status;
        f.status.clear();

        std::string selected = ToStd(oa_filter->GetValue());
        if (selected == ToStd(UTF8_STR("全部未完成"))) f.deadline_state = "incomplete";
        else if (selected == ToStd(UTF8_STR("5天内到期"))) f.deadline_state = "due5";
        else if (selected == ToStd(UTF8_STR("30天内到期"))) f.deadline_state = "due30";
        else if (selected == ToStd(UTF8_STR("已完成"))) f.deadline_state = "completed";

        auto records = db->GetOARecords(f);

        std::map<std::string, std::string> level_map;
        for (const auto& p : db->GetPatents()) level_map[p.geke_code] = p.patent_level;

        wxDateTime now = wxDateTime::Now();
        int urgent_count = 0, row = 0;
        for (const auto& oa : records) {
            long idx = oa_list->InsertItem(row, DB_STR(oa.geke_code));
            oa_list->SetItem(idx, 1, DB_STR(oa.patent_title));
            oa_list->SetItem(idx, 2, DB_STR(oa.oa_type));
            oa_list->SetItem(idx, 3, DB_STR(oa.issue_date));
            oa_list->SetItem(idx, 4, DB_STR(oa.official_deadline));

            wxString days_str = "-";
            wxColour bg_color;
            if (oa.is_completed) {
                days_str = "Completed";
                bg_color = wxColour(200, 255, 200);
            } else if (!oa.official_deadline.empty()) {
                wxDateTime deadline;
                deadline.ParseFormat(oa.official_deadline.c_str(), "%Y-%m-%d");
                if (deadline.IsValid()) {
                    int days = (deadline - now).GetDays();
                    if (days < 0) { days_str = wxString::Format("Overdue %d days", -days); bg_color = wxColour(255, 200, 200); urgent_count++; }
                    else if (days <= 5) { days_str = wxString::Format("%d days", days); bg_color = wxColour(255, 255, 150); urgent_count++; }
                    else if (days <= 10) { days_str = wxString::Format("%d days", days); bg_color = wxColour(255, 230, 200); }
                    else if (days <= 30) { days_str = wxString::Format("%d days", days); bg_color = wxColour(255, 250, 220); }
                    else days_str = wxString::Format("%d days", days);
                }
            }
            oa_list->SetItem(idx, 5, days_str);
            oa_list->SetItem(idx, 6, DB_STR(oa.handler));
            oa_list->SetItem(idx, 7, DB_STR(oa.writer));
            oa_list->SetItem(idx, 8, DB_STR(oa.progress));

            std::string level = level_map.count(oa.geke_code) ? level_map[oa.geke_code] : "";
            oa_list->SetItem(idx, 9, DB_STR(level));
            oa_list->SetItem(idx, 10, oa.source.empty() ? "-" : DB_STR(oa.source));

            if (level == "core" || level.find("核心") != std::string::npos) {
                oa_list->SetItemBackgroundColour(idx, wxColour(255, 100, 100));
            } else if (level == "important" || level.find("重要") != std::string::npos) {
                oa_list->SetItemBackgroundColour(idx, wxColour(255, 230, 100));
            } else if (bg_color.IsOk()) {
                oa_list->SetItemBackgroundColour(idx, bg_color);
            }
            oa_list->SetItemData(idx, oa.id);
            row++;
        }

        status_bar->SetStatusText(urgent_count > 0
            ? wxString::Format("OA Records: %d | URGENT: %d need attention!", row, urgent_count)
            : wxString::Format("OA Records: %d", row));
    }

    // ============== PCT Tab ==============
    void SetupPCTTab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        pct_list = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        pct_list->AppendColumn(UTF8_STR("编号"), wxLIST_FORMAT_LEFT, 90);
        pct_list->AppendColumn(UTF8_STR("国内同源"), wxLIST_FORMAT_LEFT, 90);
        pct_list->AppendColumn(UTF8_STR("PCT申请号"), wxLIST_FORMAT_LEFT, 140);
        pct_list->AppendColumn(UTF8_STR("国家申请号"), wxLIST_FORMAT_LEFT, 140);
        pct_list->AppendColumn(UTF8_STR("发明名称"), wxLIST_FORMAT_LEFT, 280);
        pct_list->AppendColumn(UTF8_STR("状态"), wxLIST_FORMAT_LEFT, 100);
        pct_list->AppendColumn(UTF8_STR("处理人"), wxLIST_FORMAT_LEFT, 80);
        pct_list->Bind(wxEVT_LIST_COL_CLICK, [this](wxListEvent& e) {
            int col = e.GetColumn();
            auto& state = sort_state[pct_list];
            if (state.first == col) state.second = !state.second;
            else { state.first = col; state.second = true; }
            SortListCtrl(pct_list, col, state.second);
        });
        pct_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
            long idx = pct_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (idx >= 0) {
                PCTEditDialog dlg(this, db.get(), static_cast<int>(pct_list->GetItemData(idx)));
                if (dlg.ShowModal() == wxID_OK) LoadPCT();
            }
        });
        sizer->Add(pct_list, 1, wxEXPAND | wxALL, 5);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, LANG_STR("PCT Applications", "PCT申请"));
    }

    void LoadPCT() {
        pct_list->DeleteAllItems();
        QueryFilter f = CurrentQueryFilter();
        auto patents = db->GetPCTPatents(f);
        int row = 0;
        for (const auto& p : patents) {
            long idx = pct_list->InsertItem(row, DB_STR(p.geke_code));
            pct_list->SetItem(idx, 1, DB_STR(p.domestic_source));
            pct_list->SetItem(idx, 2, DB_STR(p.application_no));
            pct_list->SetItem(idx, 3, DB_STR(p.country_app_no));
            pct_list->SetItem(idx, 4, DB_STR(p.title));
            pct_list->SetItem(idx, 5, DB_STR(p.application_status));
            pct_list->SetItem(idx, 6, DB_STR(p.handler));
            pct_list->SetItemData(idx, p.id);
            row++;
        }
        status_bar->SetStatusText(wxString::Format("PCT: %d records", row));
    }

    // ============== Software Tab ==============
    void SetupSoftwareTab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        sw_list = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        sw_list->AppendColumn(UTF8_STR("案号"), wxLIST_FORMAT_LEFT, 90);
        sw_list->AppendColumn(UTF8_STR("登记号"), wxLIST_FORMAT_LEFT, 110);
        sw_list->AppendColumn(UTF8_STR("名称"), wxLIST_FORMAT_LEFT, 280);
        sw_list->AppendColumn(UTF8_STR("权利人"), wxLIST_FORMAT_LEFT, 120);
        sw_list->AppendColumn(UTF8_STR("状态"), wxLIST_FORMAT_LEFT, 100);
        sw_list->AppendColumn(UTF8_STR("处理人"), wxLIST_FORMAT_LEFT, 80);
        sw_list->Bind(wxEVT_LIST_COL_CLICK, [this](wxListEvent& e) {
            int col = e.GetColumn();
            auto& state = sort_state[sw_list];
            if (state.first == col) state.second = !state.second;
            else { state.first = col; state.second = true; }
            SortListCtrl(sw_list, col, state.second);
        });
        sw_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
            long idx = sw_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (idx >= 0) {
                SoftwareEditDialog dlg(this, db.get(), static_cast<int>(sw_list->GetItemData(idx)));
                if (dlg.ShowModal() == wxID_OK) LoadSoftware();
            }
        });
        sizer->Add(sw_list, 1, wxEXPAND | wxALL, 5);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, LANG_STR("Software Copyright", "软件著作权"));
    }

    void LoadSoftware() {
        sw_list->DeleteAllItems();
        QueryFilter f = CurrentQueryFilter();
        auto copyrights = db->GetSoftwareCopyrights(f);
        int row = 0;
        for (const auto& s : copyrights) {
            long idx = sw_list->InsertItem(row, DB_STR(s.case_no));
            sw_list->SetItem(idx, 1, DB_STR(s.reg_no));
            sw_list->SetItem(idx, 2, DB_STR(s.title));
            sw_list->SetItem(idx, 3, DB_STR(s.current_owner));
            sw_list->SetItem(idx, 4, DB_STR(s.application_status));
            sw_list->SetItem(idx, 5, DB_STR(s.handler));
            sw_list->SetItemData(idx, s.id);
            row++;
        }
        status_bar->SetStatusText(wxString::Format("Software: %d records", row));
    }

    // ============== IC Tab ==============
    void SetupICTab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        ic_list = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        ic_list->AppendColumn(UTF8_STR("案号"), wxLIST_FORMAT_LEFT, 90);
        ic_list->AppendColumn(UTF8_STR("登记号"), wxLIST_FORMAT_LEFT, 110);
        ic_list->AppendColumn(UTF8_STR("名称"), wxLIST_FORMAT_LEFT, 280);
        ic_list->AppendColumn(UTF8_STR("权利人"), wxLIST_FORMAT_LEFT, 120);
        ic_list->AppendColumn(UTF8_STR("状态"), wxLIST_FORMAT_LEFT, 100);
        ic_list->AppendColumn(UTF8_STR("设计人"), wxLIST_FORMAT_LEFT, 80);
        ic_list->Bind(wxEVT_LIST_COL_CLICK, [this](wxListEvent& e) {
            int col = e.GetColumn();
            auto& state = sort_state[ic_list];
            if (state.first == col) state.second = !state.second;
            else { state.first = col; state.second = true; }
            SortListCtrl(ic_list, col, state.second);
        });
        ic_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
            long idx = ic_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (idx >= 0) {
                ICEditDialog dlg(this, db.get(), static_cast<int>(ic_list->GetItemData(idx)));
                if (dlg.ShowModal() == wxID_OK) LoadIC();
            }
        });
        sizer->Add(ic_list, 1, wxEXPAND | wxALL, 5);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, LANG_STR("IC Layout", "集成电路布图"));
    }

    void LoadIC() {
        ic_list->DeleteAllItems();
        QueryFilter f = CurrentQueryFilter();
        auto layouts = db->GetICLayouts(f);
        int row = 0;
        for (const auto& ic : layouts) {
            long idx = ic_list->InsertItem(row, DB_STR(ic.case_no));
            ic_list->SetItem(idx, 1, DB_STR(ic.reg_no));
            ic_list->SetItem(idx, 2, DB_STR(ic.title));
            ic_list->SetItem(idx, 3, DB_STR(ic.current_owner));
            ic_list->SetItem(idx, 4, DB_STR(ic.application_status));
            ic_list->SetItem(idx, 5, DB_STR(ic.designer));
            ic_list->SetItemData(idx, ic.id);
            row++;
        }
        status_bar->SetStatusText(wxString::Format("IC Layouts: %d records", row));
    }

    // ============== Foreign Tab ==============
    void SetupForeignTab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        foreign_list = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        foreign_list->AppendColumn(UTF8_STR("案号"), wxLIST_FORMAT_LEFT, 90);
        foreign_list->AppendColumn(UTF8_STR("PCT号"), wxLIST_FORMAT_LEFT, 140);
        foreign_list->AppendColumn(UTF8_STR("国家"), wxLIST_FORMAT_LEFT, 60);
        foreign_list->AppendColumn(UTF8_STR("申请号"), wxLIST_FORMAT_LEFT, 120);
        foreign_list->AppendColumn(UTF8_STR("名称"), wxLIST_FORMAT_LEFT, 280);
        foreign_list->AppendColumn(UTF8_STR("权利人"), wxLIST_FORMAT_LEFT, 120);
        foreign_list->AppendColumn(UTF8_STR("状态"), wxLIST_FORMAT_LEFT, 100);
        foreign_list->AppendColumn(UTF8_STR("处理人"), wxLIST_FORMAT_LEFT, 80);
        foreign_list->Bind(wxEVT_LIST_COL_CLICK, [this](wxListEvent& e) {
            int col = e.GetColumn();
            auto& state = sort_state[foreign_list];
            if (state.first == col) state.second = !state.second;
            else { state.first = col; state.second = true; }
            SortListCtrl(foreign_list, col, state.second);
        });
        foreign_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
            long idx = foreign_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (idx >= 0) {
                ForeignEditDialog dlg(this, db.get(), static_cast<int>(foreign_list->GetItemData(idx)));
                if (dlg.ShowModal() == wxID_OK) LoadForeign();
            }
        });
        sizer->Add(foreign_list, 1, wxEXPAND | wxALL, 5);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, LANG_STR("Foreign Patents", "国外专利"));
    }

    void LoadForeign() {
        foreign_list->DeleteAllItems();
        QueryFilter f = CurrentQueryFilter();
        auto patents = db->GetForeignPatents(f);
        int row = 0;
        for (const auto& fp : patents) {
            long idx = foreign_list->InsertItem(row, DB_STR(fp.case_no));
            foreign_list->SetItem(idx, 1, DB_STR(fp.pct_no));
            foreign_list->SetItem(idx, 2, DB_STR(fp.country));
            foreign_list->SetItem(idx, 3, DB_STR(fp.application_no));
            foreign_list->SetItem(idx, 4, DB_STR(fp.title));
            foreign_list->SetItem(idx, 5, DB_STR(fp.owner));
            foreign_list->SetItem(idx, 6, DB_STR(fp.patent_status));
            foreign_list->SetItem(idx, 7, DB_STR(fp.handler));
            foreign_list->SetItemData(idx, fp.id);
            row++;
        }
        status_bar->SetStatusText(wxString::Format("Foreign: %d records", row));
    }

    // ============== US Prosecution Tab ==============
    void SetupUSTab() {
    }

    // ============== Annual Fee Tab ==============
    void SetupAnnualFeeTab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* tb = new wxBoxSizer(wxHORIZONTAL);
        tb->Add(new wxStaticText(panel, wxID_ANY,
                                 UTF8_STR("年费管理（基于已授权专利与年费规则）")),
                0, wxALIGN_CENTER_VERTICAL);
        tb->AddStretchSpacer();
        wxButton* gen_btn = new wxButton(panel, ID_FEE_GENERATE, UTF8_STR("从专利生成 / Generate"));
        gen_btn->Bind(wxEVT_BUTTON, &PatXFrame::OnGenerateAnnualFees, this, ID_FEE_GENERATE);
        tb->Add(gen_btn, 0, wxRIGHT, 5);
        wxButton* paid_btn = new wxButton(panel, ID_FEE_MARK_PAID, UTF8_STR("标记已缴 / Mark Paid"));
        paid_btn->Bind(wxEVT_BUTTON, &PatXFrame::OnMarkFeePaid, this, ID_FEE_MARK_PAID);
        tb->Add(paid_btn, 0);
        sizer->Add(tb, 0, wxALL, 5);

        fee_list = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        fee_list->AppendColumn(UTF8_STR("编号"), wxLIST_FORMAT_LEFT, 90);
        fee_list->AppendColumn(UTF8_STR("名称"), wxLIST_FORMAT_LEFT, 250);
        fee_list->AppendColumn(UTF8_STR("地区"), wxLIST_FORMAT_LEFT, 50);
        fee_list->AppendColumn(UTF8_STR("年度"), wxLIST_FORMAT_LEFT, 60);
        fee_list->AppendColumn(UTF8_STR("截止日"), wxLIST_FORMAT_LEFT, 100);
        fee_list->AppendColumn(UTF8_STR("金额"), wxLIST_FORMAT_LEFT, 90);
        fee_list->AppendColumn(UTF8_STR("状态"), wxLIST_FORMAT_LEFT, 90);
        fee_list->AppendColumn(UTF8_STR("处理人"), wxLIST_FORMAT_LEFT, 80);
        fee_list->Bind(wxEVT_LIST_COL_CLICK, [this](wxListEvent& e) {
            int col = e.GetColumn();
            auto& state = sort_state[fee_list];
            if (state.first == col) state.second = !state.second;
            else { state.first = col; state.second = true; }
            SortListCtrl(fee_list, col, state.second);
        });
        sizer->Add(fee_list, 1, wxEXPAND | wxALL, 5);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, LANG_STR("Annual Fees", "年费管理"));
    }

    // CN annual fee schedule (official amounts, CNY) by patent type and
    // fee year. Other jurisdictions display "Rule not configured" instead
    // of invented data.
    static const char* CNFeeAmount(bool invention, int year) {
        if (invention) {
            if (year <= 3) return "900";
            if (year <= 6) return "1200";
            if (year <= 9) return "2000";
            if (year <= 12) return "4000";
            if (year <= 15) return "6000";
            return "8000";
        }
        if (year <= 3) return "600";
        if (year <= 5) return "900";
        if (year <= 8) return "1200";
        return "2000";
    }

    void OnGenerateAnnualFees(wxCommandEvent&) {
        // Rebuild fee rows for granted CN patents from real records
        db->Execute("DELETE FROM annual_fees WHERE is_paid = 0");
        int generated = 0;
        for (const auto& p : db->GetPatents()) {
            if (p.application_status != "granted" || p.application_date.empty()) continue;
            int start_year = atoi(p.application_date.substr(0, 4).c_str());
            int end_year = p.expiration_date.empty() ? start_year + 20
                                                     : atoi(p.expiration_date.substr(0, 4).c_str());
            bool invention = p.patent_type == "invention";
            for (int year = 1; year <= end_year - start_year; year++) {
                int due_year = start_year + year - 1;
                std::string due_date = ToStd(wxString::Format("%d%s", due_year,
                                                        p.application_date.substr(4)));
                std::string sql =
                    "INSERT INTO annual_fees (patent_id, geke_code, patent_title, patent_type, "
                    "fee_year, fee_amount, fee_period_end, jurisdiction, application_date, is_paid) VALUES (" +
                    std::to_string(p.id) + ",'" + db->EscapeString(p.geke_code) + "','" +
                    db->EscapeString(p.title) + "','" + db->EscapeString(p.patent_type) + "'," +
                    std::to_string(year) + ",'" + CNFeeAmount(invention, year) + "','" +
                    db->EscapeString(due_date) + "','CN','" + db->EscapeString(p.application_date) + "',0)";
                db->Execute(sql);
                generated++;
            }
        }
        LoadAnnualFees();
        wxMessageBox(wxString::Format(UTF8_STR("已生成 %d 条年费记录（仅中国发明专利/实用新型；其他地区显示 Rule not configured）"),
                                      generated), "Done", wxOK);
    }

    void OnMarkFeePaid(wxCommandEvent&) {
        long idx = fee_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (idx < 0) return;
        int fee_id = static_cast<int>(fee_list->GetItemData(idx));
        db->Execute("UPDATE annual_fees SET is_paid = 1, payment_date = '" +
                    db->GetCurrentDate() + "' WHERE id = " + std::to_string(fee_id));
        LoadAnnualFees();
    }

    void LoadAnnualFees() {
        fee_list->DeleteAllItems();
        sqlite3_stmt* stmt;
        // Rows come from the annual_fees table; when empty the tab shows the
        // generate hint instead of fake "900/pending" numbers.
        if (db->GetHandle() &&
            sqlite3_prepare_v2(db->GetHandle(),
                "SELECT id, geke_code, patent_title, jurisdiction, fee_year, fee_period_end, "
                "fee_amount, is_paid FROM annual_fees ORDER BY fee_period_end ASC",
                -1, &stmt, nullptr) == SQLITE_OK) {
            int row = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                auto col = [&](int i) -> const char* {
                    const char* t = (const char*)sqlite3_column_text(stmt, i);
                    return t ? t : "";
                };
                long idx = fee_list->InsertItem(row, wxString::FromUTF8(col(1)));
                fee_list->SetItem(idx, 1, wxString::FromUTF8(col(2)));
                fee_list->SetItem(idx, 2, col(3));
                fee_list->SetItem(idx, 3, std::to_string(sqlite3_column_int(stmt, 4)));
                fee_list->SetItem(idx, 4, col(5));
                std::string amount = col(6);
                std::string jurisdiction = col(3);
                fee_list->SetItem(idx, 5, amount.empty() && jurisdiction != "CN"
                                              ? "Rule not configured" : amount.c_str());
                bool paid = sqlite3_column_int(stmt, 7) != 0;
                fee_list->SetItem(idx, 6, paid ? UTF8_STR("已缴") : "pending");
                if (paid) fee_list->SetItemBackgroundColour(row, wxColour(200, 255, 200));
                fee_list->SetItemData(idx, sqlite3_column_int(stmt, 0));
                row++;
            }
            sqlite3_finalize(stmt);
        }
        if (fee_list->GetItemCount() == 0) {
            long idx = fee_list->InsertItem(0, UTF8_STR("（空）点击\"从专利生成\"基于真实专利记录生成年费"));
            fee_list->SetItemBackgroundColour(idx, wxColour(245, 245, 220));
        }
    }

    // ============== Deadline Rules Tab ==============
    void SetupDeadlineRulesTab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* tb = new wxBoxSizer(wxHORIZONTAL);
        tb->Add(new wxStaticText(panel, wxID_ANY, UTF8_STR("期限计算规则（数据库存储，网页同步与 OA 期限共用同一规则引擎）")),
                0, wxALIGN_CENTER_VERTICAL);
        tb->AddStretchSpacer();
        auto rule_btn = [&](int id, const wxString& label, void (PatXFrame::*h)(wxCommandEvent&)) {
            wxButton* btn = new wxButton(panel, id, label);
            btn->Bind(wxEVT_BUTTON, h, this);
            tb->Add(btn, 0, wxRIGHT, 5);
            return btn;
        };
        rule_btn(ID_RULE_ADD, "Add", &PatXFrame::OnRuleAdd);
        rule_btn(ID_RULE_EDIT, "Edit", &PatXFrame::OnRuleEdit);
        rule_btn(ID_RULE_DELETE, "Delete", &PatXFrame::OnRuleDelete);
        rule_btn(ID_RULE_TOGGLE, UTF8_STR("启用/禁用"), &PatXFrame::OnRuleToggle);
        sizer->Add(tb, 0, wxALL, 5);

        rule_list = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        rule_list->AppendColumn("Jurisdiction", wxLIST_FORMAT_LEFT, 90);
        rule_list->AppendColumn("Event Type", wxLIST_FORMAT_LEFT, 170);
        rule_list->AppendColumn(UTF8_STR("描述"), wxLIST_FORMAT_LEFT, 220);
        rule_list->AppendColumn(UTF8_STR("月"), wxLIST_FORMAT_LEFT, 50);
        rule_list->AppendColumn(UTF8_STR("天"), wxLIST_FORMAT_LEFT, 50);
        rule_list->AppendColumn(UTF8_STR("可延期"), wxLIST_FORMAT_LEFT, 60);
        rule_list->AppendColumn(UTF8_STR("最长延期"), wxLIST_FORMAT_LEFT, 70);
        rule_list->AppendColumn(UTF8_STR("启用"), wxLIST_FORMAT_LEFT, 50);
        rule_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, &PatXFrame::OnRuleEdit, this);
        sizer->Add(rule_list, 1, wxEXPAND | wxALL, 5);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, LANG_STR("Deadline Rules", "期限规则"));
    }

    int SelectedRuleId() {
        long idx = rule_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (idx < 0) return 0;
        return static_cast<int>(rule_list->GetItemData(idx));
    }

    void OnRuleAdd(wxCommandEvent&) {
        DeadlineRuleEditDialog dlg(this, db.get());
        if (dlg.ShowModal() == wxID_OK) LoadDeadlineRules();
    }

    void OnRuleEdit(wxCommandEvent&) {
        int id = SelectedRuleId();
        if (!id) return;
        DeadlineRuleEditDialog dlg(this, db.get(), id);
        if (dlg.ShowModal() == wxID_OK) LoadDeadlineRules();
    }

    void OnRuleDelete(wxCommandEvent&) {
        int id = SelectedRuleId();
        if (!id) return;
        if (wxMessageBox("Delete this rule?", "Confirm", wxYES_NO) == wxYES) {
            db->DeleteDeadlineRule(id);
            LoadDeadlineRules();
        }
    }

    void OnRuleToggle(wxCommandEvent&) {
        int id = SelectedRuleId();
        if (!id) return;
        for (auto& r : db->GetDeadlineRules()) {
            if (r.id == id) {
                DeadlineRule updated = r;
                updated.enabled = !r.enabled;
                db->UpdateDeadlineRule(updated);
                break;
            }
        }
        LoadDeadlineRules();
    }

    void LoadDeadlineRules() {
        rule_list->DeleteAllItems();
        int row = 0;
        for (const auto& r : db->GetDeadlineRules()) {
            long idx = rule_list->InsertItem(row, DB_STR(r.jurisdiction));
            rule_list->SetItem(idx, 1, DB_STR(r.event_type));
            rule_list->SetItem(idx, 2, DB_STR(r.rule_description));
            rule_list->SetItem(idx, 3, std::to_string(r.base_months));
            rule_list->SetItem(idx, 4, std::to_string(r.base_days));
            rule_list->SetItem(idx, 5, r.extendable ? "yes" : "no");
            rule_list->SetItem(idx, 6, std::to_string(r.max_extension_months));
            rule_list->SetItem(idx, 7, r.enabled ? UTF8_STR("是") : "no");
            rule_list->SetItemData(idx, r.id);
            if (!r.enabled) rule_list->SetItemTextColour(row, wxColour(150, 150, 150));
            row++;
        }
    }

    // ============== Theme ==============
    void SetTheme(int theme) {
        current_theme = theme;
        wxColour bg, fg;
        switch (theme) {
            case 1: bg = wxColour(26, 26, 46); fg = wxColour(212, 212, 212); break;
            case 2: bg = wxColour(199, 237, 204); fg = wxColour(51, 51, 51); break;
            default: bg = wxColour(255, 255, 255); fg = wxColour(51, 51, 51); break;
        }
        SetBackgroundColour(bg);
        SetForegroundColour(fg);
        for (auto* list : {patent_list, oa_list, pct_list, sw_list, ic_list,
                           foreign_list, fee_list, rule_list}) {
            if (list) {
                list->SetBackgroundColour(bg);
                list->SetForegroundColour(fg);
                list->Refresh();
            }
        }
        if (notebook) {
            notebook->SetBackgroundColour(bg);
            notebook->Refresh();
        }
        Refresh();
    }

    // ============== Language ==============
    void SetLanguage(int lang) {
        current_lang = lang;
        const char* en_labels[] = {"Undo", "Sync", "Export", "Import", "Refresh"};
        const char* zh_labels[] = {"撤销", "同步", "导出", "导入", "刷新"};
        for (size_t i = 0; i < toolbar_btns.size() && i < 5; i++) {
            toolbar_btns[i]->SetLabel(lang == 0 ? wxString(en_labels[i]) : UTF8_STR(zh_labels[i]));
        }
        if (btn_new) btn_new->SetLabel(LANG_STR_L(lang, "New", "新建"));
        if (btn_edit) btn_edit->SetLabel(LANG_STR_L(lang, "Edit", "编辑"));
        if (btn_delete) btn_delete->SetLabel(LANG_STR_L(lang, "Delete", "删除"));
        if (btn_batch) btn_batch->SetLabel(LANG_STR_L(lang, "Batch", "批量"));
        if (lbl_search) lbl_search->SetLabel(LANG_STR_L(lang, "Search:", "搜索:"));
        if (lbl_status) lbl_status->SetLabel(LANG_STR_L(lang, "Status:", "状态:"));
        if (lbl_handler) lbl_handler->SetLabel(LANG_STR_L(lang, "Handler:", "处理人:"));
        if (lbl_level) lbl_level->SetLabel(LANG_STR_L(lang, "Level:", "等级:"));

        SetTitle(LANG_STR_L(lang, wxString("patX - Patent Manager v") + PATX_VERSION,
                            wxString("patX - 专利管理系统 v") + PATX_VERSION));
        status_bar->SetStatusText(LANG_STR_L(lang, wxString("patX v") + PATX_VERSION + " | Database: patents.db",
                                              wxString("patX v") + PATX_VERSION + " | 数据库: patents.db"));

        for (size_t i = 0; i < notebook->GetPageCount(); i++) {
            wxString name;
            switch (i) {
                case 0: name = LANG_STR_L(lang, "Domestic Patents", "国内专利"); break;
                case 1: name = LANG_STR_L(lang, "OA Processing", "OA处理"); break;
                case 2: name = LANG_STR_L(lang, "PCT Applications", "PCT申请"); break;
                case 3: name = LANG_STR_L(lang, "Software Copyright", "软件著作权"); break;
                case 4: name = LANG_STR_L(lang, "IC Layout", "集成电路布图"); break;
                case 5: name = LANG_STR_L(lang, "Foreign Patents", "国外专利"); break;
                case 7: name = LANG_STR_L(lang, "Annual Fees", "年费管理"); break;
                case 8: name = LANG_STR_L(lang, "Deadline Rules", "期限规则"); break;
            }
            notebook->SetPageText(i, name);
        }
        RefreshCommonFiltersForTab(GetCurrentTab(), true);
        Layout();
        Refresh();
        Update();
    }

    // ============== Menu Handlers ==============
    void OnImport(wxCommandEvent&) {
        wxString filter = current_lang == 0 ?
            wxString("Excel files (*.xlsx)|*.xlsx|CSV files (*.csv)|*.csv|PDF files (*.pdf)|*.pdf|All files (*.*)|*.*") :
            UTF8_STR("Excel文件 (*.xlsx)|*.xlsx|CSV文件 (*.csv)|*.csv|PDF文件 (*.pdf)|*.pdf|所有文件 (*.*)|*.*");
        wxFileDialog dlg(this, LANG_STR("Import Data", "导入数据"), "", "", filter,
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            std::string path = ToStd(dlg.GetPath());
            wxString ext = dlg.GetPath().Lower().AfterLast('.');
            if (ext == "pdf") ImportPDF(path);
            else ImportExcel(path);
        }
    }

    void ImportPDF(const std::string& path) {
        // pdftotext fallback path shared with the USPTO text extractor
        wxString temp_txt = wxFileName::CreateTempFileName("pdf_") + ".txt";
        wxString pdftotext_cmd;
        wxString candidates[] = {"pdftotext",
                                 "C:\\Program Files\\poppler\\pdftotext.exe",
                                 "C:\\Program Files (x86)\\poppler\\pdftotext.exe",
                                 "C:\\poppler\\pdftotext.exe"};
        bool found = false;
        for (const auto& p : candidates) {
            wxArrayString output;
            if (wxExecute(p + " -v", output, wxEXEC_NODISABLE) != -1 || wxFileExists(p)) {
                pdftotext_cmd = p;
                found = true;
                break;
            }
        }
        if (!found) {
            wxMessageBox(current_lang == 0 ?
                "PDF import requires the pdftotext tool (poppler-utils)." :
                UTF8_STR("PDF导入需要 pdftotext 工具（poppler-utils）。"),
                LANG_STR("Error", "错误"), wxOK | wxICON_ERROR);
            return;
        }

        wxString cmd = wxString::Format("\"%s\" \"%s\" \"%s\"", pdftotext_cmd,
                                        wxString::FromUTF8(path.c_str()), temp_txt);
        if (wxExecute(cmd, wxEXEC_SYNC) != 0 || !wxFileExists(temp_txt)) {
            wxMessageBox(current_lang == 0 ? "Failed to extract text from PDF."
                                           : UTF8_STR("无法从PDF提取文本。"),
                         LANG_STR("Error", "错误"), wxOK | wxICON_ERROR);
            return;
        }

        wxString text;
        wxTextFile f(temp_txt);
        if (f.Open()) {
            text = f.GetFirstLine();
            while (!f.Eof()) text += "\n" + f.GetNextLine();
            f.Close();
        }
        wxRemoveFile(temp_txt);

        // Shared parsing layer with the USPTO importer: app-no + date + type
        std::string app_no;
        wxRegEx app_re(UTF8_STR("申请号[：:]\\s*(\\d{12,13}[A-Z]?)"));
        if (app_re.Matches(text)) {
            app_no = ToStd(app_re.GetMatch(text, 1));
        }
        std::string issue_date;
        wxRegEx date_re(UTF8_STR("发文日[期]?[：:]\\s*(\\d{4})\\s*年\\s*(\\d{1,2})\\s*月\\s*(\\d{1,2})\\s*日"));
        if (date_re.Matches(text)) {
            long year, month, day;
            date_re.GetMatch(text, 1).ToLong(&year);
            date_re.GetMatch(text, 2).ToLong(&month);
            date_re.GetMatch(text, 3).ToLong(&day);
            issue_date = wxString::Format("%04ld-%02ld-%02ld", year, month, day);
        }

        bool is_oa = text.Find(UTF8_STR("审查意见通知书")) != wxNOT_FOUND;
        bool is_rect = text.Find(UTF8_STR("补正通知书")) != wxNOT_FOUND;
        if (!is_oa && !is_rect) {
            wxMessageBox(current_lang == 0 ? "PDF not recognized as OA notification."
                                           : UTF8_STR("PDF未识别为OA通知书。"),
                         LANG_STR("Info", "信息"), wxOK | wxICON_INFORMATION);
            return;
        }

        // Find matching patent (shared application-number normalizer)
        std::string geke_code;
        if (!app_no.empty()) {
            auto strip_dots = [](std::string s) {
                s.erase(std::remove(s.begin(), s.end(), '.'), s.end());
                return s;
            };
            std::string needle = strip_dots(app_no);
            for (const auto& p : db->SearchPatents(app_no)) {
                std::string hay = strip_dots(p.application_number);
                if (hay.find(needle) != std::string::npos || needle.find(hay) != std::string::npos) {
                    geke_code = p.geke_code;
                    break;
                }
            }
        }

        OARecord oa;
        oa.geke_code = geke_code;
        oa.issue_date = issue_date;
        oa.oa_type = is_oa ? "1-OA" : "Admission";

        // Suggested deadline from the rule engine (marked calculated)
        if (!issue_date.empty()) {
            Patent p = db->GetPatentByCode(geke_code);
            std::string event = (p.patent_type == "utility") ? "oa_response_utility"
                                                             : "oa_response_invention";
            std::string deadline = db->CalculateDeadline("CN", event, issue_date);
            if (!deadline.empty()) {
                oa.official_deadline = deadline;
                oa.deadline_source = "calculated";
            }
        }

        int answer = wxMessageBox(wxString::Format(
            UTF8_STR("检测到OA通知书。\n\n申请号: %s\n发文日: %s\n编码: %s\n建议绝限: %s (计算值)\n\n是否创建OA记录?"),
            app_no.empty() ? UTF8_STR("(未识别)") : wxString(app_no.c_str()),
            issue_date.empty() ? UTF8_STR("(未识别)") : wxString(issue_date.c_str()),
            geke_code.empty() ? UTF8_STR("(未找到)") : wxString(geke_code.c_str()),
            oa.official_deadline.empty() ? UTF8_STR("(无规则)") : wxString(oa.official_deadline.c_str())),
            LANG_STR("Import PDF", "导入PDF"), wxYES_NO | wxICON_QUESTION);
        if (answer == wxYES) {
            db->InsertOA(oa);
            LoadOA();
            notebook->SetSelection(1);
        }
    }

    void ImportExcel(const std::string& path) {
        wxProgressDialog progress(LANG_STR("Importing...", "导入中..."),
                                  current_lang == 0 ? wxString("Reading file...")
                                                    : UTF8_STR("正在读取文件..."), 100, this);
        progress.Pulse();

        auto& excel = GetExcelIO();
        auto result = excel.ImportPatents(path, *db, [&progress](int current, int total) -> bool {
            progress.Update(std::min(current * 100 / std::max(total, 1), 99));
            return !progress.WasCancelled();
        });
        progress.Update(100);
        progress.Close();

        if (result.added + result.updated + result.skipped == 0 && !excel.GetLastError().empty()) {
            wxMessageBox((current_lang == 0 ? wxString("Import failed: ")
                                            : UTF8_STR("导入失败: ")) +
                             wxString::FromUTF8(excel.GetLastError().c_str()),
                         LANG_STR("Error", "错误"), wxOK | wxICON_ERROR);
            return;
        }

        LoadAllData();
        PATX_LOG_INFO("Import finished: added=" + std::to_string(result.added) +
                      " updated=" + std::to_string(result.updated));
        wxMessageBox(wxString::Format(
            UTF8_STR("导入完成:\n  新增: %d\n  更新: %d\n  跳过: %d\n\n详情: %s"),
            result.added, result.updated, result.skipped,
            wxString::FromUTF8(result.type_summary.c_str())),
            LANG_STR("Import", "导入"), wxOK | wxICON_INFORMATION);
    }

    // Export uses the module's ExportTable and writes a REAL workbook for
    // .xlsx paths and CSV text for .csv paths (used to be CSV in both cases).
    void OnExport(wxCommandEvent&) {
        int tab = GetCurrentTab();
        ExportTable table;
        int selected_count = 0;
        switch (tab) {
            case 0: {
                wxListCtrl* list = patent_list;
                std::vector<int> ids;
                for (long idx : SelectedRows(list)) ids.push_back(static_cast<int>(list->GetItemData(idx)));
                std::vector<Patent> rows;
                for (const auto& p : db->GetPatents(CurrentQueryFilter())) {
                    if (ids.empty() ||
                        std::find(ids.begin(), ids.end(), p.id) != ids.end()) {
                        rows.push_back(p);
                    }
                }
                selected_count = static_cast<int>(rows.size());
                table = ExcelIO::BuildPatentExport(rows);
                break;
            }
            case 1: {
                std::vector<int> ids;
                for (long idx : SelectedRows(oa_list)) ids.push_back(static_cast<int>(oa_list->GetItemData(idx)));
                std::vector<OARecord> rows;
                for (const auto& oa : db->GetOARecords(QueryFilter())) {
                    if (ids.empty() || std::find(ids.begin(), ids.end(), oa.id) != ids.end()) {
                        rows.push_back(oa);
                    }
                }
                selected_count = static_cast<int>(rows.size());
                table = ExcelIO::BuildOAExport(rows);
                break;
            }
            case 2: {
                std::vector<PCTPatent> rows = db->GetPCTPatents(CurrentQueryFilter());
                selected_count = static_cast<int>(rows.size());
                table = ExcelIO::BuildPCTExport(rows);
                break;
            }
            case 3: {
                std::vector<SoftwareCopyright> rows = db->GetSoftwareCopyrights(CurrentQueryFilter());
                selected_count = static_cast<int>(rows.size());
                table = ExcelIO::BuildSoftwareExport(rows);
                break;
            }
            case 4: {
                std::vector<ICLayout> rows = db->GetICLayouts(CurrentQueryFilter());
                selected_count = static_cast<int>(rows.size());
                table = ExcelIO::BuildICExport(rows);
                break;
            }
            case 5: {
                std::vector<ForeignPatent> rows = db->GetForeignPatents(CurrentQueryFilter());
                selected_count = static_cast<int>(rows.size());
                table = ExcelIO::BuildForeignExport(rows);
                break;
            }
            default:
                wxMessageBox(LANG_STR("This tab has no export", "此页面不支持导出"), "Export", wxOK);
                return;
        }

        if (table.rows.empty()) {
            wxMessageBox(LANG_STR("No data to export", "没有数据可导出"), "Export", wxOK | wxICON_WARNING);
            return;
        }

        wxFileDialog dlg(this, LANG_STR("Export", "导出"), "", "export.xlsx",
                         "Excel workbook (*.xlsx)|*.xlsx|CSV (*.csv)|*.csv",
                         wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK) return;
        std::string path = ToStd(dlg.GetPath());

        ExcelIO io;
        bool ok = false;
        if (path.size() >= 5 && path.substr(path.size() - 5) == ".xlsx") {
            ok = io.ExportXlsx(table, path);
        } else {
            if (path.substr(path.size() - 4) != ".csv") path += ".csv";
            ok = io.ExportCsv(table, path);
        }

        if (ok) {
            PATX_LOG_INFO("Exported " + std::to_string(selected_count) + " rows to " + path);
            wxMessageBox(wxString::Format(LANG_STR("Exported %d records to %s",
                                                   "已导出 %d 条记录到 %s"),
                                          selected_count, wxString(path.c_str())),
                         LANG_STR("Export", "导出"), wxOK | wxICON_INFORMATION);
        } else {
            wxMessageBox(wxString::FromUTF8(io.GetLastError().c_str()),
                         LANG_STR("Error", "错误"), wxOK | wxICON_ERROR);
        }
    }

    // ===================== NAS Sync =====================
    static bool CheckNASAccessible(const std::string& path) {
#ifdef _WIN32
        std::wstring wpath(path.begin(), path.end());
        DWORD attrs = GetFileAttributesW(wpath.c_str());
        return (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
#else
        struct stat st;
        return (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
#endif
    }

    void OnSync(wxCommandEvent&) {
        auto& config = NASConfig::Get();
        if (!config.enabled || config.nas_path.empty()) {
            wxMessageBox(LANG_STR("NAS not configured. Please set up NAS first.",
                                  "NAS未配置，请先设置NAS。"),
                         LANG_STR("Sync", "同步"), wxOK | wxICON_WARNING);
            return;
        }
        if (!CheckNASAccessible(config.nas_path)) {
            wxMessageBox(LANG_STR("Cannot access NAS path.", "无法访问NAS路径。"),
                         LANG_STR("Error", "错误"), wxOK | wxICON_ERROR);
            return;
        }

        // File-based sync for light/single-user use; make a safety backup of
        // both sides first (see README limitations).
        if (!db->BackupTo("patents_local_backup.db")) {
            wxMessageBox("Failed to back up local database; sync aborted", "Error", wxOK | wxICON_ERROR);
            return;
        }

        wxProgressDialog dlg(LANG_STR("Syncing", "同步中"),
                             LANG_STR("Synchronizing with NAS...", "正在与NAS同步..."),
                             100, this, wxPD_APP_MODAL | wxPD_AUTO_HIDE);
        std::string nas_db = config.nas_path + "/patents.db";

        int added = 0, updated = 0, conflicts = 0;
        bool nas_has_data = std::filesystem::exists(nas_db);
        if (nas_has_data) {
            dlg.Update(40, LANG_STR("Merging patents...", "合并数据..."));
            Database nas_db_obj(nas_db);   // read-only usage here

            auto nas_patents = nas_db_obj.GetPatents();
            for (const auto& np : nas_patents) {
                Patent local = db->GetPatentByCode(np.geke_code);
                if (local.id == 0) {
                    db->InsertPatent(np, /*log_undo=*/false);
                    added++;
                } else if (np.updated_at > local.updated_at) {
                    db->UpdatePatent(local.id, np, /*log_undo=*/false);
                    updated++;
                } else if (local.updated_at > np.updated_at) {
                    conflicts++;
                }
            }
        }

        dlg.Update(80, LANG_STR("Uploading to NAS...", "上传到NAS..."));
        if (!db->BackupTo(nas_db)) {
            wxMessageBox("Failed to write NAS database (network dropped?)", "Error", wxOK | wxICON_ERROR);
            return;
        }
        dlg.Update(100);

        db->SetConfig("last_sync_user", config.username);
        db->SetConfig("last_sync_time", std::to_string(std::time(nullptr)));
        LoadAllData();

        wxMessageBox(wxString::Format(
            "Sync completed (file-based, single-user use)\nAdded: %d\nUpdated: %d\nConflicts kept local: %d",
            added, updated, conflicts),
            LANG_STR("Sync", "同步"), wxOK | wxICON_INFORMATION);
    }

    void OnNasConfig(wxCommandEvent&) {
        auto& config = NASConfig::Get();
        wxDialog dlg(this, wxID_ANY, LANG_STR("NAS Configuration", "NAS配置"),
                     wxDefaultPosition, wxSize(520, 380));
        wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
        wxPanel* panel = new wxPanel(&dlg);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        auto row = [&](const wxString& label, wxWindow* ctrl) {
            wxBoxSizer* r = new wxBoxSizer(wxHORIZONTAL);
            r->Add(new wxStaticText(panel, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            r->Add(ctrl, 1);
            sizer->Add(r, 0, wxEXPAND | wxALL, 8);
        };

        wxTextCtrl* path_field = new wxTextCtrl(panel, wxID_ANY, config.nas_path);
        row(LANG_STR("NAS Path:", "NAS路径:"), path_field);
        wxTextCtrl* user_field = new wxTextCtrl(panel, wxID_ANY, config.username);
        row(LANG_STR("Username:", "用户名:"), user_field);
        wxSpinCtrl* sync_spin = new wxSpinCtrl(panel, wxID_ANY, std::to_string(config.auto_sync_minutes),
                                               wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 60,
                                               config.auto_sync_minutes);
        row(LANG_STR("Auto Sync (minutes):", "自动同步(分钟):"), sync_spin);
        wxCheckBox* enable_cb = new wxCheckBox(panel, wxID_ANY,
                                               LANG_STR("Enable NAS Sync", "启用NAS同步"));
        enable_cb->SetValue(config.enabled);
        sizer->Add(enable_cb, 0, wxALL, 8);

        auto* note = new wxStaticText(panel, wxID_ANY,
            LANG_STR("NAS sync is file-based and intended for light/single-user use.",
                     "NAS同步为文件级同步，适合轻量单人使用。"));
        note->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_ITALIC, wxFONTWEIGHT_NORMAL));
        sizer->Add(note, 0, wxALL, 8);

        panel->SetSizer(sizer);
        wxBoxSizer* btns = new wxBoxSizer(wxHORIZONTAL);
        btns->AddStretchSpacer();
        btns->Add(new wxButton(&dlg, wxID_OK, LANG_STR("Save", "保存")), 0, wxRIGHT, 8);
        btns->Add(new wxButton(&dlg, wxID_CANCEL, LANG_STR("Cancel", "取消")), 0);
        main_sizer->Add(panel, 1, wxEXPAND);
        main_sizer->Add(btns, 0, wxEXPAND | wxALL, 10);
        dlg.SetSizer(main_sizer);
        dlg.Centre();

        if (dlg.ShowModal() == wxID_OK) {
            config.nas_path = ToStd(path_field->GetValue());
            config.username = ToStd(user_field->GetValue());
            config.auto_sync_minutes = sync_spin->GetValue();
            config.enabled = enable_cb->GetValue();
            config.Save();
        }
    }

    void OnBackup(wxCommandEvent&) {
        wxFileDialog dlg(this, "Backup Database", "", "patents_backup.db",
                         "Database files (*.db)|*.db", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() == wxID_OK) {
            if (db->BackupTo(ToStd(dlg.GetPath()))) {
                wxMessageBox("Backup created successfully!", "Backup", wxOK | wxICON_INFORMATION);
            } else {
                wxMessageBox("Backup failed: " + db->LastError(), "Backup", wxOK | wxICON_ERROR);
            }
        }
    }

    void OnSwitchDatabase(wxCommandEvent&) {
        wxFileDialog dlg(this, "Open Database", "", "",
                         "Database (*.db)|*.db|All files (*.*)|*.*",
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            if (wxMessageBox(wxString::Format("Switch to database:\n%s", dlg.GetPath()),
                             "Switch Database", wxYES_NO | wxICON_QUESTION) == wxYES) {
                std::string new_path = ToStd(dlg.GetPath());
                dossier_controller.reset();
                db = std::make_unique<Database>(new_path);
                dossier_controller = std::make_unique<WebDossierController>(
                    this, *db, [this](const std::string& code) { ShowPatentByCode(code); });
                LoadAllData();
                status_bar->SetStatusText(wxString("patX v") + PATX_VERSION + " | Database: " +
                                          wxFileName(dlg.GetPath()).GetFullName());
            }
        }
    }

    void OnRestore(wxCommandEvent&) {
        wxFileDialog dlg(this, "Restore Backup", "", "",
                         "Database files (*.db)|*.db", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            if (wxMessageBox("This will replace the current database. Continue?",
                             "Warning", wxYES_NO | wxICON_WARNING) == wxYES) {
                std::string src = ToStd(dlg.GetPath());
                dossier_controller.reset();
                db.reset();
                std::filesystem::copy_file(src, "patents.db",
                                           std::filesystem::copy_options::overwrite_existing);
                db = std::make_unique<Database>("patents.db");
                dossier_controller = std::make_unique<WebDossierController>(
                    this, *db, [this](const std::string& code) { ShowPatentByCode(code); });
                LoadAllData();
            }
        }
    }

    void OnUndo(wxCommandEvent&) {
        if (!db->CanUndo()) {
            wxMessageBox(LANG_STR("No operations to undo", "没有可撤销的操作"),
                         LANG_STR("Undo", "撤销"), wxOK | wxICON_INFORMATION);
            return;
        }
        int count = db->Undo();
        LoadAllData();
        wxMessageBox(wxString::Format(LANG_STR("Undone %d operations", "已撤销 %d 个操作"), count),
                     LANG_STR("Undo", "撤销"), wxOK | wxICON_INFORMATION);
    }

    void OnRefresh(wxCommandEvent&) {
        LoadAllData();
    }

    void OnAbout(wxCommandEvent&) {
        wxMessageBox(
            wxString::Format(
                "patX v%s - Patent Management System\n"
                "========================================\n\n"
                "Modules: Domestic / OA / PCT / Software / IC /\n"
                "Tech: C++17 + wxWidgets + SQLite3 + OpenXLSX + libcurl\n"
                "GitHub: https://github.com/deepinwine/patX",
                PATX_VERSION),
            "About patX", wxOK | wxICON_INFORMATION);
    }

    // ---- Web dossier sync (审查信息网页同步) ----
    // Jumps to the patent tab and selects the row with this internal code.
    void ShowPatentByCode(const std::string& geke_code) {
        notebook->SetSelection(0);   // patent tab
        for (long i = 0; i < patent_list->GetItemCount(); ++i) {
            wxString item_code = patent_list->GetItemText(i, 0);
            patent_list->SetItemState(i, 0, wxLIST_STATE_SELECTED);
            if (item_code.ToStdString() == geke_code) {
                patent_list->SetItemState(i, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
                patent_list->EnsureVisible(i);
                patent_list->SetFocus();
            }
        }
    }

    void OnDossierSyncSelected(wxCommandEvent&) {
        std::vector<Patent> selected;
        long idx = -1;
        while ((idx = patent_list->GetNextItem(idx, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) >= 0) {
            int id = patent_list->GetItemData(idx);
            Patent p = db->GetPatentById(id);
            if (p.id > 0) selected.push_back(p);
        }
        if (selected.empty()) {
            wxMessageBox(UTF8_STR("请先在专利列表中选择案件"),
                         UTF8_STR("审查信息同步"), wxOK | wxICON_INFORMATION);
            return;
        }
        dossier_controller->SyncPatents(selected);
    }

    void OnDossierSyncAll(wxCommandEvent&) { dossier_controller->SyncAllActive(false); }
    void OnDossierSyncGranted(wxCommandEvent&) { dossier_controller->SyncAllActive(true); }
    void OnDossierLogin(wxCommandEvent&) { dossier_controller->Login(); }
    void OnDossierHistory(wxCommandEvent&) { dossier_controller->ShowHistory(); }
    void OnDossierErrors(wxCommandEvent&) { dossier_controller->ShowErrorCases(); }

    void OnExit(wxCommandEvent&) {
        PATX_LOG_INFO("patX shutting down");
        patx::ShutDownLogging();
        Close(true);
    }

    void LoadAllData() {
        RefreshCommonFiltersForTab(GetCurrentTab(), true);
        LoadPatents();
        LoadOA();
        LoadPCT();
        LoadSoftware();
        LoadIC();
        LoadForeign();
        LoadAnnualFees();
        LoadDeadlineRules();
    }

    wxComboBox* inventor_filter_ = nullptr;
};

class PatXApp : public wxApp {
public:
    bool OnInit() override {
        PatXFrame* frame = new PatXFrame();
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(PatXApp);
