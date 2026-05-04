// patX GUI - Complete Patent Manager with SQLite
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/splitter.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/textdlg.h>
#include <wx/choicdlg.h>
#include <wx/progdlg.h>
#include <wx/artprov.h>
#include <wx/aui/auibook.h>
#include <wx/textfile.h>
#include <wx/file.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/spinctrl.h>
#include <wx/filename.h>
#include <wx/regex.h>
#include <memory>
#include <fstream>
#include "database.hpp"
#include "excel_io.hpp"
#include "pdf_parser.hpp"

// Import types from patx namespace
using patx::PdfParser;
using patx::GetPdfParser;
using patx::ParsedOaInfo;
using patx::OaType;

// Helper macros for UTF-8 Chinese strings on Windows
// MSVC /utf-8 flag stores literals as UTF-8, but wxString(const char*) uses system locale (GBK)
// Use FromUTF8() for proper conversion
#define UTF8_STR(s) wxString::FromUTF8(s)

// Language switch helper - returns wxString with proper UTF-8 handling for Chinese
// Usage: LANG_STR("English text", "中文文本")
#define LANG_STR(en, zh) (current_lang == 0 ? wxString(en) : wxString::FromUTF8(zh))

// Same for when lang is a parameter not member variable
#define LANG_STR_L(lang, en, zh) (lang == 0 ? wxString(en) : wxString::FromUTF8(zh))

// Convert UTF-8 std::string from database to wxString
#define DB_STR(s) wxString::FromUTF8(s.c_str())

// Smart string comparison: tries numeric sort first, falls back to string
static int SmartCompare(const wxString& a, const wxString& b, bool ascending) {
    if (a.IsEmpty() && b.IsEmpty()) return 0;
    if (a.IsEmpty()) return ascending ? 1 : -1;
    if (b.IsEmpty()) return ascending ? -1 : 1;

    // Try to parse both as numbers
    double da = 0, db = 0;
    bool a_num = a.ToDouble(&da);
    bool b_num = b.ToDouble(&db);

    if (a_num && b_num) {
        return ascending ? (da < db ? -1 : da > db ? 1 : 0) : (da > db ? 1 : da < db ? -1 : 0);
    }
    return ascending ? a.Cmp(b) : b.Cmp(a);
}

// Generic sort helper for wxListCtrl - sorts by column text, preserves item data
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

// Column header right-click to show filter popup
// Maps: list -> column -> filter value
static std::map<wxListCtrl*, std::map<int, std::string>> g_column_filters;

// ============== NAS Configuration ==============
struct NASConfig {
    std::string nas_path;           // NAS共享路径
    std::string username;           // 当前用户名
    int auto_sync_minutes = 0;      // 自动同步间隔(分钟), 0=禁用
    bool enabled = false;

