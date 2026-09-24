// Edit dialogs for all IP modules, extracted from main_gui.cpp so the frame
// file stops growing. Every dialog persists its FULL record on Save -
// editing an existing record updates it; it never re-inserts.
#pragma once

#include <wx/wx.h>
#include <wx/spinctrl.h>
#include <map>
#include <string>

#include "database.hpp"

#define UTF8_STR(s) wxString::FromUTF8(s)
#define DB_STR(s) wxString::FromUTF8(s.c_str())

class PatentEditDialog : public wxDialog {
public:
    PatentEditDialog(wxWindow* parent, Database* db, int patent_id = 0);
private:
    Database* db_;
    int patent_id_;
    std::map<std::string, wxTextCtrl*> fields_;
    wxComboBox* type_combo_;
    wxComboBox* level_combo_;
    wxComboBox* status_combo_;

    void SetupUI();
    void AddField(wxWindow* parent, wxSizer* sizer, const std::string& key,
                  const wxString& label, int width = 150, bool multiline = false);
    void LoadData();
    void OnSave(wxCommandEvent&);
};

class OAEditDialog : public wxDialog {
public:
    // prefill_geke: 编号预填值（新建时从当前选中的案件带入）
    OAEditDialog(wxWindow* parent, Database* db, int oa_id = 0,
                 const std::string& prefill_geke = "");
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
    wxTextCtrl* summary_field_;
    wxTextCtrl* notes_field_;
    wxCheckBox* extendable_check_;
    wxCheckBox* completed_check_;
    // Read-only provenance block for web-synced records: user-owned fields
    // stay editable, external identity is displayed but not editable.
    wxStaticText* external_info_;

    void SetupUI();
    void LoadData();
    void OnSave(wxCommandEvent&);
};

class PCTEditDialog : public wxDialog {
public:
    PCTEditDialog(wxWindow* parent, Database* db, int pct_id = 0);
private:
    Database* db_;
    int pct_id_;
    std::map<std::string, wxTextCtrl*> fields_;
    wxComboBox* status_combo_;
    void SetupUI();
    void LoadData();
    void OnSave(wxCommandEvent&);
};

class SoftwareEditDialog : public wxDialog {
public:
    SoftwareEditDialog(wxWindow* parent, Database* db, int sw_id = 0);
private:
    Database* db_;
    int sw_id_;
    std::map<std::string, wxTextCtrl*> fields_;
    wxComboBox* status_combo_;
    void SetupUI();
    void LoadData();
    void OnSave(wxCommandEvent&);
};

class ICEditDialog : public wxDialog {
public:
    ICEditDialog(wxWindow* parent, Database* db, int ic_id = 0);
private:
    Database* db_;
    int ic_id_;
    std::map<std::string, wxTextCtrl*> fields_;
    wxComboBox* status_combo_;
    void SetupUI();
    void LoadData();
    void OnSave(wxCommandEvent&);
};

class ForeignEditDialog : public wxDialog {
public:
    ForeignEditDialog(wxWindow* parent, Database* db, int fp_id = 0);
private:
    Database* db_;
    int fp_id_;
    std::map<std::string, wxTextCtrl*> fields_;
    wxComboBox* status_combo_;
    void SetupUI();
    void LoadData();
    void OnSave(wxCommandEvent&);
};

class DeadlineRuleEditDialog : public wxDialog {
public:
    DeadlineRuleEditDialog(wxWindow* parent, Database* db, int rule_id = 0);
private:
    Database* db_;
    int rule_id_;
    wxTextCtrl* jurisdiction_field_;
    wxTextCtrl* event_type_field_;
    wxTextCtrl* description_field_;
    wxSpinCtrl* months_spin_;
    wxSpinCtrl* days_spin_;
    wxSpinCtrl* max_ext_spin_;
    wxCheckBox* extendable_check_;
    wxCheckBox* enabled_check_;
    wxTextCtrl* notes_field_;

    void LoadData();
    void OnSave(wxCommandEvent&);
};