    void Save() {
        std::ofstream f("nas_config.txt");
        if (f.is_open()) {
            f << nas_path << "\n";
            f << username << "\n";
            f << auto_sync_minutes << "\n";
            f << (enabled ? "1" : "0") << "\n";
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

// ============== Edit Dialog ==============
class PatentEditDialog : public wxDialog {
public:
    PatentEditDialog(wxWindow* parent, Database* db, int patent_id = 0)
        : wxDialog(parent, wxID_ANY, patent_id ? "Edit Patent" : "New Patent", wxDefaultPosition, wxSize(600, 500)),
          db_(db), patent_id_(patent_id) {
        SetupUI();
        if (patent_id) LoadData();
    }

private:
    Database* db_;
    int patent_id_;
    std::map<std::string, wxTextCtrl*> fields_;
    wxComboBox* type_combo_;
    wxComboBox* level_combo_;
    wxComboBox* status_combo_;

    void SetupUI() {
        wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
        wxScrolledWindow* scroll = new wxScrolledWindow(this, wxID_ANY);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        // Basic info
        sizer->Add(new wxStaticText(scroll, wxID_ANY, "=== Basic Info ==="), 0, wxTOP, 10);

        AddField(scroll, sizer, "geke_code", UTF8_STR("编号"));
        AddField(scroll, sizer, "application_number", "Application No");
        AddField(scroll, sizer, "title", "Title", 300);
        AddField(scroll, sizer, "proposal_name", "Proposal Name", 200);

        // Type/Level/Status
        wxBoxSizer* row1 = new wxBoxSizer(wxHORIZONTAL);
        row1->Add(new wxStaticText(scroll, wxID_ANY, "Type:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        type_combo_ = new wxComboBox(scroll, wxID_ANY, "invention");
        type_combo_->Append("invention");
        type_combo_->Append("utility");
        type_combo_->Append("design");
        row1->Add(type_combo_, 1, wxRIGHT, 20);

        row1->Add(new wxStaticText(scroll, wxID_ANY, "Level:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        level_combo_ = new wxComboBox(scroll, wxID_ANY, "normal");
        level_combo_->Append("core");
        level_combo_->Append("important");
        level_combo_->Append("normal");
        row1->Add(level_combo_, 1, wxRIGHT, 20);

        row1->Add(new wxStaticText(scroll, wxID_ANY, "Status:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        status_combo_ = new wxComboBox(scroll, wxID_ANY, "pending");
        status_combo_->Append("pending");
        status_combo_->Append("granted");
        status_combo_->Append("rejected");
        status_combo_->Append("abandoned");
        row1->Add(status_combo_, 1);
        sizer->Add(row1, 0, wxEXPAND | wxALL, 5);

        // Dates
        sizer->Add(new wxStaticText(scroll, wxID_ANY, "=== Dates ==="), 0, wxTOP, 10);
        wxBoxSizer* row2 = new wxBoxSizer(wxHORIZONTAL);
        AddField(scroll, row2, "application_date", "App Date", 120);
        AddField(scroll, row2, "authorization_date", "Auth Date", 120);
        AddField(scroll, row2, "expiration_date", "Exp Date", 120);
        sizer->Add(row2, 0, wxEXPAND | wxALL, 5);

        // People
        sizer->Add(new wxStaticText(scroll, wxID_ANY, "=== People ==="), 0, wxTOP, 10);
        wxBoxSizer* row3 = new wxBoxSizer(wxHORIZONTAL);
        AddField(scroll, row3, "geke_handler", UTF8_STR("处理人"), 100);
        AddField(scroll, row3, "inventor", "Inventor", 150);
        sizer->Add(row3, 0, wxEXPAND | wxALL, 5);

        wxBoxSizer* row4 = new wxBoxSizer(wxHORIZONTAL);
        AddField(scroll, row4, "rd_department", "R&D Dept", 100);
        AddField(scroll, row4, "agency_firm", "Agency", 150);
        sizer->Add(row4, 0, wxEXPAND | wxALL, 5);

        // Applicants
        sizer->Add(new wxStaticText(scroll, wxID_ANY, "=== Applicants ==="), 0, wxTOP, 10);
        AddField(scroll, sizer, "original_applicant", "Original Applicant", 200);
        AddField(scroll, sizer, "current_applicant", "Current Applicant", 200);

        // Classification
        sizer->Add(new wxStaticText(scroll, wxID_ANY, "=== Classification ==="), 0, wxTOP, 10);
        wxBoxSizer* row5 = new wxBoxSizer(wxHORIZONTAL);
        AddField(scroll, row5, "class_level1", "Level 1", 80);
        AddField(scroll, row5, "class_level2", "Level 2", 80);
        AddField(scroll, row5, "class_level3", "Level 3", 80);
        sizer->Add(row5, 0, wxEXPAND | wxALL, 5);

        // Notes
        AddField(scroll, sizer, "notes", "Notes", 400, true);

        scroll->SetSizer(sizer);
        scroll->SetScrollRate(5, 5);
        main_sizer->Add(scroll, 1, wxEXPAND | wxALL, 10);

        // Buttons
        wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton* save_btn = new wxButton(this, wxID_OK, "Save");
        wxButton* cancel_btn = new wxButton(this, wxID_CANCEL, "Cancel");
        btn_sizer->AddStretchSpacer();
        btn_sizer->Add(save_btn, 0, wxRIGHT, 10);
        btn_sizer->Add(cancel_btn, 0);
        main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

        SetSizer(main_sizer);
        Centre();

        Bind(wxEVT_BUTTON, &PatentEditDialog::OnSave, this, wxID_OK);
    }

    void AddField(wxWindow* parent, wxSizer* sizer, const std::string& key,
                  const wxString& label, int width = 150, bool multiline = false) {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(parent, wxID_ANY, label + ":"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        if (multiline) {
            wxTextCtrl* tc = new wxTextCtrl(parent, wxID_ANY, "", wxDefaultPosition,
                                           wxSize(width, 60), wxTE_MULTILINE);
            fields_[key] = tc;
            row->Add(tc, 1, wxEXPAND);
            sizer->Add(row, 0, wxEXPAND | wxALL, 3);
        } else {
            wxTextCtrl* tc = new wxTextCtrl(parent, wxID_ANY, "", wxDefaultPosition, wxSize(width, -1));
            fields_[key] = tc;
            row->Add(tc, 1);
            sizer->Add(row, 0, wxEXPAND | wxALL, 3);
        }
    }

    void LoadData() {
        Patent p = db_->GetPatentById(patent_id_);
        if (fields_.count("geke_code")) fields_["geke_code"]->SetValue(DB_STR(p.geke_code));
        if (fields_.count("application_number")) fields_["application_number"]->SetValue(DB_STR(p.application_number));
        if (fields_.count("title")) fields_["title"]->SetValue(DB_STR(p.title));
        if (fields_.count("proposal_name")) fields_["proposal_name"]->SetValue(DB_STR(p.proposal_name));
        if (fields_.count("application_date")) fields_["application_date"]->SetValue(DB_STR(p.application_date));
        if (fields_.count("authorization_date")) fields_["authorization_date"]->SetValue(DB_STR(p.authorization_date));
        if (fields_.count("expiration_date")) fields_["expiration_date"]->SetValue(DB_STR(p.expiration_date));
        if (fields_.count("geke_handler")) fields_["geke_handler"]->SetValue(DB_STR(p.geke_handler));
        if (fields_.count("inventor")) fields_["inventor"]->SetValue(DB_STR(p.inventor));
        if (fields_.count("rd_department")) fields_["rd_department"]->SetValue(DB_STR(p.rd_department));
        if (fields_.count("agency_firm")) fields_["agency_firm"]->SetValue(DB_STR(p.agency_firm));
        if (fields_.count("original_applicant")) fields_["original_applicant"]->SetValue(DB_STR(p.original_applicant));
        if (fields_.count("current_applicant")) fields_["current_applicant"]->SetValue(DB_STR(p.current_applicant));
        if (fields_.count("class_level1")) fields_["class_level1"]->SetValue(DB_STR(p.class_level1));
        if (fields_.count("class_level2")) fields_["class_level2"]->SetValue(DB_STR(p.class_level2));
        if (fields_.count("class_level3")) fields_["class_level3"]->SetValue(DB_STR(p.class_level3));
        if (fields_.count("notes")) fields_["notes"]->SetValue(DB_STR(p.notes));

        type_combo_->SetValue(DB_STR(p.patent_type));
        level_combo_->SetValue(DB_STR(p.patent_level));
        status_combo_->SetValue(DB_STR(p.application_status));
    }

    void OnSave(wxCommandEvent&) {
        if (!db_) return;

        Patent p;
        p.id = patent_id_;
        p.geke_code = fields_["geke_code"]->GetValue().ToStdString();
        p.application_number = fields_["application_number"]->GetValue().ToStdString();
        p.title = fields_["title"]->GetValue().ToStdString();
        p.proposal_name = fields_["proposal_name"]->GetValue().ToStdString();
        p.application_date = fields_["application_date"]->GetValue().ToStdString();
        p.authorization_date = fields_["authorization_date"]->GetValue().ToStdString();
        p.expiration_date = fields_["expiration_date"]->GetValue().ToStdString();
        p.geke_handler = fields_["geke_handler"]->GetValue().ToStdString();
        p.inventor = fields_["inventor"]->GetValue().ToStdString();
        p.rd_department = fields_["rd_department"]->GetValue().ToStdString();
        p.agency_firm = fields_["agency_firm"]->GetValue().ToStdString();
        p.original_applicant = fields_["original_applicant"]->GetValue().ToStdString();
        p.current_applicant = fields_["current_applicant"]->GetValue().ToStdString();
        p.class_level1 = fields_["class_level1"]->GetValue().ToStdString();
        p.class_level2 = fields_["class_level2"]->GetValue().ToStdString();
        p.class_level3 = fields_["class_level3"]->GetValue().ToStdString();
        p.notes = fields_["notes"]->GetValue().ToStdString();
        p.patent_type = type_combo_->GetValue().ToStdString();
        p.patent_level = level_combo_->GetValue().ToStdString();
        p.application_status = status_combo_->GetValue().ToStdString();

        if (p.geke_code.empty()) {
            wxMessageBox(UTF8_STR("编号不能为空"), UTF8_STR("错误"), wxOK | wxICON_ERROR);
            return;
        }
        if (p.title.empty()) {
            wxMessageBox("Title is required", "Error", wxOK | wxICON_ERROR);
            return;
        }

        bool success;
        if (patent_id_) {
            success = db_->UpdatePatent(patent_id_, p);
        } else {
            int new_id = db_->InsertPatent(p);
            success = new_id > 0;
        }

        if (success) {
            EndModal(wxID_OK);
        } else {
            wxMessageBox(UTF8_STR("保存失败（编号重复？）"), UTF8_STR("错误"), wxOK | wxICON_ERROR);
        }
    }
};

// ============== OA Edit Dialog ==============
class OAEditDialog : public wxDialog {
public:
    OAEditDialog(wxWindow* parent, Database* db, int oa_id = 0)
        : wxDialog(parent, wxID_ANY, oa_id ? "Edit OA Record" : "New OA Record", wxDefaultPosition, wxSize(500, 400)),
          db_(db), oa_id_(oa_id) {
        SetupUI();
        if (oa_id) LoadData();
    }
private:
    Database* db_;
    int oa_id_;
    wxTextCtrl* geke_code_field_;
    wxComboBox* oa_type_combo_;
    wxTextCtrl* deadline_field_;
    wxTextCtrl* issue_date_field_;
    wxTextCtrl* handler_field_;
    wxTextCtrl* writer_field_;
    wxComboBox* progress_combo_;
    wxCheckBox* extendable_check_;
    wxCheckBox* completed_check_;

    void SetupUI() {
        wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
        wxPanel* panel = new wxPanel(this);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* row1 = new wxBoxSizer(wxHORIZONTAL);
        row1->Add(new wxStaticText(panel, wxID_ANY, UTF8_STR("编号:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        geke_code_field_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(150, -1));
        row1->Add(geke_code_field_, 1);
        sizer->Add(row1, 0, wxEXPAND | wxALL, 5);

        wxBoxSizer* row2 = new wxBoxSizer(wxHORIZONTAL);
        row2->Add(new wxStaticText(panel, wxID_ANY, "OA Type:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        oa_type_combo_ = new wxComboBox(panel, wxID_ANY, "1-OA");
        oa_type_combo_->Append("1-OA");
        oa_type_combo_->Append("2-OA");
        oa_type_combo_->Append("3-OA");
        oa_type_combo_->Append("Rejection");
        oa_type_combo_->Append("Admission");
        row2->Add(oa_type_combo_, 1);
        sizer->Add(row2, 0, wxEXPAND | wxALL, 5);

        wxBoxSizer* row3 = new wxBoxSizer(wxHORIZONTAL);
        row3->Add(new wxStaticText(panel, wxID_ANY, "Issue Date:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        issue_date_field_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(100, -1));
        row3->Add(issue_date_field_, 0, wxRIGHT, 20);
        row3->Add(new wxStaticText(panel, wxID_ANY, "Deadline:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        deadline_field_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(100, -1));
        row3->Add(deadline_field_, 1);
        sizer->Add(row3, 0, wxEXPAND | wxALL, 5);

        wxBoxSizer* row4 = new wxBoxSizer(wxHORIZONTAL);
        row4->Add(new wxStaticText(panel, wxID_ANY, "Handler:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        handler_field_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(80, -1));
        row4->Add(handler_field_, 0, wxRIGHT, 20);
        row4->Add(new wxStaticText(panel, wxID_ANY, "Writer:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        writer_field_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(80, -1));
        row4->Add(writer_field_, 1);
        sizer->Add(row4, 0, wxEXPAND | wxALL, 5);

        wxBoxSizer* row5 = new wxBoxSizer(wxHORIZONTAL);
        row5->Add(new wxStaticText(panel, wxID_ANY, "Progress:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        progress_combo_ = new wxComboBox(panel, wxID_ANY, "pending");
        progress_combo_->Append("pending");
        progress_combo_->Append("drafting");
        progress_combo_->Append("submitted");
        progress_combo_->Append("completed");
        row5->Add(progress_combo_, 1);
        sizer->Add(row5, 0, wxEXPAND | wxALL, 5);

        wxBoxSizer* row6 = new wxBoxSizer(wxHORIZONTAL);
        extendable_check_ = new wxCheckBox(panel, wxID_ANY, "Extendable");
        completed_check_ = new wxCheckBox(panel, wxID_ANY, "Completed");
        row6->Add(extendable_check_, 0, wxRIGHT, 20);
        row6->Add(completed_check_, 0);
        sizer->Add(row6, 0, wxALL, 5);

        panel->SetSizer(sizer);
        main_sizer->Add(panel, 1, wxEXPAND | wxALL, 10);

        wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton* save_btn = new wxButton(this, wxID_OK, "Save");
        wxButton* cancel_btn = new wxButton(this, wxID_CANCEL, "Cancel");
        btn_sizer->AddStretchSpacer();
        btn_sizer->Add(save_btn, 0, wxRIGHT, 10);
        btn_sizer->Add(cancel_btn, 0);
        main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

        SetSizer(main_sizer);
        Centre();
        Bind(wxEVT_BUTTON, &OAEditDialog::OnSave, this, wxID_OK);
    }

    void LoadData() {
        OARecord oa = db_->GetOAById(oa_id_);
        geke_code_field_->SetValue(DB_STR(oa.geke_code));
        oa_type_combo_->SetValue(DB_STR(oa.oa_type));
        deadline_field_->SetValue(DB_STR(oa.official_deadline));
        issue_date_field_->SetValue(DB_STR(oa.issue_date));
        handler_field_->SetValue(DB_STR(oa.handler));
        writer_field_->SetValue(DB_STR(oa.writer));
        progress_combo_->SetValue(DB_STR(oa.progress));
        extendable_check_->SetValue(oa.is_extendable);
        completed_check_->SetValue(oa.is_completed);
    }

    void OnSave(wxCommandEvent&) {
        OARecord oa;
        oa.id = oa_id_;
        oa.geke_code = geke_code_field_->GetValue().ToStdString();
        oa.oa_type = oa_type_combo_->GetValue().ToStdString();
        oa.official_deadline = deadline_field_->GetValue().ToStdString();
        oa.issue_date = issue_date_field_->GetValue().ToStdString();
        oa.handler = handler_field_->GetValue().ToStdString();
        oa.writer = writer_field_->GetValue().ToStdString();
        oa.progress = progress_combo_->GetValue().ToStdString();
        oa.is_extendable = extendable_check_->GetValue();
        oa.is_completed = completed_check_->GetValue();

        if (oa.geke_code.empty()) {
            wxMessageBox(UTF8_STR("编号不能为空"), UTF8_STR("错误"), wxOK | wxICON_ERROR);
            return;
        }

        bool success;
        if (oa_id_) {
            success = db_->UpdateOA(oa_id_, oa);
        } else {
            Patent p = db_->GetPatentByCode(oa.geke_code);
            oa.patent_title = p.title;
            int new_id = db_->InsertOA(oa);
            success = new_id > 0;
        }

        if (success) {
            EndModal(wxID_OK);
        } else {
            wxMessageBox("Failed to save OA record", "Error", wxOK | wxICON_ERROR);
        }
    }
};

// ============== PCT Edit Dialog ==============
class PCTEditDialog : public wxDialog {
public:
    PCTEditDialog(wxWindow* parent, Database* db, int pct_id = 0)
        : wxDialog(parent, wxID_ANY, pct_id ? "Edit PCT" : "New PCT", wxDefaultPosition, wxSize(550, 450)),
          db_(db), pct_id_(pct_id) {
        SetupUI();
        if (pct_id) LoadData();
    }
private:
    Database* db_;
    int pct_id_;
    std::map<std::string, wxTextCtrl*> fields_;
    wxComboBox* status_combo_;

    void SetupUI() {
        wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
        wxScrolledWindow* scroll = new wxScrolledWindow(this, wxID_ANY);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        auto add_row = [&](const wxString& label, const std::string& key, int w = 200) {
            wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(scroll, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            wxTextCtrl* tc = new wxTextCtrl(scroll, wxID_ANY, "", wxDefaultPosition, wxSize(w, -1));
            fields_[key] = tc;
            row->Add(tc, 1);
            sizer->Add(row, 0, wxEXPAND | wxALL, 3);
        };

        add_row(UTF8_STR("编号:"), "geke_code", 120);
        add_row("Domestic Source:", "domestic_source", 120);
        add_row("PCT Application No:", "application_no", 150);
        add_row("Country App No:", "country_app_no", 150);
        add_row("Title:", "title", 350);

        wxBoxSizer* status_row = new wxBoxSizer(wxHORIZONTAL);
        status_row->Add(new wxStaticText(scroll, wxID_ANY, "Status:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        status_combo_ = new wxComboBox(scroll, wxID_ANY, "pending");
        status_combo_->Append("pending");
        status_combo_->Append("national phase");
        status_combo_->Append("granted");
        status_combo_->Append("rejected");
        status_combo_->Append("abandoned");
        status_row->Add(status_combo_, 1);
        sizer->Add(status_row, 0, wxEXPAND | wxALL, 3);

        add_row("Handler:", "handler", 100);
        add_row("Inventor:", "inventor", 200);
        add_row("Filing Date:", "filing_date", 100);
        add_row("Application Date:", "application_date", 100);
        add_row("Priority Date:", "priority_date", 100);
        add_row("Country:", "country", 80);
        add_row("Notes:", "notes", 350);

        scroll->SetSizer(sizer);
        scroll->SetScrollRate(5, 5);
        main_sizer->Add(scroll, 1, wxEXPAND | wxALL, 10);

        wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton* save_btn = new wxButton(this, wxID_OK, "Save");
        wxButton* cancel_btn = new wxButton(this, wxID_CANCEL, "Cancel");
        btn_sizer->AddStretchSpacer();
        btn_sizer->Add(save_btn, 0, wxRIGHT, 10);
        btn_sizer->Add(cancel_btn, 0);
        main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

        SetSizer(main_sizer);
        Centre();
        Bind(wxEVT_BUTTON, &PCTEditDialog::OnSave, this, wxID_OK);
    }

    void LoadData() {
        PCTPatent p = db_->GetPCTById(pct_id_);
        if (fields_.count("geke_code")) fields_["geke_code"]->SetValue(DB_STR(p.geke_code));
        if (fields_.count("domestic_source")) fields_["domestic_source"]->SetValue(DB_STR(p.domestic_source));
        if (fields_.count("application_no")) fields_["application_no"]->SetValue(DB_STR(p.application_no));
        if (fields_.count("country_app_no")) fields_["country_app_no"]->SetValue(DB_STR(p.country_app_no));
        if (fields_.count("title")) fields_["title"]->SetValue(DB_STR(p.title));
        if (fields_.count("handler")) fields_["handler"]->SetValue(DB_STR(p.handler));
        if (fields_.count("inventor")) fields_["inventor"]->SetValue(DB_STR(p.inventor));
        if (fields_.count("filing_date")) fields_["filing_date"]->SetValue(DB_STR(p.filing_date));
        if (fields_.count("application_date")) fields_["application_date"]->SetValue(DB_STR(p.application_date));
        if (fields_.count("priority_date")) fields_["priority_date"]->SetValue(DB_STR(p.priority_date));
        if (fields_.count("country")) fields_["country"]->SetValue(DB_STR(p.country));
        if (fields_.count("notes")) fields_["notes"]->SetValue(DB_STR(p.notes));
        status_combo_->SetValue(DB_STR(p.application_status));
    }

    void OnSave(wxCommandEvent&) {
        PCTPatent p;
        p.id = pct_id_;
        p.geke_code = fields_["geke_code"]->GetValue().ToStdString();
        p.domestic_source = fields_["domestic_source"]->GetValue().ToStdString();
        p.application_no = fields_["application_no"]->GetValue().ToStdString();
        p.country_app_no = fields_["country_app_no"]->GetValue().ToStdString();
        p.title = fields_["title"]->GetValue().ToStdString();
        p.application_status = status_combo_->GetValue().ToStdString();
        p.handler = fields_["handler"]->GetValue().ToStdString();
        p.inventor = fields_["inventor"]->GetValue().ToStdString();
        p.filing_date = fields_["filing_date"]->GetValue().ToStdString();
        p.application_date = fields_["application_date"]->GetValue().ToStdString();
        p.priority_date = fields_["priority_date"]->GetValue().ToStdString();
        p.country = fields_["country"]->GetValue().ToStdString();
        p.notes = fields_["notes"]->GetValue().ToStdString();

        if (p.geke_code.empty()) {
            wxMessageBox(UTF8_STR("编号不能为空"), UTF8_STR("错误"), wxOK | wxICON_ERROR);
            return;
        }

        bool success;
        if (pct_id_) {
            success = db_->UpdatePCT(pct_id_, p);
        } else {
            int new_id = db_->InsertPCT(p);
            success = new_id > 0;
        }

        if (success) {
            EndModal(wxID_OK);
        } else {
            wxMessageBox("Failed to save PCT", "Error", wxOK | wxICON_ERROR);
        }
    }
};

// ============== Software Edit Dialog ==============
class SoftwareEditDialog : public wxDialog {
public:
    SoftwareEditDialog(wxWindow* parent, Database* db, int sw_id = 0)
        : wxDialog(parent, wxID_ANY, sw_id ? "Edit Software Copyright" : "New Software Copyright", wxDefaultPosition, wxSize(550, 400)),
          db_(db), sw_id_(sw_id) {
        SetupUI();
        if (sw_id) LoadData();
    }
private:
    Database* db_;
    int sw_id_;
    std::map<std::string, wxTextCtrl*> fields_;
    wxComboBox* status_combo_;

    void SetupUI() {
        wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
        wxPanel* panel = new wxPanel(this);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        auto add_row = [&](const wxString& label, const std::string& key, int w = 200) {
            wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(panel, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            wxTextCtrl* tc = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(w, -1));
            fields_[key] = tc;
            row->Add(tc, 1);
            sizer->Add(row, 0, wxEXPAND | wxALL, 3);
        };

        add_row("Case No:", "case_no", 120);
        add_row("Reg No:", "reg_no", 120);
        add_row("Title:", "title", 350);
        add_row("Original Owner:", "original_owner", 200);
        add_row("Current Owner:", "current_owner", 200);

        wxBoxSizer* status_row = new wxBoxSizer(wxHORIZONTAL);
        status_row->Add(new wxStaticText(panel, wxID_ANY, "Status:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        status_combo_ = new wxComboBox(panel, wxID_ANY, "pending");
        status_combo_->Append("pending");
        status_combo_->Append("registered");
        status_combo_->Append("rejected");
        status_row->Add(status_combo_, 1);
        sizer->Add(status_row, 0, wxEXPAND | wxALL, 3);

        add_row("Handler:", "handler", 100);
        add_row("Developer:", "developer", 200);
        add_row("Inventor:", "inventor", 200);
        add_row("Dev Complete Date:", "dev_complete_date", 100);
        add_row("Application Date:", "application_date", 100);
        add_row("Reg Date:", "reg_date", 100);
        add_row("Version:", "version", 80);

        panel->SetSizer(sizer);
        main_sizer->Add(panel, 1, wxEXPAND | wxALL, 10);

        wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton* save_btn = new wxButton(this, wxID_OK, "Save");
        wxButton* cancel_btn = new wxButton(this, wxID_CANCEL, "Cancel");
        btn_sizer->AddStretchSpacer();
        btn_sizer->Add(save_btn, 0, wxRIGHT, 10);
        btn_sizer->Add(cancel_btn, 0);
        main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

        SetSizer(main_sizer);
        Centre();
        Bind(wxEVT_BUTTON, &SoftwareEditDialog::OnSave, this, wxID_OK);
    }

    void LoadData() {
        SoftwareCopyright s = db_->GetSoftwareById(sw_id_);
        if (fields_.count("case_no")) fields_["case_no"]->SetValue(DB_STR(s.case_no));
        if (fields_.count("reg_no")) fields_["reg_no"]->SetValue(DB_STR(s.reg_no));
        if (fields_.count("title")) fields_["title"]->SetValue(DB_STR(s.title));
        if (fields_.count("original_owner")) fields_["original_owner"]->SetValue(DB_STR(s.original_owner));
        if (fields_.count("current_owner")) fields_["current_owner"]->SetValue(DB_STR(s.current_owner));
        if (fields_.count("handler")) fields_["handler"]->SetValue(DB_STR(s.handler));
        if (fields_.count("developer")) fields_["developer"]->SetValue(DB_STR(s.developer));
        if (fields_.count("inventor")) fields_["inventor"]->SetValue(DB_STR(s.inventor));
        if (fields_.count("dev_complete_date")) fields_["dev_complete_date"]->SetValue(DB_STR(s.dev_complete_date));
        if (fields_.count("application_date")) fields_["application_date"]->SetValue(DB_STR(s.application_date));
        if (fields_.count("reg_date")) fields_["reg_date"]->SetValue(DB_STR(s.reg_date));
        if (fields_.count("version")) fields_["version"]->SetValue(DB_STR(s.version));
        status_combo_->SetValue(DB_STR(s.application_status));
    }

    void OnSave(wxCommandEvent&) {
        SoftwareCopyright s;
        s.id = sw_id_;
        s.case_no = fields_["case_no"]->GetValue().ToStdString();
        s.reg_no = fields_["reg_no"]->GetValue().ToStdString();
        s.title = fields_["title"]->GetValue().ToStdString();
        s.original_owner = fields_["original_owner"]->GetValue().ToStdString();
        s.current_owner = fields_["current_owner"]->GetValue().ToStdString();
        s.application_status = status_combo_->GetValue().ToStdString();
        s.handler = fields_["handler"]->GetValue().ToStdString();
        s.developer = fields_["developer"]->GetValue().ToStdString();
        s.inventor = fields_["inventor"]->GetValue().ToStdString();

        if (s.case_no.empty()) {
            wxMessageBox("Case No is required", "Error", wxOK | wxICON_ERROR);
            return;
        }

        int new_id = db_->InsertSoftware(s);
        if (new_id > 0) {
            EndModal(wxID_OK);
        } else {
            wxMessageBox("Failed to save", "Error", wxOK | wxICON_ERROR);
        }
    }
};

// ============== IC Layout Edit Dialog ==============
class ICEditDialog : public wxDialog {
public:
    ICEditDialog(wxWindow* parent, Database* db, int ic_id = 0)
        : wxDialog(parent, wxID_ANY, ic_id ? "Edit IC Layout" : "New IC Layout", wxDefaultPosition, wxSize(500, 400)),
          db_(db), ic_id_(ic_id) {
        SetupUI();
        if (ic_id) LoadData();
    }
private:
    Database* db_;
    int ic_id_;
    std::map<std::string, wxTextCtrl*> fields_;
    wxComboBox* status_combo_;

    void SetupUI() {
        wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
        wxPanel* panel = new wxPanel(this);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        auto add_row = [&](const wxString& label, const std::string& key, int w = 200) {
            wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(panel, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            wxTextCtrl* tc = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(w, -1));
            fields_[key] = tc;
            row->Add(tc, 1);
            sizer->Add(row, 0, wxEXPAND | wxALL, 3);
        };

        add_row("Case No:", "case_no", 120);
        add_row("Reg No:", "reg_no", 120);
        add_row("Title:", "title", 350);
        add_row("Original Owner:", "original_owner", 200);
        add_row("Current Owner:", "current_owner", 200);

        wxBoxSizer* status_row = new wxBoxSizer(wxHORIZONTAL);
        status_row->Add(new wxStaticText(panel, wxID_ANY, "Status:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        status_combo_ = new wxComboBox(panel, wxID_ANY, "pending");
        status_combo_->Append("pending");
        status_combo_->Append("registered");
        status_combo_->Append("rejected");
        status_row->Add(status_combo_, 1);
        sizer->Add(status_row, 0, wxEXPAND | wxALL, 3);

        add_row("Handler:", "handler", 100);
        add_row("Designer:", "designer", 150);
        add_row("Inventor:", "inventor", 150);
        add_row("Application Date:", "application_date", 100);
        add_row("Creation Date:", "creation_date", 100);
        add_row("Cert Date:", "cert_date", 100);

        panel->SetSizer(sizer);
        main_sizer->Add(panel, 1, wxEXPAND | wxALL, 10);

        wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton* save_btn = new wxButton(this, wxID_OK, "Save");
        wxButton* cancel_btn = new wxButton(this, wxID_CANCEL, "Cancel");
        btn_sizer->AddStretchSpacer();
        btn_sizer->Add(save_btn, 0, wxRIGHT, 10);
        btn_sizer->Add(cancel_btn, 0);
        main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

        SetSizer(main_sizer);
        Centre();
        Bind(wxEVT_BUTTON, &ICEditDialog::OnSave, this, wxID_OK);
    }

    void LoadData() {
        ICLayout ic = db_->GetICById(ic_id_);
        if (fields_.count("case_no")) fields_["case_no"]->SetValue(DB_STR(ic.case_no));
        if (fields_.count("reg_no")) fields_["reg_no"]->SetValue(DB_STR(ic.reg_no));
        if (fields_.count("title")) fields_["title"]->SetValue(DB_STR(ic.title));
        if (fields_.count("original_owner")) fields_["original_owner"]->SetValue(DB_STR(ic.original_owner));
        if (fields_.count("current_owner")) fields_["current_owner"]->SetValue(DB_STR(ic.current_owner));
        if (fields_.count("handler")) fields_["handler"]->SetValue(DB_STR(ic.handler));
        if (fields_.count("designer")) fields_["designer"]->SetValue(DB_STR(ic.designer));
        if (fields_.count("inventor")) fields_["inventor"]->SetValue(DB_STR(ic.inventor));
        if (fields_.count("application_date")) fields_["application_date"]->SetValue(DB_STR(ic.application_date));
        if (fields_.count("creation_date")) fields_["creation_date"]->SetValue(DB_STR(ic.creation_date));
        if (fields_.count("cert_date")) fields_["cert_date"]->SetValue(DB_STR(ic.cert_date));
        if (fields_.count("notes")) fields_["notes"]->SetValue(DB_STR(ic.notes));
        status_combo_->SetValue(DB_STR(ic.application_status));
    }

    void OnSave(wxCommandEvent&) {
        ICLayout ic;
        ic.id = ic_id_;
        ic.case_no = fields_["case_no"]->GetValue().ToStdString();
        ic.reg_no = fields_["reg_no"]->GetValue().ToStdString();
        ic.title = fields_["title"]->GetValue().ToStdString();
        ic.original_owner = fields_["original_owner"]->GetValue().ToStdString();
        ic.current_owner = fields_["current_owner"]->GetValue().ToStdString();
        ic.application_status = status_combo_->GetValue().ToStdString();
        ic.handler = fields_["handler"]->GetValue().ToStdString();
        ic.designer = fields_["designer"]->GetValue().ToStdString();
        ic.inventor = fields_["inventor"]->GetValue().ToStdString();

        if (ic.case_no.empty()) {
            wxMessageBox("Case No is required", "Error", wxOK | wxICON_ERROR);
            return;
        }

        int new_id = db_->InsertIC(ic);
        if (new_id > 0) {
            EndModal(wxID_OK);
        } else {
            wxMessageBox("Failed to save", "Error", wxOK | wxICON_ERROR);
        }
    }
};

// ============== Foreign Patent Edit Dialog ==============
class ForeignEditDialog : public wxDialog {
public:
    ForeignEditDialog(wxWindow* parent, Database* db, int fp_id = 0)
        : wxDialog(parent, wxID_ANY, fp_id ? "Edit Foreign Patent" : "New Foreign Patent", wxDefaultPosition, wxSize(550, 400)),
          db_(db), fp_id_(fp_id) {
        SetupUI();
        if (fp_id) LoadData();
    }
private:
    Database* db_;
    int fp_id_;
    std::map<std::string, wxTextCtrl*> fields_;
    wxComboBox* status_combo_;

    void SetupUI() {
        wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
        wxPanel* panel = new wxPanel(this);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        auto add_row = [&](const wxString& label, const std::string& key, int w = 200) {
            wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(panel, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            wxTextCtrl* tc = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(w, -1));
            fields_[key] = tc;
            row->Add(tc, 1);
            sizer->Add(row, 0, wxEXPAND | wxALL, 3);
        };

        add_row("Case No:", "case_no", 120);
        add_row("PCT No:", "pct_no", 150);
        add_row("Country App No:", "country_app_no", 150);
        add_row("Title:", "title", 350);
        add_row("Owner:", "owner", 200);

        wxBoxSizer* status_row = new wxBoxSizer(wxHORIZONTAL);
        status_row->Add(new wxStaticText(panel, wxID_ANY, "Status:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        status_combo_ = new wxComboBox(panel, wxID_ANY, "pending");
        status_combo_->Append("pending");
        status_combo_->Append("granted");
        status_combo_->Append("rejected");
        status_combo_->Append("abandoned");
        status_row->Add(status_combo_, 1);
        sizer->Add(status_row, 0, wxEXPAND | wxALL, 3);

        add_row("Handler:", "handler", 100);
        add_row("Inventor:", "inventor", 200);
        add_row("Country:", "country", 80);
        add_row("Application No:", "application_no", 150);
        add_row("Application Date:", "application_date", 100);
        add_row("Authorization Date:", "authorization_date", 100);

        panel->SetSizer(sizer);
        main_sizer->Add(panel, 1, wxEXPAND | wxALL, 10);

        wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton* save_btn = new wxButton(this, wxID_OK, "Save");
        wxButton* cancel_btn = new wxButton(this, wxID_CANCEL, "Cancel");
        btn_sizer->AddStretchSpacer();
        btn_sizer->Add(save_btn, 0, wxRIGHT, 10);
        btn_sizer->Add(cancel_btn, 0);
        main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

        SetSizer(main_sizer);
        Centre();
        Bind(wxEVT_BUTTON, &ForeignEditDialog::OnSave, this, wxID_OK);
    }

    void LoadData() {
        ForeignPatent f = db_->GetForeignById(fp_id_);
        if (fields_.count("case_no")) fields_["case_no"]->SetValue(DB_STR(f.case_no));
        if (fields_.count("pct_no")) fields_["pct_no"]->SetValue(DB_STR(f.pct_no));
        if (fields_.count("country_app_no")) fields_["country_app_no"]->SetValue(DB_STR(f.country_app_no));
        if (fields_.count("title")) fields_["title"]->SetValue(DB_STR(f.title));
        if (fields_.count("owner")) fields_["owner"]->SetValue(DB_STR(f.owner));
        if (fields_.count("handler")) fields_["handler"]->SetValue(DB_STR(f.handler));
        if (fields_.count("inventor")) fields_["inventor"]->SetValue(DB_STR(f.inventor));
        if (fields_.count("country")) fields_["country"]->SetValue(DB_STR(f.country));
        if (fields_.count("application_no")) fields_["application_no"]->SetValue(DB_STR(f.application_no));
        if (fields_.count("application_date")) fields_["application_date"]->SetValue(DB_STR(f.application_date));
        if (fields_.count("authorization_date")) fields_["authorization_date"]->SetValue(DB_STR(f.authorization_date));
        if (fields_.count("notes")) fields_["notes"]->SetValue(DB_STR(f.notes));
        status_combo_->SetValue(DB_STR(f.patent_status));
    }

    void OnSave(wxCommandEvent&) {
        ForeignPatent f;
        f.id = fp_id_;
        f.case_no = fields_["case_no"]->GetValue().ToStdString();
        f.pct_no = fields_["pct_no"]->GetValue().ToStdString();
        f.country_app_no = fields_["country_app_no"]->GetValue().ToStdString();
        f.title = fields_["title"]->GetValue().ToStdString();
        f.owner = fields_["owner"]->GetValue().ToStdString();
        f.patent_status = status_combo_->GetValue().ToStdString();
        f.handler = fields_["handler"]->GetValue().ToStdString();
        f.inventor = fields_["inventor"]->GetValue().ToStdString();
        f.country = fields_["country"]->GetValue().ToStdString();
        f.application_no = fields_["application_no"]->GetValue().ToStdString();

        if (f.case_no.empty()) {
            wxMessageBox("Case No is required", "Error", wxOK | wxICON_ERROR);
            return;
        }

        int new_id = db_->InsertForeign(f);
        if (new_id > 0) {
            EndModal(wxID_OK);
        } else {
            wxMessageBox("Failed to save", "Error", wxOK | wxICON_ERROR);
        }
    }
};

// ============== Main Frame ==============
class PatXFrame : public wxFrame {
public:
    PatXFrame() : wxFrame(nullptr, wxID_ANY, "patX - Patent Manager v0.3.0", wxDefaultPosition, wxSize(1600, 900)) {
        db = std::make_unique<Database>("patents.db");
        SetupMenu();
        SetupUI();
        LoadAllData();
    }

private:
    std::unique_ptr<Database> db;
    wxAuiNotebook* notebook;
    wxStatusBar* status_bar;

    // Theme
    int current_theme = 0; // 0=light, 1=dark, 2=eye

    // Patent tab
    wxListCtrl* patent_list;
    wxTextCtrl* common_search;
    wxComboBox* common_status_filter;
    wxComboBox* common_handler_filter;
    wxComboBox* common_level_filter;  // 新增：通用等级筛选
    wxStaticText* lbl_level;           // 新增：等级标签
    wxTextCtrl* patent_detail;

    // OA tab
    wxListCtrl* oa_list;
    wxComboBox* oa_filter;

    // PCT tab
    wxListCtrl* pct_list;

    // Software tab
    wxListCtrl* sw_list;

    // IC tab
    wxListCtrl* ic_list;

    // Foreign tab
    wxListCtrl* foreign_list;

    // Annual fee tab
    wxListCtrl* fee_list;

    // Deadline rules tab
    wxListCtrl* rule_list;

    // Sorting state per list (column, ascending)
    std::map<wxListCtrl*, std::pair<int, bool>> sort_state;

    void SetupMenu() {
        wxMenuBar* mb = new wxMenuBar;

        // File menu
        wxMenu* file_menu = new wxMenu;
        file_menu->Append(wxID_NEW, LANG_STR("&New Patent\tCtrl+N", "新建专利(&N)\tCtrl+N"));
        file_menu->Append(wxID_OPEN, LANG_STR("&Import...", "导入(&I)..."));
        file_menu->Append(wxID_SAVE, LANG_STR("&Export...", "导出(&E)..."));
        file_menu->AppendSeparator();
        file_menu->Append(ID_SWITCH_DB, LANG_STR("Switch Database...", "切换数据库..."));
        file_menu->Append(ID_AUTO_BACKUP, LANG_STR("Auto Backup Settings...", "自动备份设置..."));
        file_menu->AppendSeparator();
        file_menu->Append(wxID_EXIT, LANG_STR("E&xit\tAlt+F4", "退出(&X)\tAlt+F4"));
        mb->Append(file_menu, LANG_STR("&File", "文件(&F)"));

        // Edit menu
        wxMenu* edit_menu = new wxMenu;
        edit_menu->Append(wxID_UNDO, LANG_STR("&Undo\tCtrl+Z", "撤销(&U)\tCtrl+Z"));
        edit_menu->Append(wxID_REDO, LANG_STR("&Redo\tCtrl+Y", "重做(&R)\tCtrl+Y"));
        edit_menu->AppendSeparator();
        edit_menu->Append(wxID_EDIT, LANG_STR("&Edit Selected\tEnter", "编辑(&E)\tEnter"));
        edit_menu->Append(wxID_DELETE, LANG_STR("&Delete\tDel", "删除(&D)\tDel"));
        mb->Append(edit_menu, LANG_STR("&Edit", "编辑(&E)"));

        // View menu
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

        // Tools menu
        wxMenu* tools_menu = new wxMenu;
        tools_menu->Append(ID_STATISTICS, LANG_STR("&Statistics...", "统计(&S)..."));
        tools_menu->Append(ID_VALIDATE_DATA, LANG_STR("Validate Data", "数据验证"));
        tools_menu->AppendSeparator();
        tools_menu->Append(ID_SYNC, LANG_STR("&Sync with NAS", "NAS同步(&N)"));
        tools_menu->Append(ID_NAS_CONFIG, LANG_STR("NAS &Configuration...", "NAS配置(&C)..."));
        tools_menu->AppendSeparator();
        tools_menu->Append(ID_BACKUP, LANG_STR("&Backup Database", "备份数据库(&B)"));
        tools_menu->Append(ID_RESTORE, LANG_STR("&Restore Backup...", "恢复备份(&R)..."));
        mb->Append(tools_menu, LANG_STR("&Tools", "工具(&T)"));

        // Help menu
        wxMenu* help_menu = new wxMenu;
        help_menu->Append(wxID_ABOUT, LANG_STR("&About", "关于(&A)"));
        mb->Append(help_menu, LANG_STR("&Help", "帮助(&H)"));

        SetMenuBar(mb);

        // Bind events
        Bind(wxEVT_MENU, &PatXFrame::OnExit, this, wxID_EXIT);
        Bind(wxEVT_MENU, &PatXFrame::OnNewPatent, this, wxID_NEW);
        Bind(wxEVT_MENU, &PatXFrame::OnEditPatent, this, wxID_EDIT);
        Bind(wxEVT_MENU, &PatXFrame::OnDeletePatent, this, wxID_DELETE);
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
            // Validate data - check for issues
            int issues = 0;
            wxString report;

            auto patents = db->GetPatents();

            // Check empty fields
            for (const auto& p : patents) {
                if (p.geke_code.empty()) {
                    report += UTF8_STR("专利ID ") + std::to_string(p.id) + UTF8_STR(" 编号为空\n");
                    issues++;
                }
                if (p.title.empty()) {
                    report += UTF8_STR("专利 ") + DB_STR(p.geke_code) + UTF8_STR(" 名称为空\n");
                    issues++;
                }
            }

            // Check OA deadlines
            auto oas = db->GetOARecords();
            for (const auto& oa : oas) {
                if (oa.official_deadline.empty() && !oa.is_completed) {
                    report += UTF8_STR("OA缺少绝限日期: ") + DB_STR(oa.geke_code) + " - " + DB_STR(oa.oa_type) + "\n";
                    issues++;
                }
            }

            if (issues == 0) {
                wxMessageBox(current_lang == 0 ?
                    "All data validated successfully!\nNo issues found." :
                    UTF8_STR("数据验证成功！\n未发现问题。"),
                    current_lang == 0 ? "Data Validation" : UTF8_STR("数据验证"), wxOK | wxICON_INFORMATION);
            } else {
                wxMessageBox(report, wxString::Format(current_lang == 0 ?
                    "Found %d issues:" : UTF8_STR("发现 %d 个问题:"), issues), wxOK | wxICON_WARNING);
            }
        }, ID_VALIDATE_DATA);
        Bind(wxEVT_MENU, &PatXFrame::OnSwitchDatabase, this, ID_SWITCH_DB);
        Bind(wxEVT_MENU, &PatXFrame::OnAutoBackup, this, ID_AUTO_BACKUP);
        Bind(wxEVT_MENU, &PatXFrame::OnAbout, this, wxID_ABOUT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { SetTheme(0); }, ID_THEME_LIGHT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { SetTheme(1); }, ID_THEME_DARK);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { SetTheme(2); }, ID_THEME_EYE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { SetLanguage(0); }, ID_LANG_EN);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { SetLanguage(1); }, ID_LANG_ZH);
    }

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
        ID_STATISTICS
    };
    
    int current_lang = 0; // 0=English, 1=Chinese

    // Toolbar buttons - stored for language switching
    std::vector<wxButton*> toolbar_btns;
    wxButton* btn_new = nullptr;
    wxButton* btn_edit = nullptr;
    wxButton* btn_delete = nullptr;
    wxButton* btn_batch = nullptr;
    wxStaticText* lbl_search = nullptr;
    wxStaticText* lbl_status = nullptr;
    wxStaticText* lbl_handler = nullptr;

    void SetupUI() {
        wxPanel* main_panel = new wxPanel(this);
        wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

        // Top toolbar
        wxBoxSizer* top_bar = new wxBoxSizer(wxHORIZONTAL);

        wxStaticText* title = new wxStaticText(main_panel, wxID_ANY, "Patent Data Management");
        title->SetFont(wxFont(16, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
        top_bar->Add(title, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 20);
        top_bar->Add(new wxStaticText(main_panel, wxID_ANY, "Database: patents.db"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 20);
        top_bar->AddStretchSpacer();

        // Toolbar buttons
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

        // Notebook with tabs - use simple wxNotebook
        notebook = new wxAuiNotebook(main_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                      wxAUI_NB_TOP | wxAUI_NB_TAB_SPLIT | wxAUI_NB_TAB_MOVE);

        // Bind tab change event to update filters
        notebook->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
            int tab = e.GetSelection();
            // Update status filter based on current tab
            wxString current_status = common_status_filter->GetValue();
            common_status_filter->Clear();
            common_status_filter->Append(current_lang == 0 ? "All" : UTF8_STR("全部"));

            std::string status_table, status_col;
            switch (tab) {
                case 0: status_table = "patents"; status_col = "application_status"; break;
                case 1: status_table = "oa_records"; status_col = "oa_type"; break;
                case 2: status_table = "pct_patents"; status_col = "application_status"; break;
                case 3: status_table = "software_copyrights"; status_col = "application_status"; break;
                case 4: status_table = "ic_layouts"; status_col = "application_status"; break;
                case 5: status_table = "foreign_patents"; status_col = "patent_status"; break;
                default: status_table = "patents"; status_col = "application_status"; break;
            }
            auto statuses = db->GetDistinctValues(status_table, status_col);
            for (const auto& s : statuses) {
                if (!s.empty()) common_status_filter->Append(DB_STR(s));
            }
            common_status_filter->SetValue(current_status.IsEmpty() ? (current_lang == 0 ? "All" : UTF8_STR("全部")) : current_status);

            // Update handler filter based on current tab
            wxString current_handler = common_handler_filter->GetValue();
            common_handler_filter->Clear();
            common_handler_filter->Append(current_lang == 0 ? "All" : UTF8_STR("全部"));

            std::string handler_table, handler_col;
            switch (tab) {
                case 0: handler_table = "patents"; handler_col = "geke_handler"; break;
                case 1: handler_table = "oa_records"; handler_col = "handler"; break;
                case 2: handler_table = "pct_patents"; handler_col = "handler"; break;
                case 3: handler_table = "software_copyrights"; handler_col = "handler"; break;
                case 4: handler_table = "ic_layouts"; handler_col = "handler"; break;
                case 5: handler_table = "foreign_patents"; handler_col = "handler"; break;
                default: handler_table = "patents"; handler_col = "geke_handler"; break;
            }
            auto handlers = db->GetDistinctValues(handler_table, handler_col);
            for (const auto& h : handlers) {
                if (!h.empty()) common_handler_filter->Append(DB_STR(h));
            }
            common_handler_filter->SetValue(current_handler.IsEmpty() ? (current_lang == 0 ? "All" : UTF8_STR("全部")) : current_handler);

            // Update level filter (only for patents tab)
            wxString current_level = common_level_filter->GetValue();
            common_level_filter->Clear();
            common_level_filter->Append(current_lang == 0 ? "All" : UTF8_STR("全部"));
            if (tab == 0) {
                common_level_filter->Append(current_lang == 0 ? "Core" : UTF8_STR("核心"));
                common_level_filter->Append(current_lang == 0 ? "Important" : UTF8_STR("重要"));
                common_level_filter->Append(current_lang == 0 ? "Normal" : UTF8_STR("一般"));
            }
            common_level_filter->SetValue(current_level.IsEmpty() ? (current_lang == 0 ? "All" : UTF8_STR("全部")) : current_level);
            lbl_level->Show(tab == 0);
            common_level_filter->Show(tab == 0);

            e.Skip();
        });

        // Make sure notebook has a proper size
        notebook->SetMinSize(wxSize(800, 600));

        // ===== Common toolbar above notebook =====
        wxPanel* toolbar_panel = new wxPanel(main_panel);
        wxBoxSizer* toolbar_sizer = new wxBoxSizer(wxHORIZONTAL);

        // Action buttons
        btn_new = new wxButton(toolbar_panel, wxID_ANY, LANG_STR("New", UTF8_STR("新建")));
        btn_new->Bind(wxEVT_BUTTON, &PatXFrame::OnNewByCurrentTab, this);
        toolbar_sizer->Add(btn_new, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        btn_edit = new wxButton(toolbar_panel, wxID_ANY, LANG_STR("Edit", UTF8_STR("编辑")));
        btn_edit->Bind(wxEVT_BUTTON, &PatXFrame::OnEditByCurrentTab, this);
        toolbar_sizer->Add(btn_edit, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        btn_delete = new wxButton(toolbar_panel, wxID_ANY, LANG_STR("Delete", UTF8_STR("删除")));
        btn_delete->Bind(wxEVT_BUTTON, &PatXFrame::OnDeleteByCurrentTab, this);
        toolbar_sizer->Add(btn_delete, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        toolbar_sizer->AddSpacer(15);

        btn_batch = new wxButton(toolbar_panel, wxID_ANY, LANG_STR("Batch", UTF8_STR("批量")));
        btn_batch->Bind(wxEVT_BUTTON, &PatXFrame::OnBatchByCurrentTab, this);
        toolbar_sizer->Add(btn_batch, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        toolbar_sizer->AddSpacer(20);

        // Search and filters
        lbl_search = new wxStaticText(toolbar_panel, wxID_ANY, LANG_STR("Search:", UTF8_STR("搜索:")));
        toolbar_sizer->Add(lbl_search, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        common_search = new wxTextCtrl(toolbar_panel, wxID_ANY, "", wxDefaultPosition, wxSize(200, -1), wxTE_PROCESS_ENTER);
        common_search->Bind(wxEVT_TEXT_ENTER, &PatXFrame::OnSearchByCurrentTab, this);
        toolbar_sizer->Add(common_search, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        toolbar_sizer->AddSpacer(10);

        lbl_status = new wxStaticText(toolbar_panel, wxID_ANY, LANG_STR("Status:", UTF8_STR("状态:")));
        toolbar_sizer->Add(lbl_status, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        common_status_filter = new wxComboBox(toolbar_panel, wxID_ANY, LANG_STR("All", UTF8_STR("全部")), wxDefaultPosition, wxSize(100, -1));
        common_status_filter->Append(LANG_STR("All", UTF8_STR("全部")));
        common_status_filter->Append(LANG_STR("Pending", UTF8_STR("审查中")));
        common_status_filter->Append(LANG_STR("Granted", UTF8_STR("已授权")));
        common_status_filter->Append(LANG_STR("Rejected", UTF8_STR("已驳回")));
        common_status_filter->Bind(wxEVT_COMBOBOX, &PatXFrame::OnFilterByCurrentTab, this);
        toolbar_sizer->Add(common_status_filter, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        toolbar_sizer->AddSpacer(10);

        lbl_handler = new wxStaticText(toolbar_panel, wxID_ANY, LANG_STR("Handler:", UTF8_STR("处理人:")));
        toolbar_sizer->Add(lbl_handler, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        common_handler_filter = new wxComboBox(toolbar_panel, wxID_ANY, LANG_STR("All", UTF8_STR("全部")), wxDefaultPosition, wxSize(100, -1));
        common_handler_filter->Append(LANG_STR("All", UTF8_STR("全部")));
        common_handler_filter->Bind(wxEVT_COMBOBOX, &PatXFrame::OnFilterByCurrentTab, this);
        toolbar_sizer->Add(common_handler_filter, 0, wxRIGHT | wxTOP | wxBOTTOM, 3);

        toolbar_sizer->AddSpacer(10);

        // 等级筛选
        lbl_level = new wxStaticText(toolbar_panel, wxID_ANY, LANG_STR("Level:", UTF8_STR("等级:")));
        toolbar_sizer->Add(lbl_level, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        common_level_filter = new wxComboBox(toolbar_panel, wxID_ANY, LANG_STR("All", UTF8_STR("全部")), wxDefaultPosition, wxSize(100, -1));
        common_level_filter->Append(LANG_STR("All", UTF8_STR("全部")));
        common_level_filter->Append(LANG_STR("Core", UTF8_STR("核心")));
        common_level_filter->Append(LANG_STR("Important", UTF8_STR("重要")));
        common_level_filter->Append(LANG_STR("Normal", UTF8_STR("一般")));
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
        SetupAnnualFeeTab();
        SetupDeadlineRulesTab();

        main_sizer->Add(notebook, 1, wxEXPAND | wxALL, 5);

        status_bar = CreateStatusBar();
        status_bar->SetStatusText("patX v1.0.0 | Database: patents.db | Press F1 for help");

        main_panel->SetSizer(main_sizer);
        
        // Force layout update
        main_sizer->SetSizeHints(this);
        SetSize(wxSize(1400, 900));
        Centre();
    }

    // ============== Patent Tab ==============
    void SetupPatentTab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        // Toolbar: only patent-specific filters (发明人) + export buttons
        wxBoxSizer* tb1 = new wxBoxSizer(wxHORIZONTAL);

        // 发明人筛选 (专利特有)
        tb1->Add(new wxStaticText(panel, wxID_ANY, UTF8_STR("发明人:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        wxComboBox* inventor_filter = new wxComboBox(panel, wxID_ANY, UTF8_STR("全部"), wxDefaultPosition, wxSize(120, -1));
        inventor_filter->Append(UTF8_STR("全部"));
        auto inventors = db->GetDistinctValues("patents", "inventor");
        for (const auto& inv : inventors) {
            if (!inv.empty()) inventor_filter->Append(DB_STR(inv));
        }
        inventor_filter->Bind(wxEVT_COMBOBOX, [this, inventor_filter](wxCommandEvent&) {
            LoadPatentsWithInventor(inventor_filter->GetValue());
        });
        tb1->Add(inventor_filter, 0, wxRIGHT, 10);

        wxButton* export_btn = new wxButton(panel, wxID_ANY, LANG_STR("Export CSV", UTF8_STR("导出CSV")));
        export_btn->Bind(wxEVT_BUTTON, &PatXFrame::OnExport, this);
        tb1->Add(export_btn, 0, wxRIGHT, 5);

        wxButton* report_btn = new wxButton(panel, wxID_ANY, "Report");
        report_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxFileDialog dlg(this, "Export Report", "", "patent_report.txt",
                             "Text files (*.txt)|*.txt|CSV (*.csv)|*.csv", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (dlg.ShowModal() == wxID_OK) {
                auto patents = db->GetPatents();
                wxTextFile file(dlg.GetPath());
                file.Create();

                file.AddLine("=== Patent Management Report ===");
                file.AddLine(wxString::Format("Generated: %s", wxDateTime::Now().FormatDate()));
                file.AddLine(wxString::Format("Total Patents: %zu", patents.size()));
                file.AddLine("");

                int pending=0, granted=0, rejected=0, core=0;
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
                    file.AddLine(DB_STR(p.geke_code) + " | " + DB_STR(p.title) + " | " + DB_STR(p.patent_type) + " | " + DB_STR(p.patent_level) + " | " + DB_STR(p.application_status));
                }

                file.Write();
                file.Close();
                wxMessageBox("Report exported!", "Done", wxOK | wxICON_INFORMATION);
            }
        });
        tb1->Add(report_btn, 0, wxRIGHT, 5);

        tb1->AddStretchSpacer();
        sizer->Add(tb1, 0, wxALL, 5);

        // Batch operations toolbar
        wxBoxSizer* tb2 = new wxBoxSizer(wxHORIZONTAL);
        tb2->Add(new wxStaticText(panel, wxID_ANY, "Batch:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

        wxButton* batch_level_btn = new wxButton(panel, wxID_ANY, "Set Level");
        batch_level_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            std::vector<long> selected;
            long idx = -1;
            while ((idx = patent_list->GetNextItem(idx, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) >= 0) {
                selected.push_back(idx);
            }
            if (selected.empty()) { wxMessageBox("Select patents first", "Info", wxOK); return; }

            wxArrayString levels; levels.Add("core"); levels.Add("important"); levels.Add("normal");
            int choice = wxGetSingleChoiceIndex("Select level:", "Batch Set Level", levels, this);
            if (choice >= 0) {
                wxString new_level = levels[choice];
                for (long i : selected) {
                    int id = patent_list->GetItemData(i);
                    Patent p = db->GetPatentById(id);
                    p.patent_level = new_level.ToStdString();
                    db->UpdatePatent(id, p);
                }
                LoadPatents();
                wxMessageBox(wxString::Format("Updated %zu patents", selected.size()), "Done", wxOK);
            }
        });
        tb2->Add(batch_level_btn, 0, wxRIGHT, 5);

        wxButton* batch_status_btn = new wxButton(panel, wxID_ANY, "Set Status");
        batch_status_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            std::vector<long> selected;
            long idx = -1;
            while ((idx = patent_list->GetNextItem(idx, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) >= 0) {
                selected.push_back(idx);
            }
            if (selected.empty()) { wxMessageBox("Select patents first", "Info", wxOK); return; }

            wxArrayString statuses; statuses.Add("pending"); statuses.Add("granted"); statuses.Add("rejected");
            int choice = wxGetSingleChoiceIndex("Select status:", "Batch Set Status", statuses, this);
            if (choice >= 0) {
                wxString new_status = statuses[choice];
                for (long i : selected) {
                    int id = patent_list->GetItemData(i);
                    Patent p = db->GetPatentById(id);
                    p.application_status = new_status.ToStdString();
                    if (new_status == "granted" && !p.application_date.empty() && p.expiration_date.empty()) {
                        int years = (p.patent_type == "invention") ? 20 : 10;
                        int app_year = atoi(p.application_date.substr(0, 4).c_str());
                        p.expiration_date = wxString::Format("%d-%s", app_year + years, p.application_date.substr(5)).ToStdString();
                    }
                    db->UpdatePatent(id, p);
                }
                LoadPatents();
                wxMessageBox(wxString::Format("Updated %zu patents", selected.size()), "Done", wxOK);
            }
        });
        tb2->Add(batch_status_btn, 0, wxRIGHT, 5);

        wxButton* calc_exp_btn = new wxButton(panel, wxID_ANY, "Calc Expiration");
        calc_exp_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            auto patents = db->GetPatents();
            int updated = 0;
            for (const auto& p : patents) {
                if (p.application_status == "granted" && !p.application_date.empty() && p.expiration_date.empty()) {
                    Patent up = p;
                    int years = (p.patent_type == "invention") ? 20 : 10;
                    int app_year = atoi(p.application_date.substr(0, 4).c_str());
                    up.expiration_date = wxString::Format("%d-%s", app_year + years, p.application_date.substr(5)).ToStdString();
                    db->UpdatePatent(p.id, up);
                    updated++;
                }
            }
            LoadPatents();
            wxMessageBox(wxString::Format("Calculated %d expiration dates\nInvention: 20y, Utility/Design: 10y", updated), "Done", wxOK);
        });
        tb2->Add(calc_exp_btn, 0);
        sizer->Add(tb2, 0, wxALL, 5);

        // Splitter: list + details
        wxSplitterWindow* splitter = new wxSplitterWindow(panel, wxID_ANY);

        patent_list = new wxListCtrl(splitter, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                      wxLC_REPORT);
        patent_list->AppendColumn(UTF8_STR("编码"), wxLIST_FORMAT_LEFT, 90);
        patent_list->AppendColumn(UTF8_STR("申请号"), wxLIST_FORMAT_LEFT, 130);
        patent_list->AppendColumn(UTF8_STR("发明名称"), wxLIST_FORMAT_LEFT, 220);
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
                int patent_id = patent_list->GetItemData(idx);
                PatentEditDialog dlg(this, db.get(), patent_id);
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

        // 列标题右键筛选
        patent_list->Bind(wxEVT_LIST_COL_RIGHT_CLICK, [this](wxListEvent& e) {
            int col = e.GetColumn();
            // 列到数据库字段的映射
            std::vector<std::string> col_to_field = {
                "geke_code", "application_number", "title", "patent_type",
                "patent_level", "application_status", "class_level1", "class_level2",
                "class_level3", "geke_handler", "inventor", "application_date",
                "authorization_date", "expiration_date", "agency_firm", "notes"
            };
            if (col < 0 || col >= (int)col_to_field.size()) return;

            wxMenu menu;
            menu.Append(10000, UTF8_STR("(全部)"));
            menu.AppendSeparator();

            auto values = db->GetDistinctValues("patents", col_to_field[col]);
            for (size_t i = 0; i < values.size() && i < 30; i++) {
                if (!values[i].empty()) {
                    menu.Append(10001 + i, DB_STR(values[i]));
                }
            }

            menu.Bind(wxEVT_MENU, [this, col, values](wxCommandEvent& ev) {
                int id = ev.GetId();
                if (id == 10000) {
                    g_column_filters[patent_list][col] = "";
                } else {
                    size_t idx = id - 10001;
                    if (idx < values.size()) {
                        g_column_filters[patent_list][col] = values[idx];
                    }
                }
                LoadPatents();
            });

            PopupMenu(&menu);
        });

        // Right-click context menu
        patent_list->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent& e) {
            long idx = patent_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);

            wxMenu menu;
            menu.Append(wxID_EDIT, LANG_STR("Edit", UTF8_STR("编辑")));
            menu.Append(wxID_DELETE, LANG_STR("Delete", UTF8_STR("删除")));
            menu.AppendSeparator();
            menu.Append(wxID_NEW, LANG_STR("New", UTF8_STR("新建")));
            menu.AppendSeparator();
            menu.Append(ID_SET_LEVEL, UTF8_STR("设置等级"));
            menu.Append(ID_COPY_CODE, UTF8_STR("复制编号"));
            menu.Append(ID_COPY_TITLE, UTF8_STR("复制标题"));

            menu.Bind(wxEVT_MENU, &PatXFrame::OnEditPatent, this, wxID_EDIT);
            menu.Bind(wxEVT_MENU, &PatXFrame::OnDeletePatent, this, wxID_DELETE);
            menu.Bind(wxEVT_MENU, &PatXFrame::OnNewPatent, this, wxID_NEW);
            menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
                // Set Level
                long idx = patent_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
                if (idx < 0) return;
                wxArrayString levels;
                levels.Add(UTF8_STR("核心专利"));
                levels.Add(UTF8_STR("重要专利"));
                levels.Add(UTF8_STR("一般专利"));
                int choice = wxGetSingleChoiceIndex(UTF8_STR("选择专利等级:"), UTF8_STR("设置等级"), levels, this);
                if (choice >= 0) {
                    int id = patent_list->GetItemData(idx);
                    Patent p = db->GetPatentById(id);
                    p.patent_level = levels[choice].ToUTF8().data();
                    db->UpdatePatent(id, p);
                    patent_list->SetItem(idx, 4, levels[choice]);
                    // Update color
                    if (choice == 0) patent_list->SetItemBackgroundColour(idx, wxColour(255, 210, 210));
                    else if (choice == 1) patent_list->SetItemBackgroundColour(idx, wxColour(255, 245, 200));
                    else patent_list->SetItemBackgroundColour(idx, *wxWHITE);
                }
            }, ID_SET_LEVEL);
            menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
                long idx = patent_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
                if (idx >= 0) {
                    wxString code = patent_list->GetItemText(idx, 0);
                    if (wxClipboard::Get()->Open()) {
                        wxClipboard::Get()->SetData(new wxTextDataObject(code));
                        wxClipboard::Get()->Close();
                    }
                }
            }, ID_COPY_CODE);
            menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
                long idx = patent_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
                if (idx >= 0) {
                    wxString title = patent_list->GetItemText(idx, 2);
                    if (wxClipboard::Get()->Open()) {
                        wxClipboard::Get()->SetData(new wxTextDataObject(title));
                        wxClipboard::Get()->Close();
                    }
                }
            }, ID_COPY_TITLE);

            PopupMenu(&menu);
        });

        wxPanel* detail_panel = new wxPanel(splitter);
        wxBoxSizer* detail_sizer = new wxBoxSizer(wxVERTICAL);
        detail_sizer->Add(new wxStaticText(detail_panel, wxID_ANY, "Patent Details"), 0, wxBOTTOM, 5);
        patent_detail = new wxTextCtrl(detail_panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                        wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH);
        patent_detail->SetFont(wxFont(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
        detail_sizer->Add(patent_detail, 1, wxEXPAND);
        
        // Quick action buttons
        wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton* copy_btn = new wxButton(detail_panel, wxID_ANY, "Copy Code");
        copy_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            long idx = patent_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (idx >= 0) {
                wxString code = patent_list->GetItemText(idx, 0);
                if (wxClipboard::Get()->Open()) {
                    wxClipboard::Get()->SetData(new wxTextDataObject(code));
                    wxClipboard::Get()->Close();
                    status_bar->SetStatusText(wxString::Format("Copied: %s", code));
                }
            }
        });
        wxButton* calc_btn = new wxButton(detail_panel, wxID_ANY, "Calc Days");
        calc_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            long idx = patent_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (idx >= 0) {
                Patent p = db->GetPatentById(patent_list->GetItemData(idx));
                if (!p.expiration_date.empty()) {
                    wxDateTime exp; exp.ParseFormat(p.expiration_date.c_str(), "%Y-%m-%d");
                    if (exp.IsValid()) {
                        int days = (exp - wxDateTime::Now()).GetDays();
                        wxMessageBox(wxString::Format("Days until expiration: %d", days), "Calculator", wxOK);
                    }
                }
            }
        });
        btn_sizer->Add(copy_btn, 0, wxRIGHT, 5);
        btn_sizer->Add(calc_btn, 0);
        detail_sizer->Add(btn_sizer, 0, wxTOP, 5);
        detail_panel->SetSizer(detail_sizer);

        splitter->SplitHorizontally(patent_list, detail_panel, 500);
        sizer->Add(splitter, 1, wxEXPAND | wxALL, 5);

        panel->SetSizer(sizer);
        notebook->AddPage(panel, LANG_STR("Domestic Patents", "国内专利"));
    }

    void LoadPatents() {
        if (!db || !db->IsOpen()) return;
        patent_list->DeleteAllItems();

        // Use common filters
        wxString status_f = common_status_filter->GetValue();
        wxString level_f = common_level_filter->GetValue();
        wxString handler_f = common_handler_filter->GetValue();

        auto isAll = [](const wxString& s) {
            return s == "All" || s == UTF8_STR("全部") || s.IsEmpty();
        };

        auto patents = db->GetPatents(
            isAll(status_f) ? "" : status_f.ToStdString(),
            isAll(level_f) ? "" : level_f.ToStdString(),
            isAll(handler_f) ? "" : handler_f.ToStdString()
        );

        int row = 0;
        for (const auto& p : patents) {
            long idx = patent_list->InsertItem(row, DB_STR(p.geke_code));
            patent_list->SetItem(idx, 1, DB_STR(p.application_number));
            patent_list->SetItem(idx, 2, DB_STR(p.title));
            patent_list->SetItem(idx, 3, DB_STR(p.patent_type));
            patent_list->SetItem(idx, 4, DB_STR(p.patent_level));
            patent_list->SetItem(idx, 5, DB_STR(p.application_status));
            patent_list->SetItem(idx, 6, DB_STR(p.class_level1));
            patent_list->SetItem(idx, 7, DB_STR(p.class_level2));
            patent_list->SetItem(idx, 8, DB_STR(p.class_level3));
            patent_list->SetItem(idx, 9, DB_STR(p.geke_handler));
            patent_list->SetItem(idx, 10, DB_STR(p.inventor));
            patent_list->SetItem(idx, 11, DB_STR(p.application_date));
            patent_list->SetItem(idx, 12, DB_STR(p.authorization_date));
            patent_list->SetItem(idx, 13, DB_STR(p.expiration_date));
            patent_list->SetItem(idx, 14, DB_STR(p.agency_firm));
            patent_list->SetItem(idx, 15, DB_STR(p.notes));
            patent_list->SetItemData(idx, p.id);

            // Color by level
            if (p.patent_level == "core" || p.patent_level.find("核心") != std::string::npos) {
                patent_list->SetItemBackgroundColour(idx, wxColour(255, 100, 100));
            } else if (p.patent_level == "important" || p.patent_level.find("重要") != std::string::npos) {
                patent_list->SetItemBackgroundColour(idx, wxColour(255, 230, 100));
            }
            row++;
        }
        status_bar->SetStatusText(wxString::Format("Domestic Patents: %d records", row));
    }

    void LoadPatentsWithInventor(const wxString& inventor_f) {
        if (!db || !db->IsOpen()) return;
        patent_list->DeleteAllItems();

        wxString status_f = common_status_filter->GetValue();
        wxString level_f = common_level_filter->GetValue();
        wxString handler_f = common_handler_filter->GetValue();

        auto isAll = [](const wxString& s) {
            return s == "All" || s == UTF8_STR("全部") || s.IsEmpty();
        };

        auto patents = db->GetPatents(
            isAll(status_f) ? "" : status_f.ToStdString(),
            isAll(level_f) ? "" : level_f.ToStdString(),
            isAll(handler_f) ? "" : handler_f.ToStdString()
        );

        int row = 0;
        for (const auto& p : patents) {
            if (!isAll(inventor_f) && p.inventor.find(inventor_f.ToStdString()) == std::string::npos) continue;
            long idx = patent_list->InsertItem(row, DB_STR(p.geke_code));
            patent_list->SetItem(idx, 1, DB_STR(p.application_number));
            patent_list->SetItem(idx, 2, DB_STR(p.title));
            patent_list->SetItem(idx, 3, DB_STR(p.patent_type));
            patent_list->SetItem(idx, 4, DB_STR(p.patent_level));
            patent_list->SetItem(idx, 5, DB_STR(p.application_status));
            patent_list->SetItem(idx, 6, DB_STR(p.class_level1));
            patent_list->SetItem(idx, 7, DB_STR(p.class_level2));
            patent_list->SetItem(idx, 8, DB_STR(p.class_level3));
            patent_list->SetItem(idx, 9, DB_STR(p.geke_handler));
            patent_list->SetItem(idx, 10, DB_STR(p.inventor));
            patent_list->SetItem(idx, 11, DB_STR(p.application_date));
            patent_list->SetItem(idx, 12, DB_STR(p.authorization_date));
            patent_list->SetItem(idx, 13, DB_STR(p.expiration_date));
            patent_list->SetItem(idx, 14, DB_STR(p.agency_firm));
            patent_list->SetItem(idx, 15, DB_STR(p.notes));
            patent_list->SetItemData(idx, p.id);

            if (p.patent_level == "core" || p.patent_level.find("核心") != std::string::npos) {
                patent_list->SetItemBackgroundColour(idx, wxColour(255, 100, 100));
            } else if (p.patent_level == "important" || p.patent_level.find("重要") != std::string::npos) {
                patent_list->SetItemBackgroundColour(idx, wxColour(255, 230, 100));
            }
            row++;
        }
        status_bar->SetStatusText(wxString::Format("Domestic Patents: %d records", row));
    }

    void SearchPatents() {
        wxString kw = common_search->GetValue();
        if (kw.IsEmpty()) { LoadPatents(); return; }

        patent_list->DeleteAllItems();
        auto patents = db->SearchPatents(kw.ToStdString());

        int row = 0;
        for (const auto& p : patents) {
            long idx = patent_list->InsertItem(row, DB_STR(p.geke_code));
            patent_list->SetItem(idx, 1, DB_STR(p.application_number));
            patent_list->SetItem(idx, 2, DB_STR(p.title));
            patent_list->SetItem(idx, 3, DB_STR(p.patent_type));
            patent_list->SetItem(idx, 4, DB_STR(p.patent_level));
            patent_list->SetItem(idx, 5, DB_STR(p.application_status));
            patent_list->SetItem(idx, 6, DB_STR(p.class_level1));
            patent_list->SetItem(idx, 7, DB_STR(p.class_level2));
            patent_list->SetItem(idx, 8, DB_STR(p.class_level3));
            patent_list->SetItem(idx, 9, DB_STR(p.geke_handler));
            patent_list->SetItem(idx, 10, DB_STR(p.inventor));
            patent_list->SetItem(idx, 11, DB_STR(p.application_date));
            patent_list->SetItem(idx, 12, DB_STR(p.authorization_date));
            patent_list->SetItem(idx, 13, DB_STR(p.expiration_date));
            patent_list->SetItem(idx, 14, DB_STR(p.agency_firm));
            patent_list->SetItem(idx, 15, DB_STR(p.notes));
            patent_list->SetItemData(idx, p.id);

            if (p.patent_level == "core" || p.patent_level.find("核心") != std::string::npos) {
                patent_list->SetItemBackgroundColour(idx, wxColour(255, 100, 100));
            } else if (p.patent_level == "important" || p.patent_level.find("重要") != std::string::npos) {
                patent_list->SetItemBackgroundColour(idx, wxColour(255, 230, 100));
            }
            row++;
        }
        status_bar->SetStatusText(wxString::Format("Search: %d results", row));
    }

    void OnPatentSelected(wxListEvent& event) {
        long idx = event.GetIndex();
        int id = patent_list->GetItemData(idx);
        Patent p = db->GetPatentById(id);
        auto oas = db->GetOAByPatent(p.geke_code);

        wxString oa_info;
        for (const auto& oa : oas) {
            oa_info += "\n  [" + DB_STR(oa.oa_type) + "] " + DB_STR(oa.progress) + " - Deadline: " + DB_STR(oa.official_deadline) + " - " + DB_STR(oa.handler) + (oa.is_completed ? " [DONE]" : "");
        }

        wxString info;
        info << UTF8_STR("编号: ") << DB_STR(p.geke_code) << "\n"
             << UTF8_STR("申请号: ") << DB_STR(p.application_number) << "\n"
             << UTF8_STR("名称: ") << DB_STR(p.title) << "\n\n"
             << DB_STR(p.patent_type) << " | " << DB_STR(p.patent_level) << " | " << DB_STR(p.application_status) << "\n\n"
             << UTF8_STR("处理人: ") << DB_STR(p.geke_handler) << " | " << UTF8_STR("发明人: ") << DB_STR(p.inventor) << "\n"
             << "R&D Dept: " << DB_STR(p.rd_department) << " | Agency: " << DB_STR(p.agency_firm) << "\n\n"
             << "Application Date: " << DB_STR(p.application_date) << "\n"
             << "Authorization Date: " << DB_STR(p.authorization_date) << "\n"
             << "Expiration Date: " << DB_STR(p.expiration_date) << "\n\n"
             << UTF8_STR("分类: ") << DB_STR(p.class_level1) << " / " << DB_STR(p.class_level2) << " / " << DB_STR(p.class_level3) << "\n\n"
             << UTF8_STR("申请人(原): ") << DB_STR(p.original_applicant) << "\n"
             << UTF8_STR("申请人(现): ") << DB_STR(p.current_applicant) << "\n\n"
             << UTF8_STR("备注: ") << (p.notes.empty() ? "None" : DB_STR(p.notes)) << "\n\n"
             << "─────────────────────────────────\n"
             << UTF8_STR("OA记录: ") << (oa_info.IsEmpty() ? "None" : oa_info);
        patent_detail->SetValue(info);
    }

    void OnNewPatent(wxCommandEvent& event) {
        PatentEditDialog dlg(this, db.get());
        if (dlg.ShowModal() == wxID_OK) {
            LoadPatents();
            wxMessageBox("Patent created successfully", "Success", wxOK | wxICON_INFORMATION);
        }
    }

    void OnEditPatent(wxCommandEvent& event) {
        long idx = patent_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (idx < 0) {
            wxMessageBox("Please select a patent first", "Info", wxOK | wxICON_INFORMATION);
            return;
        }
        int id = patent_list->GetItemData(idx);
        PatentEditDialog dlg(this, db.get(), id);
        if (dlg.ShowModal() == wxID_OK) {
            LoadPatents();
        }
    }

    void OnDeletePatent(wxCommandEvent& event) {
        // Get all selected items
        std::vector<long> selected;
        long idx = -1;
        while ((idx = patent_list->GetNextItem(idx, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) >= 0) {
            selected.push_back(idx);
        }

        if (selected.empty()) {
            wxMessageBox("Please select patent(s) to delete", "Info", wxOK | wxICON_INFORMATION);
            return;
        }

        if (selected.size() == 1) {
            // Single delete
            long i = selected[0];
            wxString code = patent_list->GetItemText(i, 0);
            if (wxMessageBox(wxString::Format("Delete patent %s?", code), "Confirm", wxYES_NO | wxICON_QUESTION) == wxYES) {
                int id = patent_list->GetItemData(i);
                db->DeletePatent(id);
                LoadPatents();
            }
        } else {
            // Multi delete - use batch for undo
            if (wxMessageBox(wxString::Format("Delete %zu patents?\n\n(Can be undone)", selected.size()),
                            "Confirm Delete", wxYES_NO | wxICON_QUESTION) == wxYES) {
                db->BeginBatch();
                for (long i : selected) {
                    int id = patent_list->GetItemData(i);
                    db->DeletePatent(id);
                }
                LoadPatents();
                wxMessageBox(wxString::Format("Deleted %zu patents\nUse Undo (Ctrl+Z) to restore", selected.size()), "Deleted", wxOK | wxICON_INFORMATION);
            }
        }
    }

    // ============== Common Tab Actions ==============
    int GetCurrentTab() { return notebook->GetSelection(); }

    wxListCtrl* GetCurrentList() {
        std::vector<wxListCtrl*> lists = {patent_list, oa_list, pct_list, sw_list,
            ic_list, foreign_list, fee_list, rule_list};
        int tab = GetCurrentTab();
        if (tab >= 0 && tab < (int)lists.size()) return lists[tab];
        return nullptr;
    }

    void OnNewByCurrentTab(wxCommandEvent&) {
        int tab = GetCurrentTab();
        switch (tab) {
            case 0: { wxCommandEvent evt; OnNewPatent(evt); break; }
            default:
                wxMessageBox(LANG_STR("New entry for this tab - coming soon", UTF8_STR("此页面新建功能即将推出")), "Info", wxOK);
        }
    }

    void OnEditByCurrentTab(wxCommandEvent&) {
        int tab = GetCurrentTab();
        wxListCtrl* list = GetCurrentList();
        if (!list) return;
        long idx = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (idx < 0) {
            wxMessageBox(LANG_STR("Please select an item", UTF8_STR("请先选择一条记录")), "Info", wxOK);
            return;
        }
        switch (tab) {
            case 0: { wxCommandEvent evt; OnEditPatent(evt); break; }  // 国内专利
            case 1: {  // OA处理
                int oa_id = oa_list->GetItemData(idx);
                OAEditDialog dlg(this, db.get(), oa_id);
                if (dlg.ShowModal() == wxID_OK) LoadOA();
                break;
            }
            case 2: wxMessageBox("Edit PCT - coming soon", "Info", wxOK); break;
            case 3: wxMessageBox("Edit Software - coming soon", "Info", wxOK); break;
            case 4: wxMessageBox("Edit IC - coming soon", "Info", wxOK); break;
            case 5: wxMessageBox("Edit Foreign - coming soon", "Info", wxOK); break;
            default: wxMessageBox("Edit - coming soon", "Info", wxOK);
        }
    }

    void OnDeleteByCurrentTab(wxCommandEvent&) {
        int tab = GetCurrentTab();
        wxListCtrl* list = GetCurrentList();
        if (!list) return;

        std::vector<long> selected;
        long idx = -1;
        while ((idx = list->GetNextItem(idx, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) >= 0)
            selected.push_back(idx);
        if (selected.empty()) {
            wxMessageBox(LANG_STR("Please select item(s) to delete", UTF8_STR("请先选择要删除的记录")), "Info", wxOK);
            return;
        }

        wxString confirm_msg = wxString::Format("Delete %zu item(s)?", selected.size());
        if (wxMessageBox(confirm_msg, LANG_STR("Confirm", UTF8_STR("确认")), wxYES_NO) != wxYES) return;

        for (long i : selected) {
            int id = list->GetItemData(i);
            switch (tab) {
                case 0: db->DeletePatent(id); break;
                case 1: db->DeleteOA(id); break;
                case 2: db->DeletePCT(id); break;
                case 3: db->DeleteSoftware(id); break;
                case 4: db->DeleteIC(id); break;
                case 5: db->DeleteForeign(id); break;
            }
        }
        // Reload current tab
        switch (tab) {
            case 0: LoadPatents(); break;
            case 1: LoadOA(); break;
            case 2: LoadPCT(); break;
            case 3: LoadSoftware(); break;
            case 4: LoadIC(); break;
            case 5: LoadForeign(); break;
        }
    }

    void OnStatistics(wxCommandEvent&) {
        auto patents = db->GetPatents();
        auto oas = db->GetOARecords();

        // 状态分布
        std::map<std::string, int> status_map;
        for (const auto& p : patents) {
            std::string s = p.application_status;
            if (s.empty()) s = "unknown";
            status_map[s]++;
        }

        // 类型分布
        std::map<std::string, int> type_map;
        for (const auto& p : patents) {
            std::string t = p.patent_type;
            if (t.empty()) t = "unknown";
            type_map[t]++;
        }

        // 等级分布
        std::map<std::string, int> level_map;
        for (const auto& p : patents) {
            std::string l = p.patent_level;
            if (l.empty()) l = "unknown";
            level_map[l]++;
        }

        int pending_oa = 0, completed_oa = 0;
        for (const auto& oa : oas) {
            if (oa.is_completed) completed_oa++;
            else pending_oa++;
        }

        // Build report
        wxString msg;
        msg += LANG_STR("=== Patent Statistics ===\n\n", UTF8_STR("=== 专利统计 ===\n\n"));
        msg += wxString::Format(LANG_STR("Total Patents: %zu\n", UTF8_STR("国内专利总数: %zu\n")), patents.size());
        msg += "\n";

        msg += LANG_STR("--- By Status ---\n", UTF8_STR("--- 按状态 ---\n"));
        for (const auto& [k, v] : status_map) {
            msg += wxString::Format("  %s: %d\n", DB_STR(k), v);
        }
        msg += "\n";

        msg += LANG_STR("--- By Type ---\n", UTF8_STR("--- 按类型 ---\n"));
        for (const auto& [k, v] : type_map) {
            msg += wxString::Format("  %s: %d\n", DB_STR(k), v);
        }
        msg += "\n";

        msg += LANG_STR("--- By Level ---\n", UTF8_STR("--- 按等级 ---\n"));
        for (const auto& [k, v] : level_map) {
            msg += wxString::Format("  %s: %d\n", DB_STR(k), v);
        }
        msg += "\n";

        msg += LANG_STR("--- OA Records ---\n", UTF8_STR("--- OA记录 ---\n"));
        msg += wxString::Format(LANG_STR("  Pending: %d\n  Completed: %d\n", UTF8_STR("  待处理: %d\n  已完成: %d\n")),
            pending_oa, completed_oa);

        wxMessageBox(msg, LANG_STR("Statistics", UTF8_STR("统计")), wxOK | wxICON_INFORMATION);
    }

    void OnBatchByCurrentTab(wxCommandEvent&) {
        int tab = GetCurrentTab();
        wxListCtrl* list = GetCurrentList();
        if (!list) return;

        std::vector<long> selected;
        long idx = -1;
        while ((idx = list->GetNextItem(idx, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) >= 0)
            selected.push_back(idx);
        if (selected.empty()) {
            wxMessageBox(LANG_STR("Select items first", UTF8_STR("请先选择要操作的记录")), "Info", wxOK);
            return;
        }

        if (tab == 0) {
            // Batch set status
            wxArrayString statuses;
            statuses.Add(UTF8_STR("审查中"));
            statuses.Add(UTF8_STR("已授权"));
            statuses.Add(UTF8_STR("已驳回"));
            statuses.Add(UTF8_STR("已放弃"));
            int ch = wxGetSingleChoiceIndex(UTF8_STR("选择状态:"), UTF8_STR("批量设置状态"), statuses, this);
            if (ch >= 0) {
                for (long i : selected) {
                    Patent p = db->GetPatentById(list->GetItemData(i));
                    p.application_status = statuses[ch].ToUTF8().data();
                    db->UpdatePatent(list->GetItemData(i), p);
                }
                LoadPatents();
                wxMessageBox(wxString::Format(UTF8_STR("已更新 %zu 条记录"), selected.size()), "Done", wxOK);
            }
        } else {
            wxMessageBox(LANG_STR("Batch operation not supported for this tab yet", UTF8_STR("此页面暂不支持批量操作")), "Info", wxOK);
        }
    }

    void OnSearchByCurrentTab(wxCommandEvent&) {
        int tab = GetCurrentTab();
        wxString keyword = common_search->GetValue();
        switch (tab) {
            case 0: SearchPatents(); break;
            case 1: LoadOA(); break;
            case 2: LoadPCT(); break;
            case 3: LoadSoftware(); break;
            case 4: LoadIC(); break;
            case 5: LoadForeign(); break;
        }
    }

    void OnFilterByCurrentTab(wxCommandEvent&) {
        int tab = GetCurrentTab();

        // Update status filter with values from current tab's table
        wxString current_status = common_status_filter->GetValue();
        common_status_filter->Clear();
        common_status_filter->Append(LANG_STR("All", UTF8_STR("全部")));

        std::string status_table;
        std::string status_col;
        switch (tab) {
            case 0: status_table = "patents"; status_col = "application_status"; break;
            case 1: status_table = "oa_records"; status_col = "oa_type"; break;
            case 2: status_table = "pct_patents"; status_col = "application_status"; break;
            case 3: status_table = "software_copyrights"; status_col = "application_status"; break;
            case 4: status_table = "ic_layouts"; status_col = "application_status"; break;
            case 5: status_table = "foreign_patents"; status_col = "patent_status"; break;
            default: status_table = "patents"; status_col = "application_status"; break;
        }
        auto statuses = db->GetDistinctValues(status_table, status_col);
        for (const auto& s : statuses) {
            if (!s.empty()) common_status_filter->Append(DB_STR(s));
        }
        common_status_filter->SetValue(current_status);

        // Update handler filter with values from current tab's table
        wxString current_handler = common_handler_filter->GetValue();
        common_handler_filter->Clear();
        common_handler_filter->Append(LANG_STR("All", UTF8_STR("全部")));

        std::string table_name;
        std::string col_name;
        switch (tab) {
            case 0: table_name = "patents"; col_name = "geke_handler"; break;
            case 1: table_name = "oa_records"; col_name = "handler"; break;
            case 2: table_name = "pct_patents"; col_name = "handler"; break;
            case 3: table_name = "software_copyrights"; col_name = "handler"; break;
            case 4: table_name = "ic_layouts"; col_name = "handler"; break;
            case 5: table_name = "foreign_patents"; col_name = "handler"; break;
            default: table_name = "patents"; col_name = "geke_handler"; break;
        }
        auto handlers = db->GetDistinctValues(table_name, col_name);
        for (const auto& h : handlers) {
            if (!h.empty()) common_handler_filter->Append(DB_STR(h));
        }
        common_handler_filter->SetValue(current_handler);

        // Level filter only valid for patents tab
        wxString current_level = common_level_filter->GetValue();
        common_level_filter->Clear();
        common_level_filter->Append(LANG_STR("All", UTF8_STR("全部")));
        if (tab == 0) {
            common_level_filter->Append(LANG_STR("Core", UTF8_STR("核心")));
            common_level_filter->Append(LANG_STR("Important", UTF8_STR("重要")));
            common_level_filter->Append(LANG_STR("Normal", UTF8_STR("一般")));
            auto levels = db->GetDistinctValues("patents", "patent_level");
            for (const auto& l : levels) {
                if (!l.empty() && l != "core" && l != "important" && l != "normal" &&
                    l.find("核心") == std::string::npos && l.find("重要") == std::string::npos && l.find("一般") == std::string::npos) {
                    common_level_filter->Append(DB_STR(l));
                }
            }
        }
        common_level_filter->SetValue(current_level);
        lbl_level->Show(tab == 0);
        common_level_filter->Show(tab == 0);

        switch (tab) {
            case 0: LoadPatents(); break;
            case 1: LoadOA(); break;
            case 2: LoadPCT(); break;
            case 3: LoadSoftware(); break;
            case 4: LoadIC(); break;
            case 5: LoadForeign(); break;
        }
    }

    // ============== OA Tab ==============
    void SetupOATab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* tb = new wxBoxSizer(wxHORIZONTAL);
        tb->Add(new wxStaticText(panel, wxID_ANY, UTF8_STR("筛选:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        oa_filter = new wxComboBox(panel, wxID_ANY, UTF8_STR("全部未完成"), wxDefaultPosition, wxSize(150, -1));
        oa_filter->Append(UTF8_STR("全部未完成"));
        oa_filter->Append(UTF8_STR("5天内到期"));
        oa_filter->Append(UTF8_STR("30天内到期"));
        oa_filter->Append(UTF8_STR("全部"));
        oa_filter->Append(UTF8_STR("已完成"));
        oa_filter->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { LoadOA(); });
        tb->Add(oa_filter, 0, wxRIGHT, 15);

        tb->AddStretchSpacer();

        wxButton* complete_btn = new wxButton(panel, wxID_ANY, "Mark Complete");
        complete_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            long idx = oa_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (idx >= 0) {
                int id = oa_list->GetItemData(idx);
                db->MarkOACompleted(id);
                LoadOA();
                wxMessageBox("Marked as completed", "Success", wxOK | wxICON_INFORMATION);
            }
        });
        wxButton* extend_btn = new wxButton(panel, wxID_ANY, "Request Extension");
        extend_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            long idx = oa_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (idx >= 0) {
                OARecord oa = db->GetOAById(oa_list->GetItemData(idx));
                if (!oa.is_extendable) {
                    wxMessageBox("This OA is not extendable", "Warning", wxOK | wxICON_WARNING);
                    return;
                }
                if (wxMessageBox("Request 2-month extension?", "Extension", wxYES_NO | wxICON_QUESTION) == wxYES) {
                    wxString new_deadline = DB_STR(oa.official_deadline) + " (extended)";
                    wxMessageBox("Extension requested\nNew deadline will be calculated\n\nOriginal: " + DB_STR(oa.official_deadline), "Extension", wxOK | wxICON_INFORMATION);
                }
            }
        });
        wxButton* urgent_btn = new wxButton(panel, wxID_ANY, "Show Urgent");
        urgent_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            oa_filter->SetValue("Within 5 days");
            LoadOA();
            auto records = db->GetOARecords("Within 5 days");
            int urgent = 0;
            for (const auto& oa : records) {
                if (!oa.is_completed) urgent++;
            }
            if (urgent > 0) {
                wxMessageBox(wxString::Format("WARNING: %d OA records need urgent attention!\n\nPlease check OA Processing tab for details.\n\nRed = overdue\nYellow = due within 5 days\nOrange = due within 10 days", urgent), "Urgent Alert", wxOK | wxICON_WARNING);
            } else {
                wxMessageBox("No urgent OA records. Good job!", "Status", wxOK | wxICON_INFORMATION);
            }
        });
        tb->Add(complete_btn, 0, wxRIGHT, 5);
        tb->Add(extend_btn, 0, wxRIGHT, 5);
        tb->Add(urgent_btn, 0);

        sizer->Add(tb, 0, wxALL, 5);

        oa_list = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        oa_list->AppendColumn(UTF8_STR("编号"), wxLIST_FORMAT_LEFT, 90);
        oa_list->AppendColumn(UTF8_STR("发明名称"), wxLIST_FORMAT_LEFT, 220);
        oa_list->AppendColumn(UTF8_STR("OA类型"), wxLIST_FORMAT_LEFT, 80);
        oa_list->AppendColumn(UTF8_STR("发文日"), wxLIST_FORMAT_LEFT, 90);
        oa_list->AppendColumn(UTF8_STR("截止日"), wxLIST_FORMAT_LEFT, 100);
        oa_list->AppendColumn(UTF8_STR("剩余天数"), wxLIST_FORMAT_LEFT, 80);
        oa_list->AppendColumn(UTF8_STR("处理人"), wxLIST_FORMAT_LEFT, 80);
        oa_list->AppendColumn(UTF8_STR("撰写人"), wxLIST_FORMAT_LEFT, 80);
        oa_list->AppendColumn(UTF8_STR("进度"), wxLIST_FORMAT_LEFT, 100);
        oa_list->AppendColumn(UTF8_STR("等级"), wxLIST_FORMAT_LEFT, 80);

        oa_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
            long idx = oa_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (idx >= 0) {
                int oa_id = oa_list->GetItemData(idx);
                OAEditDialog dlg(this, db.get(), oa_id);
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

        std::string filter_type = oa_filter->GetValue().ToStdString();
        std::string handler_filter = common_handler_filter->GetValue().ToStdString();

        // Check for "All" or "全部" or empty
        if (handler_filter == "All" || handler_filter == "全部" || handler_filter.empty()) {
            handler_filter = "";
        }

        std::string writer_filter;
        auto records = db->GetOARecords(filter_type, handler_filter, writer_filter);

        // Get current date for deadline calculation
        wxDateTime now = wxDateTime::Now();
        int urgent_count = 0;
        
        // Preload patent level mapping
        std::map<std::string, std::string> level_map;
        auto patents = db->GetPatents();
        for (const auto& p : patents) {
            level_map[p.geke_code] = p.patent_level;
        }
        
        int row = 0;
        for (const auto& oa : records) {
            long idx = oa_list->InsertItem(row, DB_STR(oa.geke_code));
            oa_list->SetItem(idx, 1, DB_STR(oa.patent_title));
            oa_list->SetItem(idx, 2, DB_STR(oa.oa_type));
            oa_list->SetItem(idx, 3, DB_STR(oa.issue_date));
            oa_list->SetItem(idx, 4, DB_STR(oa.official_deadline));
            
            // Calculate days remaining
            wxString days_str = "-";
            int days_val = 9999;
            wxColour bg_color;
            
            if (oa.is_completed) {
                days_str = "Completed";
                days_val = 99999;
                bg_color = wxColour(200, 255, 200);  // Green
            } else if (!oa.official_deadline.empty()) {
                wxDateTime deadline;
                deadline.ParseFormat(oa.official_deadline.c_str(), "%Y-%m-%d");
                if (deadline.IsValid()) {
                    wxTimeSpan diff = deadline - now;
                    int days = diff.GetDays();
                    days_val = days;
                    
                    if (days < 0) {
                        days_str = wxString::Format("Overdue %d days", -days);
                        bg_color = wxColour(255, 200, 200);  // Red
                        urgent_count++;
                    } else if (days <= 5) {
                        days_str = wxString::Format("%d days", days);
                        bg_color = wxColour(255, 255, 150);  // Yellow - very urgent
                        urgent_count++;
                    } else if (days <= 10) {
                        days_str = wxString::Format("%d days", days);
                        bg_color = wxColour(255, 230, 200);  // Orange
                    } else if (days <= 30) {
                        days_str = wxString::Format("%d days", days);
                        bg_color = wxColour(255, 250, 220);  // Light yellow
                    } else {
                        days_str = wxString::Format("%d days", days);
                    }
                }
            }
            
            oa_list->SetItem(idx, 5, days_str);
            oa_list->SetItem(idx, 6, DB_STR(oa.handler));
            oa_list->SetItem(idx, 7, DB_STR(oa.writer));
            oa_list->SetItem(idx, 8, DB_STR(oa.progress));
            
            // Patent level
            std::string level = level_map.count(oa.geke_code) ? level_map[oa.geke_code] : "";
            oa_list->SetItem(idx, 9, DB_STR(level));
            
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
        
        if (urgent_count > 0) {
            status_bar->SetStatusText(wxString::Format("OA Records: %d | URGENT: %d need attention!", row, urgent_count));
        } else {
            status_bar->SetStatusText(wxString::Format("OA Records: %d | All good!", row));
        }
    }

    // ============== PCT Tab ==============
    void SetupPCTTab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* tb1 = new wxBoxSizer(wxHORIZONTAL);
        tb1->Add(new wxStaticText(panel, wxID_ANY, UTF8_STR("搜索:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        pct_list = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);

        sizer->Add(tb1, 0, wxALL, 5);

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
                int pct_id = pct_list->GetItemData(idx);
                PCTEditDialog dlg(this, db.get(), pct_id);
                dlg.ShowModal();
                LoadPCT();
            }
        });

        sizer->Add(pct_list, 1, wxEXPAND | wxALL, 5);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, LANG_STR("PCT Applications", "PCT申请"));
    }

    void LoadPCT() {
        if (!db || !db->IsOpen()) return;
        pct_list->DeleteAllItems();
        auto patents = db->GetPCTPatents();
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
    }

    // ============== Software Tab ==============
    void SetupSoftwareTab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* tb = new wxBoxSizer(wxHORIZONTAL);
        tb->AddStretchSpacer();
        sizer->Add(tb, 0, wxALL, 5);

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
                int sw_id = sw_list->GetItemData(idx);
                SoftwareEditDialog dlg(this, db.get(), sw_id);
                dlg.ShowModal();
                LoadSoftware();
            }
        });

        sizer->Add(sw_list, 1, wxEXPAND | wxALL, 5);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, LANG_STR("Software Copyright", "软件著作权"));
    }

    void LoadSoftware() {
        if (!db || !db->IsOpen()) return;
        sw_list->DeleteAllItems();
        auto copyrights = db->GetSoftwareCopyrights();
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
    }

    // ============== IC Tab ==============
    void SetupICTab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* tb = new wxBoxSizer(wxHORIZONTAL);
        tb->AddStretchSpacer();
        sizer->Add(tb, 0, wxALL, 5);

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
                int ic_id = ic_list->GetItemData(idx);
                ICEditDialog dlg(this, db.get(), ic_id);
                dlg.ShowModal();
                LoadIC();
            }
        });

        sizer->Add(ic_list, 1, wxEXPAND | wxALL, 5);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, LANG_STR("IC Layout", "集成电路布图"));
    }

    void LoadIC() {
        if (!db || !db->IsOpen()) return;
        ic_list->DeleteAllItems();
        auto layouts = db->GetICLayouts();
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
    }

    // ============== Foreign Tab ==============
    void SetupForeignTab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* tb = new wxBoxSizer(wxHORIZONTAL);
        tb->AddStretchSpacer();
        sizer->Add(tb, 0, wxALL, 5);

        foreign_list = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        foreign_list->AppendColumn(UTF8_STR("案号"), wxLIST_FORMAT_LEFT, 90);
        foreign_list->AppendColumn(UTF8_STR("PCT号"), wxLIST_FORMAT_LEFT, 140);
        foreign_list->AppendColumn(UTF8_STR("国家"), wxLIST_FORMAT_LEFT, 70);
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
                int foreign_id = foreign_list->GetItemData(idx);
                ForeignEditDialog dlg(this, db.get(), foreign_id);
                dlg.ShowModal();
                LoadForeign();
            }
        });

        sizer->Add(foreign_list, 1, wxEXPAND | wxALL, 5);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, LANG_STR("Foreign Patents", "国外专利"));
    }

    void LoadForeign() {
        if (!db || !db->IsOpen()) return;
        foreign_list->DeleteAllItems();
        auto patents = db->GetForeignPatents();
        int row = 0;
        for (const auto& f : patents) {
            long idx = foreign_list->InsertItem(row, DB_STR(f.case_no));
            foreign_list->SetItem(idx, 1, DB_STR(f.pct_no));
            foreign_list->SetItem(idx, 2, DB_STR(f.country));
            foreign_list->SetItem(idx, 3, DB_STR(f.title));
            foreign_list->SetItem(idx, 4, DB_STR(f.owner));
            foreign_list->SetItem(idx, 5, DB_STR(f.patent_status));
            foreign_list->SetItem(idx, 6, DB_STR(f.handler));
            foreign_list->SetItemData(idx, f.id);
            row++;
        }
    }

    // ============== Annual Fee Tab ==============
    void SetupAnnualFeeTab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* tb = new wxBoxSizer(wxHORIZONTAL);
        tb->Add(new wxStaticText(panel, wxID_ANY, "Annual Fee Management"), 0, wxALIGN_CENTER_VERTICAL);
        tb->AddStretchSpacer();
        sizer->Add(tb, 0, wxALL, 5);

        fee_list = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        fee_list->AppendColumn(UTF8_STR("编号"), wxLIST_FORMAT_LEFT, 90);
        fee_list->AppendColumn(UTF8_STR("名称"), wxLIST_FORMAT_LEFT, 250);
        fee_list->AppendColumn(UTF8_STR("年度"), wxLIST_FORMAT_LEFT, 60);
        fee_list->AppendColumn(UTF8_STR("截止日"), wxLIST_FORMAT_LEFT, 100);
        fee_list->AppendColumn(UTF8_STR("金额"), wxLIST_FORMAT_LEFT, 80);
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

    void LoadAnnualFees() {
        if (!db || !db->IsOpen()) return;
        fee_list->DeleteAllItems();
        // Sample data for now
        auto patents = db->GetPatents();
        int row = 0;
        for (const auto& p : patents) {
            if (!p.expiration_date.empty() && p.application_status == "granted") {
                long idx = fee_list->InsertItem(row, DB_STR(p.geke_code));
                fee_list->SetItem(idx, 1, DB_STR(p.title));
                fee_list->SetItem(idx, 2, wxString::Format("%d", 2026 - atoi(p.application_date.substr(0, 4).c_str())));
                fee_list->SetItem(idx, 3, DB_STR(p.expiration_date));
                fee_list->SetItem(idx, 4, "900");
                fee_list->SetItem(idx, 5, "pending");
                fee_list->SetItem(idx, 6, DB_STR(p.geke_handler));
                row++;
                if (row >= 50) break;
            }
        }
    }

    // ============== Deadline Rules Tab ==============
    void SetupDeadlineRulesTab() {
        wxPanel* panel = new wxPanel(notebook);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* tb = new wxBoxSizer(wxHORIZONTAL);
        tb->Add(new wxStaticText(panel, wxID_ANY, "Deadline Calculation Rules"), 0, wxALIGN_CENTER_VERTICAL);
        tb->AddStretchSpacer();

        wxButton* add_rule_btn = new wxButton(panel, wxID_ANY, "Add Rule");
        add_rule_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxMessageBox("Add Rule dialog - coming soon!", "Info", wxOK | wxICON_INFORMATION);
        });
        tb->Add(add_rule_btn, 0);

        sizer->Add(tb, 0, wxALL, 5);

        rule_list = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        rule_list->AppendColumn("Rule Name", wxLIST_FORMAT_LEFT, 200);
        rule_list->AppendColumn("Applies To", wxLIST_FORMAT_LEFT, 100);
        rule_list->AppendColumn("Deadline Type", wxLIST_FORMAT_LEFT, 120);
        rule_list->AppendColumn("Calculation", wxLIST_FORMAT_LEFT, 150);
        rule_list->AppendColumn("Days/Value", wxLIST_FORMAT_LEFT, 80);

        rule_list->Bind(wxEVT_LIST_COL_CLICK, [this](wxListEvent& e) {
            int col = e.GetColumn();
            auto& state = sort_state[rule_list];
            if (state.first == col) state.second = !state.second;
            else { state.first = col; state.second = true; }
            SortListCtrl(rule_list, col, state.second);
        });

        sizer->Add(rule_list, 1, wxEXPAND | wxALL, 5);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, LANG_STR("Deadline Rules", "期限规则"));
    }

    void LoadDeadlineRules() {
        rule_list->DeleteAllItems();
        // Sample rules
        struct Rule { wxString name, applies, type, calc, days; };
        Rule rules[] = {
            {"OA Response (Invention)", "Domestic", "OA Response", "From Issue Date", "4 months"},
            {"OA Response (Utility)", "Domestic", "OA Response", "From Issue Date", "2 months"},
            {"PCT National Phase", "PCT", "National Entry", "From Priority", "30 months"},
            {"Annuity Payment", "All", "Annual Fee", "From Application", "Before each year"},
        };
        int row = 0;
        for (const auto& r : rules) {
            long idx = rule_list->InsertItem(row, r.name);
            rule_list->SetItem(idx, 1, r.applies);
            rule_list->SetItem(idx, 2, r.type);
            rule_list->SetItem(idx, 3, r.calc);
            rule_list->SetItem(idx, 4, r.days);
            row++;
        }
    }

    // ============== Theme ==============
    void SetTheme(int theme) {
        current_theme = theme;
        wxColour bg, fg, alt_bg;
        switch (theme) {
            case 1: // Dark
                bg = wxColour(26, 26, 46);
                fg = wxColour(212, 212, 212);
                alt_bg = wxColour(40, 40, 60);  // alternating row
                break;
            case 2: // Eye protection
                bg = wxColour(199, 237, 204);
                fg = wxColour(51, 51, 51);
                alt_bg = wxColour(220, 240, 220);
                break;
            default: // Light
                bg = wxColour(255, 255, 255);
                fg = wxColour(51, 51, 51);
                alt_bg = wxColour(240, 240, 240);
        }
        SetBackgroundColour(bg);
        SetForegroundColour(fg);

        // Update all list controls
        std::vector<wxListCtrl*> lists = {patent_list, oa_list, pct_list, sw_list,
                                          ic_list, foreign_list, fee_list, rule_list};
        for (auto* list : lists) {
            if (list) {
                list->SetBackgroundColour(bg);
                list->SetForegroundColour(fg);
                list->Refresh();
            }
        }

        // Update notebook tabs
        if (notebook) {
            notebook->SetBackgroundColour(bg);
            notebook->Refresh();
        }

        Refresh();
    }
    
    // ============== Language ==============
    void SetLanguage(int lang) {
        current_lang = lang;

        // Update toolbar buttons
        {
            const char* en_labels[] = {"Undo", "Sync", "Export", "Import", "Refresh"};
            const char* zh_labels[] = {"撤销", "同步", "导出", "导入", "刷新"};
            for (size_t i = 0; i < toolbar_btns.size() && i < 5; i++) {
                toolbar_btns[i]->SetLabel(lang == 0 ? wxString(en_labels[i]) : UTF8_STR(zh_labels[i]));
                toolbar_btns[i]->Refresh();
            }
        }
        // Update common toolbar buttons
        if (btn_new) btn_new->SetLabel(LANG_STR_L(lang, "New", "新建"));
        if (btn_edit) btn_edit->SetLabel(LANG_STR_L(lang, "Edit", "编辑"));
        if (btn_delete) btn_delete->SetLabel(LANG_STR_L(lang, "Delete", "删除"));
        if (btn_batch) btn_batch->SetLabel(LANG_STR_L(lang, "Batch", "批量"));
        if (lbl_search) lbl_search->SetLabel(LANG_STR_L(lang, "Search:", "搜索:"));
        if (lbl_status) lbl_status->SetLabel(LANG_STR_L(lang, "Status:", "状态:"));
        if (lbl_handler) lbl_handler->SetLabel(LANG_STR_L(lang, "Handler:", "处理人:"));
        if (lbl_level) lbl_level->SetLabel(LANG_STR_L(lang, "Level:", "等级:"));

        // Update common_status_filter items
        if (common_status_filter) {
            wxString cur = common_status_filter->GetValue();
            common_status_filter->Clear();
            common_status_filter->Append(LANG_STR_L(lang, "All", "全部"));
            common_status_filter->Append(LANG_STR_L(lang, "Pending", "审查中"));
            common_status_filter->Append(LANG_STR_L(lang, "Granted", "已授权"));
            common_status_filter->Append(LANG_STR_L(lang, "Rejected", "已驳回"));
            common_status_filter->SetValue(cur);
        }
        // Update common_handler_filter items
        if (common_handler_filter) {
            wxString cur = common_handler_filter->GetValue();
            common_handler_filter->Clear();
            common_handler_filter->Append(LANG_STR_L(lang, "All", "全部"));
            // Add actual handlers from database
            auto handlers = db->GetDistinctValues("patents", "geke_handler");
            for (const auto& h : handlers) common_handler_filter->Append(DB_STR(h));
            if (!cur.IsEmpty()) common_handler_filter->SetValue(cur);
            else common_handler_filter->SetValue(LANG_STR_L(lang, "All", "全部"));
        }
        // Update common_level_filter items
        if (common_level_filter) {
            wxString cur = common_level_filter->GetValue();
            common_level_filter->Clear();
            common_level_filter->Append(LANG_STR_L(lang, "All", "全部"));
            common_level_filter->Append(LANG_STR_L(lang, "Core", "核心"));
            common_level_filter->Append(LANG_STR_L(lang, "Important", "重要"));
            common_level_filter->Append(LANG_STR_L(lang, "Normal", "一般"));
            common_level_filter->SetValue(cur);
        }

        Layout();

        // Update window title
        SetTitle(LANG_STR_L(lang, "patX - Patent Manager v1.0.0", "patX - 专利管理系统 v1.0.0"));
        
        // Update status bar
        status_bar->SetStatusText(lang == 0 ?
            wxString("patX v1.0.0 | Database: patents.db | Press F1 for help") :
            UTF8_STR("patX v1.0.0 | 数据库: patents.db | 按F1获取帮助"));
        
        // Tab names and column headers always stay in Chinese - no update needed

        // Update notebook tab names
        for (size_t i = 0; i < notebook->GetPageCount(); i++) {
            wxString newName;
            switch (i) {
                case 0: newName = LANG_STR_L(lang, "Domestic Patents", "国内专利"); break;
                case 1: newName = LANG_STR_L(lang, "OA Processing", "OA处理"); break;
                case 2: newName = LANG_STR_L(lang, "PCT Applications", "PCT申请"); break;
                case 3: newName = LANG_STR_L(lang, "Software Copyright", "软件著作权"); break;
                case 4: newName = LANG_STR_L(lang, "IC Layout", "集成电路布图"); break;
                case 5: newName = LANG_STR_L(lang, "Foreign Patents", "国外专利"); break;
                case 6: newName = LANG_STR_L(lang, "Annual Fees", "年费管理"); break;
                case 7: newName = LANG_STR_L(lang, "Deadline Rules", "期限规则"); break;
            }
            notebook->SetPageText(i, newName);
        }

        // Rebuild menu bar with new language
        wxMenuBar* oldMb = GetMenuBar();
        if (oldMb) {
            // Store current menu state if needed
        }
        
        wxMenuBar* newMb = new wxMenuBar();
        
        // File menu
        wxMenu* file_menu = new wxMenu;
        file_menu->Append(wxID_NEW, LANG_STR_L(lang, "&New Patent\tCtrl+N", "新建专利(&N)\tCtrl+N"));
        file_menu->Append(wxID_OPEN, LANG_STR_L(lang, "&Import...", "导入(&I)..."));
        file_menu->Append(wxID_SAVE, LANG_STR_L(lang, "&Export...", "导出(&E)..."));
        file_menu->AppendSeparator();
        file_menu->Append(ID_SWITCH_DB, LANG_STR_L(lang, "Switch Database...", "切换数据库..."));
        file_menu->Append(ID_AUTO_BACKUP, LANG_STR_L(lang, "Auto Backup Settings...", "自动备份设置..."));
        file_menu->AppendSeparator();
        file_menu->Append(wxID_EXIT, LANG_STR_L(lang, "E&xit\tAlt+F4", "退出(&X)\tAlt+F4"));
        newMb->Append(file_menu, LANG_STR_L(lang, "&File", "文件(&F)"));
        
        // Edit menu
        wxMenu* edit_menu = new wxMenu;
        edit_menu->Append(wxID_UNDO, LANG_STR_L(lang, "&Undo\tCtrl+Z", "撤销(&U)\tCtrl+Z"));
        edit_menu->Append(wxID_REDO, LANG_STR_L(lang, "&Redo\tCtrl+Y", "重做(&R)\tCtrl+Y"));
        edit_menu->AppendSeparator();
        edit_menu->Append(wxID_EDIT, LANG_STR_L(lang, "&Edit Selected\tEnter", "编辑(&E)\tEnter"));
        edit_menu->Append(wxID_DELETE, LANG_STR_L(lang, "&Delete\tDel", "删除(&D)\tDel"));
        newMb->Append(edit_menu, LANG_STR_L(lang, "&Edit", "编辑(&E)"));
        
        // View menu
        wxMenu* view_menu = new wxMenu;
        view_menu->Append(wxID_REFRESH, LANG_STR_L(lang, "&Refresh\tF5", "刷新(&R)\tF5"));
        view_menu->AppendSeparator();
        view_menu->Append(ID_THEME_LIGHT, LANG_STR_L(lang, "Light Theme", "浅色主题"));
        view_menu->Append(ID_THEME_DARK, LANG_STR_L(lang, "Dark Theme", "深色主题"));
        view_menu->Append(ID_THEME_EYE, LANG_STR_L(lang, "Eye Protection Theme", "护眼主题"));
        view_menu->AppendSeparator();
        view_menu->Append(ID_LANG_EN, "English");
        view_menu->Append(ID_LANG_ZH, UTF8_STR("中文"));
        newMb->Append(view_menu, LANG_STR_L(lang, "&View", "视图(&V)"));
        
        // Tools menu
        wxMenu* tools_menu = new wxMenu;
        tools_menu->Append(ID_STATISTICS, LANG_STR_L(lang, "&Statistics...", "统计(&S)..."));
        tools_menu->Append(ID_VALIDATE_DATA, LANG_STR_L(lang, "Validate Data", "数据验证"));
        tools_menu->AppendSeparator();
        tools_menu->Append(ID_SYNC, LANG_STR_L(lang, "&Sync with NAS", "NAS同步(&N)"));
        tools_menu->Append(ID_NAS_CONFIG, LANG_STR_L(lang, "NAS &Configuration...", "NAS配置(&C)..."));
        tools_menu->AppendSeparator();
        tools_menu->Append(ID_BACKUP, LANG_STR_L(lang, "&Backup Database", "备份数据库(&B)"));
        tools_menu->Append(ID_RESTORE, LANG_STR_L(lang, "&Restore Backup...", "恢复备份(&R)..."));
        newMb->Append(tools_menu, LANG_STR_L(lang, "&Tools", "工具(&T)"));
        
        // Help menu
        wxMenu* help_menu = new wxMenu;
        help_menu->Append(wxID_ABOUT, LANG_STR_L(lang, "&About", "关于(&A)"));
        newMb->Append(help_menu, LANG_STR_L(lang, "&Help", "帮助(&H)"));
        
        SetMenuBar(newMb);
        
        // Refresh the window
        Refresh();
        Update();
        
        // Show brief message
        wxMessageBox(lang == 0 ?
            wxString("Language changed to English") :
            UTF8_STR("语言已切换为中文"),
            LANG_STR_L(lang, "Language", "语言"), wxOK | wxICON_INFORMATION);
    }
    
    // ============== Menu Handlers ==============
    void OnImport(wxCommandEvent&) {
        wxString filter = current_lang == 0 ?
            wxString("Excel files (*.xlsx)|*.xlsx|CSV files (*.csv)|*.csv|PDF files (*.pdf)|*.pdf|All files (*.*)|*.*") :
            UTF8_STR("Excel文件 (*.xlsx)|*.xlsx|CSV文件 (*.csv)|*.csv|PDF文件 (*.pdf)|*.pdf|所有文件 (*.*)|*.*");
        wxFileDialog dlg(this, LANG_STR("Import Data", "导入数据"), "", "", filter,
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            std::string path = dlg.GetPath().utf8_str().data();
            wxString ext = dlg.GetPath().Lower().AfterLast('.');

            if (ext == "pdf") {
                ImportPDF(path);
            } else {
                ImportExcel(path);
            }
        }
    }

    void ImportPDF(const std::string& path) {
#ifdef POPPLER_CPP_AVAILABLE
        // Parse PDF using poppler-cpp
        auto& parser = GetPdfParser();
        auto info = parser.Parse(path);

        if (!info.valid) {
            wxString msg = current_lang == 0 ?
                wxString("PDF parse failed: ") + wxString::FromUTF8(info.error_message.c_str()) :
                UTF8_STR("PDF解析失败: ") + wxString::FromUTF8(info.error_message.c_str());
            wxMessageBox(msg, LANG_STR("Error", "错误"), wxOK | wxICON_ERROR);
            return;
        }

        // Check if it's an OA notification
        if (info.oa_type == OaType::OFFICE_ACTION || info.oa_type == OaType::RETIFICATION) {
            // Find matching patent by application_no
            std::string app_no = info.application_no;
            // Remove CN prefix if present
            if (app_no.find("CN") == 0) {
                app_no = app_no.substr(2);
            }
            // Remove dots
            app_no.erase(std::remove(app_no.begin(), app_no.end(), '.'), app_no.end());

            // Search for patent
            auto patents = db->SearchPatents(app_no);
            std::string geke_code;
            for (const auto& p : patents) {
                std::string p_app_no = p.application_number;
                p_app_no.erase(std::remove(p_app_no.begin(), p_app_no.end(), '.'), p_app_no.end());
                if (p_app_no.find(app_no) != std::string::npos || app_no.find(p_app_no) != std::string::npos) {
                    geke_code = p.geke_code;
                    break;
                }
            }

            OARecord oa;
            oa.geke_code = geke_code;
            oa.patent_title = info.patent_title;
            oa.issue_date = info.issue_date;
            oa.official_deadline = info.official_deadline;

            // Determine OA type string
            if (info.oa_number > 0) {
                oa.oa_type = std::to_string(info.oa_number) + "-OA";
            } else {
                // Count existing OA records for this geke_code
                int existing_count = (int)db->GetOAByPatent(geke_code).size();
                oa.oa_type = std::to_string(existing_count + 1) + "-OA";
            }

            // Show confirmation dialog
            wxString confirm_msg;
            if (geke_code.empty()) {
                confirm_msg = current_lang == 0 ?
                    wxString::Format("OA notification detected but no matching patent found.\n\nApplication No: %s\nOA Type: %s\nIssue Date: %s\n\nCreate OA record without patent link?",
                        wxString::FromUTF8(info.application_no.c_str()),
                        wxString::FromUTF8(oa.oa_type.c_str()),
                        wxString::FromUTF8(info.issue_date.c_str())) :
                    wxString::Format(UTF8_STR("检测到OA通知书但未找到匹配专利。\n\n申请号: %s\nOA类型: %s\n发文日: %s\n\n是否创建无关联专利的OA记录?"),
                        wxString::FromUTF8(info.application_no.c_str()),
                        wxString::FromUTF8(oa.oa_type.c_str()),
                        wxString::FromUTF8(info.issue_date.c_str()));
            } else {
                confirm_msg = current_lang == 0 ?
                    wxString::Format("OA notification detected:\n\nGeke Code: %s\nApplication No: %s\nOA Type: %s\nIssue Date: %s\nDeadline: %s\n\nCreate OA record?",
                        wxString::FromUTF8(geke_code.c_str()),
                        wxString::FromUTF8(info.application_no.c_str()),
                        wxString::FromUTF8(oa.oa_type.c_str()),
                        wxString::FromUTF8(info.issue_date.c_str()),
                        wxString::FromUTF8(info.official_deadline.c_str())) :
                    wxString::Format(UTF8_STR("检测到OA通知书:\n\n格科编码: %s\n申请号: %s\nOA类型: %s\n发文日: %s\n绝限日: %s\n\n是否创建OA记录?"),
                        wxString::FromUTF8(geke_code.c_str()),
                        wxString::FromUTF8(info.application_no.c_str()),
                        wxString::FromUTF8(oa.oa_type.c_str()),
                        wxString::FromUTF8(info.issue_date.c_str()),
                        wxString::FromUTF8(info.official_deadline.c_str()));
            }

            int answer = wxMessageBox(confirm_msg, LANG_STR("Import PDF", "导入PDF"), wxYES_NO | wxICON_QUESTION);
            if (answer == wxYES) {
                db->InsertOA(oa);
                LoadOA();
                // Switch to OA tab
                notebook->SetSelection(1);
                wxMessageBox(current_lang == 0 ? "OA record created." : UTF8_STR("OA记录已创建。"),
                            LANG_STR("Success", "成功"), wxOK | wxICON_INFORMATION);
            }
        } else {
            wxString msg = current_lang == 0 ?
                wxString::Format("PDF parsed but not recognized as OA notification.\n\nApplication No: %s\nType: Unknown", wxString::FromUTF8(info.application_no.c_str())) :
                wxString::Format(UTF8_STR("PDF已解析但未识别为OA通知书。\n\n申请号: %s\n类型: 未知"), wxString::FromUTF8(info.application_no.c_str()));
            wxMessageBox(msg, LANG_STR("Info", "信息"), wxOK | wxICON_INFORMATION);
        }
#else
        // Fallback: try to use pdftotext command-line tool
        wxString temp_txt = wxFileName::CreateTempFileName("pdf_") + ".txt";

        // Try pdftotext from common locations
        wxString pdftotext_cmd;
        wxString pdftotext_paths[] = {
            "pdftotext",
            "C:\\Program Files\\poppler\\pdftotext.exe",
            "C:\\Program Files (x86)\\poppler\\pdftotext.exe",
            "C:\\poppler\\pdftotext.exe"
        };

        bool found = false;
        for (const auto& p : pdftotext_paths) {
            wxArrayString output;
            if (wxExecute(p + " -v", output, wxEXEC_NODISABLE) != -1 || wxFileExists(p)) {
                pdftotext_cmd = p;
                found = true;
                break;
            }
        }

        if (!found) {
            wxMessageBox(current_lang == 0 ?
                "PDF import requires poppler-cpp library or pdftotext tool.\n\nPlease install poppler-utils or build with poppler-cpp support." :
                UTF8_STR("PDF导入需要 poppler-cpp 库或 pdftotext 工具。\n\n请安装 poppler-utils 或使用支持 poppler-cpp 的版本构建。"),
                LANG_STR("Error", "错误"), wxOK | wxICON_ERROR);
            return;
        }

        // Extract text from PDF
        wxString cmd = wxString::Format("\"%s\" \"%s\" \"%s\"", pdftotext_cmd, wxString::FromUTF8(path.c_str()), temp_txt);
        long result = wxExecute(cmd, wxEXEC_SYNC);

        if (result != 0 || !wxFileExists(temp_txt)) {
            wxMessageBox(current_lang == 0 ? "Failed to extract text from PDF." : UTF8_STR("无法从PDF提取文本。"),
                        LANG_STR("Error", "错误"), wxOK | wxICON_ERROR);
            return;
        }

        // Read extracted text
        wxString text;
        wxTextFile f(temp_txt);
        if (f.Open()) {
            text = f.GetFirstLine();
            while (!f.Eof()) {
                text += "\n" + f.GetNextLine();
            }
            f.Close();
        }
        wxRemoveFile(temp_txt);

        // Simple text parsing for OA notification
        // Extract application number
        std::string app_no;
        wxRegEx app_re("申请号[：:]\s*(\d{12,13}[A-Z]?)");
        if (app_re.Matches(text)) {
            app_no = app_re.GetMatch(text, 1).ToUTF8().data();
        }

        // Extract issue date
        std::string issue_date;
        wxRegEx date_re("发文日[期]?[：:]\s*(\d{4})\s*年\s*(\d{1,2})\s*月\s*(\d{1,2})\s*日");
        if (date_re.Matches(text)) {
            long year, month, day;
            date_re.GetMatch(text, 1).ToLong(&year);
            date_re.GetMatch(text, 2).ToLong(&month);
            date_re.GetMatch(text, 3).ToLong(&day);
            char buf[16];
            snprintf(buf, sizeof(buf), "%04ld-%02ld-%02ld", year, month, day);
            issue_date = buf;
        }

        // Detect OA type
        bool is_oa = text.Find("审查意见通知书") != wxNOT_FOUND;
        bool is_rect = text.Find("补正通知书") != wxNOT_FOUND;

        if (!is_oa && !is_rect) {
            wxMessageBox(current_lang == 0 ? "PDF not recognized as OA notification." : UTF8_STR("PDF未识别为OA通知书。"),
                        LANG_STR("Info", "信息"), wxOK | wxICON_INFORMATION);
            return;
        }

        // Find matching patent
        std::string geke_code;
        if (!app_no.empty()) {
            auto patents = db->SearchPatents(app_no);
            for (const auto& p : patents) {
                std::string p_app_no = p.application_number;
                p_app_no.erase(std::remove(p_app_no.begin(), p_app_no.end(), '.'), p_app_no.end());
                if (p_app_no.find(app_no) != std::string::npos || app_no.find(p_app_no) != std::string::npos) {
                    geke_code = p.geke_code;
                    break;
                }
            }
        }

        // Create OA record
        OARecord oa;
        oa.geke_code = geke_code;
        oa.issue_date = issue_date;
        oa.oa_type = "1-OA"; // Default, user can edit

        // Show confirmation dialog
        wxString confirm_msg = current_lang == 0 ?
            wxString::Format("OA notification detected.\n\nApplication No: %s\nIssue Date: %s\nGeke Code: %s\n\nCreate OA record?",
                wxString::FromUTF8(app_no.c_str()),
                wxString::FromUTF8(issue_date.c_str()),
                geke_code.empty() ? "(not found)" : wxString::FromUTF8(geke_code.c_str())) :
            wxString::Format(UTF8_STR("检测到OA通知书。\n\n申请号: %s\n发文日: %s\n格科编码: %s\n\n是否创建OA记录?"),
                wxString::FromUTF8(app_no.c_str()),
                wxString::FromUTF8(issue_date.c_str()),
                geke_code.empty() ? UTF8_STR("(未找到)") : wxString::FromUTF8(geke_code.c_str()));

        if (wxMessageBox(confirm_msg, LANG_STR("Import PDF", "导入PDF"), wxYES_NO | wxICON_QUESTION) == wxYES) {
            db->InsertOA(oa);
            LoadOA();
            notebook->SetSelection(1);
            wxMessageBox(current_lang == 0 ? "OA record created." : UTF8_STR("OA记录已创建。"),
                        LANG_STR("Success", "成功"), wxOK | wxICON_INFORMATION);
        }
#endif
    }

    void ImportExcel(const std::string& path) {
        wxString title = LANG_STR("Importing...", "导入中...");
        wxProgressDialog progress(title, current_lang == 0 ?
            wxString("Reading file...") : UTF8_STR("正在读取文件..."), 100, this);
        progress.Pulse();

        auto& excel = GetExcelIO();
        auto result = excel.ImportPatents(path, *db, [&progress](int current, int total) -> bool {
            progress.Update(std::min(current * 100 / std::max(total, 1), 99));
            return !progress.WasCancelled();
        });

        progress.Update(100);
        progress.Close();

        if (result.added + result.updated + result.skipped == 0 && !excel.GetLastError().empty()) {
            wxString msg = current_lang == 0 ?
                wxString("Import failed: ") + excel.GetLastError() :
                UTF8_STR("导入失败: ") + excel.GetLastError();
            wxMessageBox(msg, LANG_STR("Error", "错误"), wxOK | wxICON_ERROR);
            return;
        }

        LoadAllData();
        wxString msg;
        if (current_lang == 0) {
            msg = wxString::Format("Import complete:\n  Added: %d\n  Updated: %d\n  Skipped: %d\n  Errors: %d\n\nDetails: %s",
                result.added, result.updated, result.skipped, result.errors,
                wxString::FromUTF8(result.type_summary.c_str()));
        } else {
            msg = wxString::Format(UTF8_STR("导入完成:\n  新增: %d\n  更新: %d\n  跳过: %d\n  错误: %d\n\n详情: %s"),
                result.added, result.updated, result.skipped, result.errors,
                wxString::FromUTF8(result.type_summary.c_str()));
        }
        wxMessageBox(msg, LANG_STR("Import", "导入"), wxOK | wxICON_INFORMATION);
    }

    void OnExport(wxCommandEvent&) {
        // Get current tab
        int current_tab = notebook->GetSelection();
        wxListCtrl* current_list = nullptr;

        switch (current_tab) {
            case 0: current_list = patent_list; break;
            case 1: current_list = oa_list; break;
            case 2: current_list = pct_list; break;
            case 3: current_list = sw_list; break;
            case 4: current_list = ic_list; break;
            case 5: current_list = foreign_list; break;
            case 6: current_list = fee_list; break;
            case 7: current_list = rule_list; break;
        }

        if (!current_list) {
            wxMessageBox(current_lang == 0 ?
                wxString("No data to export") :
                UTF8_STR("没有数据可导出"),
                current_lang == 0 ? wxString("Export") : UTF8_STR("导出"), wxOK | wxICON_WARNING);
            return;
        }

        // Get selected items
        std::vector<int> selected_ids;
        long item = -1;
        while ((item = current_list->GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1) {
            selected_ids.push_back(static_cast<int>(current_list->GetItemData(item)));
        }

        if (selected_ids.empty()) {
            wxMessageBox(current_lang == 0 ?
                wxString("No items selected. Please select items to export.") :
                UTF8_STR("未选中任何项目，请先选择要导出的项。"),
                current_lang == 0 ? wxString("Export") : UTF8_STR("导出"), wxOK | wxICON_WARNING);
            return;
        }

        wxFileDialog dlg(this,
            current_lang == 0 ? wxString("Export Selected") : UTF8_STR("导出选中项"),
            "", "export.csv",
            UTF8_STR("CSV文件 (*.csv)|*.csv|Excel文件 (*.xlsx)|*.xlsx|所有文件 (*.*)|*.*"),
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

        if (dlg.ShowModal() != wxID_OK) return;

        wxString path = dlg.GetPath();
        wxFile file(path, wxFile::write);
        if (!file.IsOpened()) {
            wxMessageBox(current_lang == 0 ?
                wxString("Cannot create file") :
                UTF8_STR("无法创建文件"),
                current_lang == 0 ? wxString("Error") : UTF8_STR("错误"), wxOK | wxICON_ERROR);
            return;
        }

        // Export based on tab type
        if (current_tab == 0) {
            // Patents
            file.Write(UTF8_STR("编号,申请号,发明名称,类型,等级,状态,处理人,发明人,申请日,到期日\n"));

            auto all_patents = db->GetPatents();
            for (int id : selected_ids) {
                for (const auto& p : all_patents) {
                    if (p.id == id) {
                        wxString line = wxString::Format("%s,%s,\"%s\",%s,%s,%s,%s,%s,%s,%s\n",
                            DB_STR(p.geke_code), DB_STR(p.application_number), DB_STR(p.title),
                            DB_STR(p.patent_type), DB_STR(p.patent_level), DB_STR(p.application_status),
                            DB_STR(p.geke_handler), DB_STR(p.inventor),
                            DB_STR(p.application_date), DB_STR(p.expiration_date));
                        file.Write(line);
                        break;
                    }
                }
            }
        } else if (current_tab == 1) {
            // OA
            file.Write(UTF8_STR("编号,专利名称,OA类型,发文日,截止日,处理人,进度\n"));

            auto all_oas = db->GetOARecords();
            for (int id : selected_ids) {
                for (const auto& oa : all_oas) {
                    if (oa.id == id) {
                        wxString line = wxString::Format("%s,\"%s\",%s,%s,%s,%s,%s\n",
                            DB_STR(oa.geke_code), DB_STR(oa.patent_title), DB_STR(oa.oa_type),
                            DB_STR(oa.issue_date), DB_STR(oa.official_deadline),
                            DB_STR(oa.handler), DB_STR(oa.progress));
                        file.Write(line);
                        break;
                    }
                }
            }
        } else if (current_tab == 2) {
            // PCT
            file.Write(UTF8_STR("编号,国内同源,PCT申请号,国家申请号,发明名称,状态,处理人\n"));

            auto all_pct = db->GetPCTPatents();
            for (int id : selected_ids) {
                for (const auto& p : all_pct) {
                    if (p.id == id) {
                        wxString line = wxString::Format("%s,%s,%s,%s,\"%s\",%s,%s\n",
                            DB_STR(p.geke_code), DB_STR(p.domestic_source), DB_STR(p.application_no),
                            DB_STR(p.country_app_no), DB_STR(p.title),
                            DB_STR(p.application_status), DB_STR(p.handler));
                        file.Write(line);
                        break;
                    }
                }
            }
        } else if (current_tab == 3) {
            // Software
            file.Write(UTF8_STR("案号,登记号,名称,权利人,状态,处理人\n"));

            auto all_sw = db->GetSoftwareCopyrights();
            for (int id : selected_ids) {
                for (const auto& s : all_sw) {
                    if (s.id == id) {
                        wxString line = wxString::Format("%s,%s,\"%s\",%s,%s,%s\n",
                            DB_STR(s.case_no), DB_STR(s.reg_no), DB_STR(s.title),
                            DB_STR(s.current_owner), DB_STR(s.application_status),
                            DB_STR(s.handler));
                        file.Write(line);
                        break;
                    }
                }
            }
        } else if (current_tab == 4) {
            // IC
            file.Write(UTF8_STR("案号,登记号,名称,权利人,状态,处理人\n"));

            auto all_ic = db->GetICLayouts();
            for (int id : selected_ids) {
                for (const auto& ic : all_ic) {
                    if (ic.id == id) {
                        wxString line = wxString::Format("%s,%s,\"%s\",%s,%s,%s\n",
                            DB_STR(ic.case_no), DB_STR(ic.reg_no), DB_STR(ic.title),
                            DB_STR(ic.current_owner), DB_STR(ic.application_status),
                            DB_STR(ic.handler));
                        file.Write(line);
                        break;
                    }
                }
            }
        } else if (current_tab == 5) {
            // Foreign
            file.Write(UTF8_STR("案号,PCT号,国家,名称,权利人,状态,处理人\n"));

            auto all_fp = db->GetForeignPatents();
            for (int id : selected_ids) {
                for (const auto& f : all_fp) {
                    if (f.id == id) {
                        wxString line = wxString::Format("%s,%s,%s,\"%s\",%s,%s,%s\n",
                            DB_STR(f.case_no), DB_STR(f.pct_no), DB_STR(f.country),
                            DB_STR(f.title), DB_STR(f.owner),
                            DB_STR(f.patent_status), DB_STR(f.handler));
                        file.Write(line);
                        break;
                    }
                }
            }
        }

        file.Close();
        wxMessageBox(current_lang == 0 ?
            wxString::Format("Exported %zu selected records", selected_ids.size()) :
            wxString::Format(UTF8_STR("已导出 %zu 条选中记录"), selected_ids.size()),
            current_lang == 0 ? wxString("Export") : UTF8_STR("导出"), wxOK | wxICON_INFORMATION);
    }

    // ===================== NAS Sync Functions =====================
    static bool CheckNASAccessible(const std::string& path) {
#ifdef _WIN32
        // Windows: check if UNC path is accessible
        std::wstring wpath(path.begin(), path.end());
        DWORD attrs = GetFileAttributesW(wpath.c_str());
        return (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
#else
        struct stat st;
        return (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
#endif
    }

    static bool CopyFileToNAS(const std::string& local_path, const std::string& nas_path) {
#ifdef _WIN32
        std::wstring wlocal(local_path.begin(), local_path.end());
        std::wstring wnas(nas_path.begin(), nas_path.end());
        return CopyFileW(wlocal.c_str(), wnas.c_str(), FALSE) != 0;
#else
        return std::filesystem::copy_file(local_path, nas_path,
            std::filesystem::copy_options::overwrite_existing);
#endif
    }

    void OnSync(wxCommandEvent&) {
        auto& config = NASConfig::Get();
        if (!config.enabled || config.nas_path.empty()) {
            wxMessageBox(current_lang == 0 ?
                wxString("NAS not configured. Please set up NAS first.") :
                UTF8_STR("NAS未配置，请先设置NAS。"),
                current_lang == 0 ? wxString("Sync") : UTF8_STR("同步"),
                wxOK | wxICON_WARNING);
            return;
        }

        // Check NAS accessibility
        if (!CheckNASAccessible(config.nas_path)) {
            wxMessageBox(current_lang == 0 ?
                wxString("Cannot access NAS path. Please check network connection.") :
                UTF8_STR("无法访问NAS路径，请检查网络连接。"),
                current_lang == 0 ? wxString("Error") : UTF8_STR("错误"),
                wxOK | wxICON_ERROR);
            return;
        }

        wxProgressDialog dlg(
            current_lang == 0 ? wxString("Syncing") : UTF8_STR("同步中"),
            current_lang == 0 ? wxString("Synchronizing with NAS...") : UTF8_STR("正在与NAS同步..."),
            100, this, wxPD_APP_MODAL | wxPD_AUTO_HIDE);

        std::string nas_db = config.nas_path + "/patents.db";
        std::string local_db = "patents.db";

        // Step 1: Backup local database
        dlg.Update(10, current_lang == 0 ? wxString("Backing up local database...") : UTF8_STR("备份本地数据库..."));
        std::string backup_db = "patents_local_backup.db";
        #ifdef _WIN32
        CopyFileA(local_db.c_str(), backup_db.c_str(), FALSE);
        #else
        std::filesystem::copy_file(local_db, backup_db, std::filesystem::copy_options::overwrite_existing);
        #endif

        // Step 2: Check if NAS database exists
        bool nas_has_data = false;
        #ifdef _WIN32
        DWORD attrs = GetFileAttributesA(nas_db.c_str());
        nas_has_data = (attrs != INVALID_FILE_ATTRIBUTES);
        #else
        struct stat st;
        nas_has_data = (stat(nas_db.c_str(), &st) == 0);
        #endif

        int added = 0, updated = 0, conflicts = 0;

        if (nas_has_data) {
            // Step 3: Download NAS database to temp
            dlg.Update(30, current_lang == 0 ? wxString("Downloading NAS database...") : UTF8_STR("下载NAS数据库..."));
            std::string temp_nas_db = "patents_nas_temp.db";
            #ifdef _WIN32
            CopyFileA(nas_db.c_str(), temp_nas_db.c_str(), FALSE);
            #else
            std::filesystem::copy_file(nas_db, temp_nas_db, std::filesystem::copy_options::overwrite_existing);
            #endif

            // Step 4: Open NAS database and merge
            dlg.Update(50, current_lang == 0 ? wxString("Merging data...") : UTF8_STR("合并数据..."));
            Database nas_db_obj(temp_nas_db);

            // Merge patents
            auto nas_patents = nas_db_obj.GetPatents();
            for (const auto& np : nas_patents) {
                Patent local = db->GetPatentByCode(np.geke_code);
                if (local.id == 0) {
                    // New record from NAS
                    db->InsertPatent(np);
                    added++;
                } else if (np.updated_at > local.updated_at) {
                    // NAS version is newer
                    db->UpdatePatent(local.id, np);
                    updated++;
                } else if (np.updated_at < local.updated_at && local.updated_at > np.updated_at) {
                    // Local is newer, conflict detected
                    conflicts++;
                }
            }

            // Clean up temp
            #ifdef _WIN32
            DeleteFileA(temp_nas_db.c_str());
            #else
            std::filesystem::remove(temp_nas_db);
            #endif
        }

        // Step 5: Upload local database to NAS
        dlg.Update(80, current_lang == 0 ? wxString("Uploading to NAS...") : UTF8_STR("上传到NAS..."));
        #ifdef _WIN32
        CopyFileA(local_db.c_str(), nas_db.c_str(), FALSE);
        #else
        std::filesystem::copy_file(local_db, nas_db, std::filesystem::copy_options::overwrite_existing);
        #endif

        dlg.Update(100);

        // Mark current user in database
        db->SetConfig("last_sync_user", config.username);
        db->SetConfig("last_sync_time", std::to_string(std::time(nullptr)));

        LoadAllData();

        wxString msg;
        if (current_lang == 0) {
            msg = wxString::Format("Sync completed!\nAdded: %d\nUpdated: %d\nConflicts: %d",
                added, updated, conflicts);
        } else {
            msg = wxString::Format(UTF8_STR("同步完成！\n新增: %d\n更新: %d\n冲突: %d"),
                added, updated, conflicts);
        }
        wxMessageBox(msg, current_lang == 0 ? wxString("Sync") : UTF8_STR("同步"),
            wxOK | wxICON_INFORMATION);
    }

    void OnNasConfig(wxCommandEvent&) {
        auto& config = NASConfig::Get();

        wxDialog dlg(this, wxID_ANY,
            current_lang == 0 ? wxString("NAS Configuration") : UTF8_STR("NAS配置"),
            wxDefaultPosition, wxSize(500, 300));
        wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
        wxPanel* panel = new wxPanel(&dlg);
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

        // NAS Path
        wxBoxSizer* row1 = new wxBoxSizer(wxHORIZONTAL);
        row1->Add(new wxStaticText(panel, wxID_ANY,
            current_lang == 0 ? wxString("NAS Path:") : UTF8_STR("NAS路径:")),
            0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        wxTextCtrl* path_field = new wxTextCtrl(panel, wxID_ANY,
            wxString::FromUTF8(config.nas_path.c_str()), wxDefaultPosition, wxSize(350, -1));
        row1->Add(path_field, 1);
        wxButton* browse_btn = new wxButton(panel, wxID_ANY,
            current_lang == 0 ? wxString("Browse...") : UTF8_STR("浏览..."));
        browse_btn->Bind(wxEVT_BUTTON, [&dlg, path_field](wxCommandEvent&) {
            wxDirDialog dir_dlg(&dlg,
                wxString::FromUTF8("选择NAS目录"),
                path_field->GetValue());
            if (dir_dlg.ShowModal() == wxID_OK) {
                path_field->SetValue(dir_dlg.GetPath());
            }
        });
        row1->Add(browse_btn, 0, wxLEFT, 5);
        sizer->Add(row1, 0, wxEXPAND | wxALL, 10);

        // Username
        wxBoxSizer* row2 = new wxBoxSizer(wxHORIZONTAL);
        row2->Add(new wxStaticText(panel, wxID_ANY,
            current_lang == 0 ? wxString("Username:") : UTF8_STR("用户名:")),
            0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        wxTextCtrl* user_field = new wxTextCtrl(panel, wxID_ANY,
            wxString::FromUTF8(config.username.c_str()));
        row2->Add(user_field, 1);
        sizer->Add(row2, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        // Auto sync interval
        wxBoxSizer* row3 = new wxBoxSizer(wxHORIZONTAL);
        row3->Add(new wxStaticText(panel, wxID_ANY,
            current_lang == 0 ? wxString("Auto Sync (minutes):") : UTF8_STR("自动同步(分钟):")),
            0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        wxSpinCtrl* sync_spin = new wxSpinCtrl(panel, wxID_ANY,
            wxString::Format("%d", config.auto_sync_minutes),
            wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 60, config.auto_sync_minutes);
        row3->Add(sync_spin, 0);
        row3->Add(new wxStaticText(panel, wxID_ANY,
            current_lang == 0 ? wxString("(0=disabled)") : UTF8_STR("(0=禁用)")),
            0, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);
        sizer->Add(row3, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        // Enable checkbox
        wxCheckBox* enable_cb = new wxCheckBox(panel, wxID_ANY,
            current_lang == 0 ? wxString("Enable NAS Sync") : UTF8_STR("启用NAS同步"));
        enable_cb->SetValue(config.enabled);
        sizer->Add(enable_cb, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        // Status
        wxString status;
        if (config.enabled && !config.nas_path.empty()) {
            bool accessible = CheckNASAccessible(config.nas_path);
            status = current_lang == 0 ?
                wxString::Format("Status: %s", accessible ? "Connected" : "Disconnected") :
                wxString::Format(UTF8_STR("状态: %s"), accessible ? UTF8_STR("已连接") : UTF8_STR("未连接"));
        } else {
            status = current_lang == 0 ? wxString("Status: Not configured") : UTF8_STR("状态: 未配置");
        }
        sizer->Add(new wxStaticText(panel, wxID_ANY, status), 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        panel->SetSizer(sizer);

        // Buttons
        wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton* test_btn = new wxButton(&dlg, wxID_ANY,
            current_lang == 0 ? wxString("Test Connection") : UTF8_STR("测试连接"));
        test_btn->Bind(wxEVT_BUTTON, [path_field](wxCommandEvent&) {
            std::string path = path_field->GetValue().ToStdString();
            bool ok = CheckNASAccessible(path);
            wxMessageBox(ok ?
                wxString::FromUTF8("连接成功！") :
                wxString::FromUTF8("连接失败，请检查路径和网络。"),
                wxString::FromUTF8("测试结果"), wxOK | (ok ? wxICON_INFORMATION : wxICON_ERROR));
        });
        btn_sizer->Add(test_btn, 0, wxRIGHT, 10);

        wxButton* ok_btn = new wxButton(&dlg, wxID_OK,
            current_lang == 0 ? wxString("Save") : UTF8_STR("保存"));
        wxButton* cancel_btn = new wxButton(&dlg, wxID_CANCEL,
            current_lang == 0 ? wxString("Cancel") : UTF8_STR("取消"));
        btn_sizer->AddStretchSpacer();
        btn_sizer->Add(ok_btn, 0, wxRIGHT, 10);
        btn_sizer->Add(cancel_btn, 0);
        main_sizer->Add(panel, 1, wxEXPAND);
        main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

        dlg.SetSizer(main_sizer);
        dlg.Centre();

        if (dlg.ShowModal() == wxID_OK) {
            config.nas_path = path_field->GetValue().ToStdString();
            config.username = user_field->GetValue().ToStdString();
            config.auto_sync_minutes = sync_spin->GetValue();
            config.enabled = enable_cb->GetValue();
            config.Save();

            wxMessageBox(current_lang == 0 ?
                wxString("NAS configuration saved.") :
                UTF8_STR("NAS配置已保存。"),
                current_lang == 0 ? wxString("Success") : UTF8_STR("成功"),
                wxOK | wxICON_INFORMATION);
        }
    }

    void OnBackup(wxCommandEvent&) {
        wxFileDialog dlg(this, "Backup Database", "", "patents_backup.db",
                         "Database files (*.db)|*.db", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() == wxID_OK) {
            wxCopyFile("patents.db", dlg.GetPath());
            wxMessageBox("Backup created successfully!", "Backup", wxOK | wxICON_INFORMATION);
        }
    }

    void OnSwitchDatabase(wxCommandEvent&) {
        wxFileDialog dlg(this, "Open Database", "", "",
                         "Database (*.db)|*.db|All files (*.*)|*.*",
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            if (wxMessageBox(wxString::Format("Switch to database:\n%s\n\nCurrent database will be closed.",
                            dlg.GetPath()), "Switch Database", wxYES_NO | wxICON_QUESTION) == wxYES) {
                db.reset();
                db = std::make_unique<Database>(dlg.GetPath().ToStdString());
                LoadAllData();
                status_bar->SetStatusText(wxString::Format("patX v1.0.0 | Database: %s",
                    wxFileName(dlg.GetPath()).GetFullName()));
                wxMessageBox("Database switched successfully!", "Success", wxOK | wxICON_INFORMATION);
            }
        }
    }

    void OnAutoBackup(wxCommandEvent&) {
        wxDialog dlg(this, wxID_ANY, "Auto Backup Settings", wxDefaultPosition, wxSize(400, 250));
        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        
        wxCheckBox* enable_cb = new wxCheckBox(&dlg, wxID_ANY, "Enable auto backup on exit");
        sizer->Add(enable_cb, 0, wxALL, 10);
        
        wxBoxSizer* row1 = new wxBoxSizer(wxHORIZONTAL);
        row1->Add(new wxStaticText(&dlg, wxID_ANY, "Backup interval:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
        wxComboBox* interval = new wxComboBox(&dlg, wxID_ANY, "Daily", wxDefaultPosition, wxSize(150, -1));
        interval->Append("Daily"); interval->Append("Weekly"); interval->Append("Monthly");
        row1->Add(interval, 1);
        sizer->Add(row1, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
        
        wxBoxSizer* row2 = new wxBoxSizer(wxHORIZONTAL);
        row2->Add(new wxStaticText(&dlg, wxID_ANY, "Max backups:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
        wxSpinCtrl* max_backups = new wxSpinCtrl(&dlg, wxID_ANY, "10", wxDefaultPosition, wxSize(80, -1));
        max_backups->SetRange(1, 100);
        row2->Add(max_backups, 1);
        sizer->Add(row2, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
        
        sizer->Add(new wxStaticText(&dlg, wxID_ANY, "Backup location: same folder as database"),
                   0, wxLEFT | wxRIGHT | wxTOP, 15);
        
        wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
        btn_sizer->Add(new wxButton(&dlg, wxID_OK, "Save"), 0, wxALL, 5);
        btn_sizer->Add(new wxButton(&dlg, wxID_CANCEL, "Cancel"), 0, wxALL, 5);
        sizer->Add(btn_sizer, 0, wxALIGN_CENTER | wxTOP, 15);
        
        dlg.SetSizer(sizer);
        dlg.ShowModal();
    }

    void OnRestore(wxCommandEvent&) {
        wxFileDialog dlg(this, "Restore Backup", "", "",
                         "Database files (*.db)|*.db", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            if (wxMessageBox("This will replace current database. Continue?",
                            "Warning", wxYES_NO | wxICON_WARNING) == wxYES) {
                wxCopyFile(dlg.GetPath(), "patents.db");
                LoadAllData();
                wxMessageBox("Database restored successfully!", "Restore", wxOK | wxICON_INFORMATION);
            }
        }
    }

    // Undo using database undo manager

    void OnUndo(wxCommandEvent&) {
        if (!db->CanUndo()) {
            wxMessageBox(LANG_STR("No operations to undo", "没有可撤销的操作"),
                         LANG_STR("Undo", "撤销"), wxOK | wxICON_INFORMATION);
            return;
        }

        int count = db->Undo();
        LoadAllData();
        wxString msg = current_lang == 0 ?
            wxString::Format("Undone %d operations", count) :
            wxString::Format(UTF8_STR("已撤销 %d 个操作"), count);
        wxMessageBox(msg, LANG_STR("Undo", "撤销"), wxOK | wxICON_INFORMATION);
    }

    void OnRefresh(wxCommandEvent&) {
        LoadAllData();
        wxMessageBox("Data refreshed!", "Refresh", wxOK | wxICON_INFORMATION);
    }

    void OnAbout(wxCommandEvent&) {
        wxMessageBox(
            "patX v1.0.0 - Patent Management System\n"
            "========================================\n\n"
            "Features:\n"
            "- 8 Tabs: Patents, OA, PCT, Software, IC, Foreign, AnnualFee, Rules\n"
            "- Full CRUD with edit dialogs\n"
            "- Batch operations: Set Level/Status\n"
            "- CSV Import/Export\n"
            "- OA deadline alerts (color-coded)\n"
            "- Theme switching (Light/Dark/Eye)\n"
            "- Search & Statistics\n"
            "- Backup/Restore\n\n"
            "Keyboard Shortcuts:\n"
            "Ctrl+N - New Patent\n"
            "Ctrl+E - Edit\n"
            "Ctrl+Z - Undo\n"
            "Ctrl+F - Search\n"
            "Del - Delete\n"
            "F5 - Refresh\n"
            "F1 - Help\n\n"
            "Tech: C++17 + wxWidgets + SQLite3\n"
            "Portable: 6 MB single exe\n\n"
            "GitHub: https://github.com/deepinwine/patX",
            "About patX", wxOK | wxICON_INFORMATION
        );
    }

    void OnExit(wxCommandEvent&) { Close(true); }

    void LoadAllData() {
        LoadPatents();
        LoadOA();
        LoadPCT();
        LoadSoftware();
        LoadIC();
        LoadForeign();
        LoadAnnualFees();
        LoadDeadlineRules();
    }
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
