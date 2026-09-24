#include "ui/edit_dialogs.hpp"
#include "patx/log.hpp"

#include <wx/spinctrl.h>
#include <wx/textctrl.h>

// wxString -> UTF-8 std::string (wx 3.2/3.3 compatible)
static inline std::string ToStd(const wxString& s) { return std::string(s.utf8_str()); }

// ===========================================================================
// Patent
// ===========================================================================

PatentEditDialog::PatentEditDialog(wxWindow* parent, Database* db, int patent_id)
    : wxDialog(parent, wxID_ANY, patent_id ? "Edit Patent" : "New Patent",
               wxDefaultPosition, wxSize(640, 640)),
      db_(db), patent_id_(patent_id) {
    SetupUI();
    if (patent_id) LoadData();
}

void PatentEditDialog::SetupUI() {
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    wxScrolledWindow* scroll = new wxScrolledWindow(this, wxID_ANY);
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    sizer->Add(new wxStaticText(scroll, wxID_ANY, "=== Basic Info ==="), 0, wxTOP, 10);

    AddField(scroll, sizer, "geke_code", UTF8_STR("编号"));
    AddField(scroll, sizer, "application_number", "Application No");
    AddField(scroll, sizer, "title", "Title", 300);
    AddField(scroll, sizer, "proposal_name", "Proposal Name", 200);

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

    sizer->Add(new wxStaticText(scroll, wxID_ANY, "=== Dates ==="), 0, wxTOP, 10);
    wxBoxSizer* row2 = new wxBoxSizer(wxHORIZONTAL);
    AddField(scroll, row2, "application_date", "App Date", 120);
    AddField(scroll, row2, "authorization_date", "Auth Date", 120);
    AddField(scroll, row2, "expiration_date", "Exp Date", 120);
    sizer->Add(row2, 0, wxEXPAND | wxALL, 5);

    sizer->Add(new wxStaticText(scroll, wxID_ANY, "=== People ==="), 0, wxTOP, 10);
    wxBoxSizer* row3 = new wxBoxSizer(wxHORIZONTAL);
    AddField(scroll, row3, "geke_handler", UTF8_STR("处理人"), 100);
    AddField(scroll, row3, "inventor", "Inventor", 150);
    sizer->Add(row3, 0, wxEXPAND | wxALL, 5);

    wxBoxSizer* row4 = new wxBoxSizer(wxHORIZONTAL);
    AddField(scroll, row4, "rd_department", "R&D Dept", 100);
    AddField(scroll, row4, "agency_firm", "Agency", 150);
    sizer->Add(row4, 0, wxEXPAND | wxALL, 5);

    sizer->Add(new wxStaticText(scroll, wxID_ANY, "=== Applicants ==="), 0, wxTOP, 10);
    AddField(scroll, sizer, "original_applicant", "Original Applicant", 200);
    AddField(scroll, sizer, "current_applicant", "Current Applicant", 200);

    sizer->Add(new wxStaticText(scroll, wxID_ANY, "=== Classification ==="), 0, wxTOP, 10);
    wxBoxSizer* row5 = new wxBoxSizer(wxHORIZONTAL);
    AddField(scroll, row5, "class_level1", "Level 1", 80);
    AddField(scroll, row5, "class_level2", "Level 2", 80);
    AddField(scroll, row5, "class_level3", "Level 3", 80);
    AddField(scroll, row5, "class_level4", "Level 4", 80);
    sizer->Add(row5, 0, wxEXPAND | wxALL, 5);

    // Structured metadata previously buried in notes
    sizer->Add(new wxStaticText(scroll, wxID_ANY, "=== Structured Info ==="), 0, wxTOP, 10);
    wxBoxSizer* row6 = new wxBoxSizer(wxHORIZONTAL);
    AddField(scroll, row6, "technology_route", UTF8_STR("技术路线"), 130);
    AddField(scroll, row6, "rd_project", UTF8_STR("研发项目"), 130);
    AddField(scroll, row6, "internal_rd_project", UTF8_STR("内部研发项目"), 130);
    sizer->Add(row6, 0, wxEXPAND | wxALL, 5);

    wxBoxSizer* row7 = new wxBoxSizer(wxHORIZONTAL);
    AddField(scroll, row7, "agent_name", UTF8_STR("代理人"), 100);
    AddField(scroll, row7, "agent_code", UTF8_STR("代理人编码"), 100);
    AddField(scroll, row7, "disclosure_writer", UTF8_STR("交底书撰写人"), 110);
    sizer->Add(row7, 0, wxEXPAND | wxALL, 5);

    wxBoxSizer* row8 = new wxBoxSizer(wxHORIZONTAL);
    AddField(scroll, row8, "fee_status", UTF8_STR("缴费状态"), 90);
    AddField(scroll, row8, "project_id", "Project ID", 90);
    AddField(scroll, row8, "tags", UTF8_STR("标签"), 110);
    sizer->Add(row8, 0, wxEXPAND | wxALL, 5);

    AddField(scroll, sizer, "notes", "Notes", 400, true);

    scroll->SetSizer(sizer);
    scroll->SetScrollRate(5, 5);
    main_sizer->Add(scroll, 1, wxEXPAND | wxALL, 10);

    wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(new wxButton(this, wxID_OK, "Save"), 0, wxRIGHT, 10);
    btn_sizer->Add(new wxButton(this, wxID_CANCEL, "Cancel"), 0);
    main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

    SetSizer(main_sizer);
    Centre();
    Bind(wxEVT_BUTTON, &PatentEditDialog::OnSave, this, wxID_OK);
}

void PatentEditDialog::AddField(wxWindow* parent, wxSizer* sizer, const std::string& key,
                                const wxString& label, int width, bool multiline) {
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

void PatentEditDialog::LoadData() {
    Patent p = db_->GetPatentById(patent_id_);
    auto set = [&](const char* key, const std::string& value) {
        if (fields_.count(key)) fields_[key]->SetValue(DB_STR(value));
    };
    set("geke_code", p.geke_code);
    set("application_number", p.application_number);
    set("title", p.title);
    set("proposal_name", p.proposal_name);
    set("application_date", p.application_date);
    set("authorization_date", p.authorization_date);
    set("expiration_date", p.expiration_date);
    set("geke_handler", p.geke_handler);
    set("inventor", p.inventor);
    set("rd_department", p.rd_department);
    set("agency_firm", p.agency_firm);
    set("original_applicant", p.original_applicant);
    set("current_applicant", p.current_applicant);
    set("class_level1", p.class_level1);
    set("class_level2", p.class_level2);
    set("class_level3", p.class_level3);
    set("class_level4", p.class_level4);
    set("technology_route", p.technology_route);
    set("rd_project", p.rd_project);
    set("internal_rd_project", p.internal_rd_project);
    set("agent_name", p.agent_name);
    set("agent_code", p.agent_code);
    set("disclosure_writer", p.disclosure_writer);
    set("fee_status", p.fee_status);
    set("project_id", p.project_id);
    set("tags", p.tags);
    set("notes", p.notes);
    type_combo_->SetValue(DB_STR(p.patent_type));
    level_combo_->SetValue(DB_STR(p.patent_level));
    status_combo_->SetValue(DB_STR(p.application_status));
}

void PatentEditDialog::OnSave(wxCommandEvent&) {
    if (!db_) return;

    Patent p;
    p.id = patent_id_;
    auto get = [&](const char* key) -> std::string {
        return fields_.count(key) ? ToStd(fields_[key]->GetValue()) : "";
    };
    p.geke_code = get("geke_code");
    p.application_number = get("application_number");
    p.title = get("title");
    p.proposal_name = get("proposal_name");
    p.application_date = get("application_date");
    p.authorization_date = get("authorization_date");
    p.expiration_date = get("expiration_date");
    p.geke_handler = get("geke_handler");
    p.inventor = get("inventor");
    p.rd_department = get("rd_department");
    p.agency_firm = get("agency_firm");
    p.original_applicant = get("original_applicant");
    p.current_applicant = get("current_applicant");
    p.class_level1 = get("class_level1");
    p.class_level2 = get("class_level2");
    p.class_level3 = get("class_level3");
    p.class_level4 = get("class_level4");
    p.technology_route = get("technology_route");
    p.rd_project = get("rd_project");
    p.internal_rd_project = get("internal_rd_project");
    p.agent_name = get("agent_name");
    p.agent_code = get("agent_code");
    p.disclosure_writer = get("disclosure_writer");
    p.fee_status = get("fee_status");
    p.project_id = get("project_id");
    p.tags = get("tags");
    p.notes = get("notes");
    p.patent_type = ToStd(type_combo_->GetValue());
    p.patent_level = ToStd(level_combo_->GetValue());
    p.application_status = ToStd(status_combo_->GetValue());

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
        wxMessageBox(UTF8_STR("保存失败（编号重复？）") + "\n" + db_->LastError(),
                     UTF8_STR("错误"), wxOK | wxICON_ERROR);
    }
}

// ===========================================================================
// OA record
// ===========================================================================

OAEditDialog::OAEditDialog(wxWindow* parent, Database* db, int oa_id,
                           const std::string& prefill_geke)
    : wxDialog(parent, wxID_ANY, oa_id ? "Edit OA Record" : "New OA Record",
               wxDefaultPosition, wxSize(560, 560)),
      db_(db), oa_id_(oa_id) {
    SetupUI();
    if (oa_id) LoadData();
    else if (!prefill_geke.empty()) geke_code_field_->ChangeValue(prefill_geke);
}

void OAEditDialog::SetupUI() {
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    wxScrolledWindow* panel = new wxScrolledWindow(this, wxID_ANY);
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    auto add_row = [&](const wxString& label, wxTextCtrl** out, int w = 120,
                       bool multiline = false) {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(panel, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        *out = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition,
                              wxSize(w, multiline ? 60 : -1),
                              multiline ? wxTE_MULTILINE : 0);
        row->Add(*out, 1, multiline ? wxEXPAND : 0);
        sizer->Add(row, 0, wxEXPAND | wxALL, 4);
    };

    add_row(UTF8_STR("编号:"), &geke_code_field_, 150);
    {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(panel, wxID_ANY, "OA Type:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        oa_type_combo_ = new wxComboBox(panel, wxID_ANY, "1-OA");
        for (const char* t : {"1-OA", "2-OA", "3-OA", "4-OA", "Rejection", "Admission",
                              "Non-Final Office Action", "Final Office Action",
                              "Restriction Requirement", "Advisory Action"}) {
            oa_type_combo_->Append(t);
        }
        row->Add(oa_type_combo_, 1);
        sizer->Add(row, 0, wxEXPAND | wxALL, 4);
    }
    {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(panel, wxID_ANY, "Issue Date:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        issue_date_field_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(100, -1));
        row->Add(issue_date_field_, 0, wxRIGHT, 20);
        row->Add(new wxStaticText(panel, wxID_ANY, "Deadline:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        deadline_field_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(100, -1));
        row->Add(deadline_field_, 1);
        sizer->Add(row, 0, wxEXPAND | wxALL, 4);
    }
    {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(panel, wxID_ANY, "Handler:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        handler_field_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(80, -1));
        row->Add(handler_field_, 0, wxRIGHT, 20);
        row->Add(new wxStaticText(panel, wxID_ANY, "Writer:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        writer_field_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(80, -1));
        row->Add(writer_field_, 1);
        sizer->Add(row, 0, wxEXPAND | wxALL, 4);
    }
    {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(panel, wxID_ANY, "Progress:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        progress_combo_ = new wxComboBox(panel, wxID_ANY, "pending");
        for (const char* p : {"pending", "drafting", "submitted", "completed"}) {
            progress_combo_->Append(p);
        }
        row->Add(progress_combo_, 1);
        sizer->Add(row, 0, wxEXPAND | wxALL, 4);
    }
    add_row(UTF8_STR("审查意见摘要:"), &summary_field_, 200, true);
    add_row("Notes:", &notes_field_, 200, true);

    {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        extendable_check_ = new wxCheckBox(panel, wxID_ANY, "Extendable");
        completed_check_ = new wxCheckBox(panel, wxID_ANY, "Completed");
        row->Add(extendable_check_, 0, wxRIGHT, 20);
        row->Add(completed_check_, 0);
        sizer->Add(row, 0, wxALL, 4);
    }

    external_info_ = new wxStaticText(panel, wxID_ANY, "");
    external_info_->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_ITALIC, wxFONTWEIGHT_NORMAL));
    sizer->Add(external_info_, 0, wxALL, 4);

    panel->SetSizer(sizer);
    panel->SetScrollRate(5, 5);
    main_sizer->Add(panel, 1, wxEXPAND | wxALL, 10);

    wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(new wxButton(this, wxID_OK, "Save"), 0, wxRIGHT, 10);
    btn_sizer->Add(new wxButton(this, wxID_CANCEL, "Cancel"), 0);
    main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

    SetSizer(main_sizer);
    Centre();
    Bind(wxEVT_BUTTON, &OAEditDialog::OnSave, this, wxID_OK);
}

void OAEditDialog::LoadData() {
    OARecord oa = db_->GetOAById(oa_id_);
    geke_code_field_->SetValue(DB_STR(oa.geke_code));
    oa_type_combo_->SetValue(DB_STR(oa.oa_type));
    deadline_field_->SetValue(DB_STR(oa.official_deadline));
    issue_date_field_->SetValue(DB_STR(oa.issue_date));
    handler_field_->SetValue(DB_STR(oa.handler));
    writer_field_->SetValue(DB_STR(oa.writer));
    progress_combo_->SetValue(DB_STR(oa.progress));
    summary_field_->SetValue(DB_STR(oa.oa_summary));
    notes_field_->SetValue(DB_STR(oa.notes));
    extendable_check_->SetValue(oa.is_extendable);
    completed_check_->SetValue(oa.is_completed);

    if (oa.source == "cnipa") {
        external_info_->SetLabel(UTF8_STR("网页同步记录 — 远端文档: ") +
                                 DB_STR(oa.remote_document_id) +
                                 (oa.deadline_source == "calculated"
                                      ? UTF8_STR(" | 期限为计算值，可手动修改")
                                      : ""));
    }
}

void OAEditDialog::OnSave(wxCommandEvent&) {
    OARecord oa = oa_id_ ? db_->GetOAById(oa_id_) : OARecord();
    oa.geke_code = ToStd(geke_code_field_->GetValue());
    oa.oa_type = ToStd(oa_type_combo_->GetValue());
    oa.official_deadline = ToStd(deadline_field_->GetValue());
    oa.issue_date = ToStd(issue_date_field_->GetValue());
    oa.handler = ToStd(handler_field_->GetValue());
    oa.writer = ToStd(writer_field_->GetValue());
    oa.progress = ToStd(progress_combo_->GetValue());
    oa.oa_summary = ToStd(summary_field_->GetValue());
    oa.notes = ToStd(notes_field_->GetValue());
    oa.is_extendable = extendable_check_->GetValue();
    oa.is_completed = completed_check_->GetValue();
    if (oa.is_completed && oa.response_date.empty()) {
        oa.response_date = db_->GetCurrentDate();
    }
    // Manual deadline edit wins over future re-syncs
    if (oa_id_ && !oa.official_deadline.empty() &&
        oa.deadline_source != "manual" && oa.deadline_source != "official") {
        OARecord old = db_->GetOAById(oa_id_);
        if (old.official_deadline != oa.official_deadline) {
            oa.deadline_source = "manual";
        }
    }

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
        wxMessageBox("Failed to save OA record: " + db_->LastError(), "Error", wxOK | wxICON_ERROR);
    }
}

// ===========================================================================
// Shared plumbing for the simple one-record dialogs
// ===========================================================================

namespace {

using DialogRow = std::tuple<wxString, std::string, int>;

// Common dialog scaffolding: scrolled field grid + status combo + buttons.
void BuildSimpleDialogBase(wxDialog* dlg,
                           const std::vector<DialogRow>& rows,
                           const wxString& status_label,
                           std::map<std::string, wxTextCtrl*>& fields,
                           wxComboBox*& status_combo) {
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    wxScrolledWindow* panel = new wxScrolledWindow(dlg, wxID_ANY);
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    for (const auto& [label, key, width] : rows) {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(panel, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        wxTextCtrl* tc = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(width, -1));
        fields[key] = tc;
        row->Add(tc, 1);
        sizer->Add(row, 0, wxEXPAND | wxALL, 3);
    }

    wxBoxSizer* status_row = new wxBoxSizer(wxHORIZONTAL);
    status_row->Add(new wxStaticText(panel, wxID_ANY, status_label), 0,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    status_combo = new wxComboBox(panel, wxID_ANY, "pending");
    status_combo->Append("pending");
    status_combo->Append("granted");
    status_combo->Append("registered");
    status_combo->Append("rejected");
    status_combo->Append("abandoned");
    status_row->Add(status_combo, 1);
    sizer->Add(status_row, 0, wxEXPAND | wxALL, 3);

    panel->SetSizer(sizer);
    panel->SetScrollRate(5, 5);
    main_sizer->Add(panel, 1, wxEXPAND | wxALL, 10);

    wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(new wxButton(dlg, wxID_OK, "Save"), 0, wxRIGHT, 10);
    btn_sizer->Add(new wxButton(dlg, wxID_CANCEL, "Cancel"), 0);
    main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

    dlg->SetSizer(main_sizer);
    dlg->Centre();
}

std::string FieldValue(const std::map<std::string, wxTextCtrl*>& fields, const char* key) {
    auto it = fields.find(key);
    return it == fields.end() ? "" : ToStd(it->second->GetValue());
}

void SetFieldValue(std::map<std::string, wxTextCtrl*>& fields, const char* key,
                   const std::string& value) {
    auto it = fields.find(key);
    if (it != fields.end()) it->second->SetValue(DB_STR(value));
}

} // namespace

// ===========================================================================
// PCT
// ===========================================================================

PCTEditDialog::PCTEditDialog(wxWindow* parent, Database* db, int pct_id)
    : wxDialog(parent, wxID_ANY, pct_id ? "Edit PCT" : "New PCT",
               wxDefaultPosition, wxSize(560, 620)),
      db_(db), pct_id_(pct_id) {
    using T = std::tuple<wxString, std::string, int>;
    std::vector<DialogRow> rows = {
        {UTF8_STR("编号:"), "geke_code", 120},
        {"Domestic Source:", "domestic_source", 120},
        {"PCT Application No:", "application_no", 150},
        {"Country App No:", "country_app_no", 150},
        {"Title:", "title", 350},
        {"Handler:", "handler", 100},
        {"Inventor:", "inventor", 200},
        {"Filing Date:", "filing_date", 100},
        {"Application Date:", "application_date", 100},
        {"Priority Date:", "priority_date", 100},
        {"Country:", "country", 80},
        {"Notes:", "notes", 350},
    };
    wxComboBox* combo = nullptr;
    BuildSimpleDialogBase(this, rows, UTF8_STR("状态:"), fields_, combo);
    status_combo_ = combo;

    if (pct_id) LoadData();
    Bind(wxEVT_BUTTON, &PCTEditDialog::OnSave, this, wxID_OK);
}

void PCTEditDialog::LoadData() {
    PCTPatent p = db_->GetPCTById(pct_id_);
    SetFieldValue(fields_, "geke_code", p.geke_code);
    SetFieldValue(fields_, "domestic_source", p.domestic_source);
    SetFieldValue(fields_, "application_no", p.application_no);
    SetFieldValue(fields_, "country_app_no", p.country_app_no);
    SetFieldValue(fields_, "title", p.title);
    SetFieldValue(fields_, "handler", p.handler);
    SetFieldValue(fields_, "inventor", p.inventor);
    SetFieldValue(fields_, "filing_date", p.filing_date);
    SetFieldValue(fields_, "application_date", p.application_date);
    SetFieldValue(fields_, "priority_date", p.priority_date);
    SetFieldValue(fields_, "country", p.country);
    SetFieldValue(fields_, "notes", p.notes);
    status_combo_->SetValue(DB_STR(p.application_status));
}

void PCTEditDialog::OnSave(wxCommandEvent&) {
    PCTPatent p = pct_id_ ? db_->GetPCTById(pct_id_) : PCTPatent();
    p.geke_code = FieldValue(fields_, "geke_code");
    p.domestic_source = FieldValue(fields_, "domestic_source");
    p.application_no = FieldValue(fields_, "application_no");
    p.country_app_no = FieldValue(fields_, "country_app_no");
    p.title = FieldValue(fields_, "title");
    p.application_status = ToStd(status_combo_->GetValue());
    p.handler = FieldValue(fields_, "handler");
    p.inventor = FieldValue(fields_, "inventor");
    p.filing_date = FieldValue(fields_, "filing_date");
    p.application_date = FieldValue(fields_, "application_date");
    p.priority_date = FieldValue(fields_, "priority_date");
    p.country = FieldValue(fields_, "country");
    p.notes = FieldValue(fields_, "notes");

    if (p.geke_code.empty()) {
        wxMessageBox(UTF8_STR("编号不能为空"), UTF8_STR("错误"), wxOK | wxICON_ERROR);
        return;
    }

    bool success;
    if (pct_id_) {
        success = db_->UpdatePCT(pct_id_, p);   // full-field update, not title-only
    } else {
        success = db_->InsertPCT(p) > 0;
    }
    if (success) EndModal(wxID_OK);
    else wxMessageBox("Failed to save PCT: " + db_->LastError(), "Error", wxOK | wxICON_ERROR);
}

// ===========================================================================
// Software
// ===========================================================================

SoftwareEditDialog::SoftwareEditDialog(wxWindow* parent, Database* db, int sw_id)
    : wxDialog(parent, wxID_ANY, sw_id ? "Edit Software Copyright" : "New Software Copyright",
               wxDefaultPosition, wxSize(560, 620)),
      db_(db), sw_id_(sw_id) {
    std::vector<DialogRow> rows = {
        {"Case No:", "case_no", 120},
        {"Reg No:", "reg_no", 120},
        {"Title:", "title", 350},
        {"Original Owner:", "original_owner", 200},
        {"Current Owner:", "current_owner", 200},
        {"Handler:", "handler", 100},
        {"Developer:", "developer", 200},
        {"Inventor:", "inventor", 200},
        {"Dev Complete Date:", "dev_complete_date", 100},
        {"Application Date:", "application_date", 100},
        {"Reg Date:", "reg_date", 100},
        {"Version:", "version", 80},
        {"Notes:", "notes", 350},
    };
    wxComboBox* combo = nullptr;
    BuildSimpleDialogBase(this, rows, UTF8_STR("状态:"), fields_, combo);
    status_combo_ = combo;

    if (sw_id) LoadData();
    Bind(wxEVT_BUTTON, &SoftwareEditDialog::OnSave, this, wxID_OK);
}

void SoftwareEditDialog::LoadData() {
    SoftwareCopyright s = db_->GetSoftwareById(sw_id_);
    SetFieldValue(fields_, "case_no", s.case_no);
    SetFieldValue(fields_, "reg_no", s.reg_no);
    SetFieldValue(fields_, "title", s.title);
    SetFieldValue(fields_, "original_owner", s.original_owner);
    SetFieldValue(fields_, "current_owner", s.current_owner);
    SetFieldValue(fields_, "handler", s.handler);
    SetFieldValue(fields_, "developer", s.developer);
    SetFieldValue(fields_, "inventor", s.inventor);
    SetFieldValue(fields_, "dev_complete_date", s.dev_complete_date);
    SetFieldValue(fields_, "application_date", s.application_date);
    SetFieldValue(fields_, "reg_date", s.reg_date);
    SetFieldValue(fields_, "version", s.version);
    SetFieldValue(fields_, "notes", s.notes);
    status_combo_->SetValue(DB_STR(s.application_status));
}

void SoftwareEditDialog::OnSave(wxCommandEvent&) {
    SoftwareCopyright s = sw_id_ ? db_->GetSoftwareById(sw_id_) : SoftwareCopyright();
    s.case_no = FieldValue(fields_, "case_no");
    s.reg_no = FieldValue(fields_, "reg_no");
    s.title = FieldValue(fields_, "title");
    s.original_owner = FieldValue(fields_, "original_owner");
    s.current_owner = FieldValue(fields_, "current_owner");
    s.application_status = ToStd(status_combo_->GetValue());
    s.handler = FieldValue(fields_, "handler");
    s.developer = FieldValue(fields_, "developer");
    s.inventor = FieldValue(fields_, "inventor");
    s.dev_complete_date = FieldValue(fields_, "dev_complete_date");
    s.application_date = FieldValue(fields_, "application_date");
    s.reg_date = FieldValue(fields_, "reg_date");
    s.version = FieldValue(fields_, "version");
    s.notes = FieldValue(fields_, "notes");

    if (s.case_no.empty()) {
        wxMessageBox("Case No is required", "Error", wxOK | wxICON_ERROR);
        return;
    }

    // THE fix: editing updates the record instead of inserting a duplicate
    bool success;
    if (sw_id_) {
        success = db_->UpdateSoftware(sw_id_, s);
    } else {
        success = db_->InsertSoftware(s) > 0;
    }
    if (success) EndModal(wxID_OK);
    else wxMessageBox("Failed to save: " + db_->LastError(), "Error", wxOK | wxICON_ERROR);
}

// ===========================================================================
// IC Layout
// ===========================================================================

ICEditDialog::ICEditDialog(wxWindow* parent, Database* db, int ic_id)
    : wxDialog(parent, wxID_ANY, ic_id ? "Edit IC Layout" : "New IC Layout",
               wxDefaultPosition, wxSize(560, 600)),
      db_(db), ic_id_(ic_id) {
    std::vector<DialogRow> rows = {
        {"Case No:", "case_no", 120},
        {"Reg No:", "reg_no", 120},
        {"Title:", "title", 350},
        {"Original Owner:", "original_owner", 200},
        {"Current Owner:", "current_owner", 200},
        {"Handler:", "handler", 100},
        {"Designer:", "designer", 150},
        {"Inventor:", "inventor", 150},
        {"Application Date:", "application_date", 100},
        {"Creation Date:", "creation_date", 100},
        {"Cert Date:", "cert_date", 100},
        {"Notes:", "notes", 350},
    };
    wxComboBox* combo = nullptr;
    BuildSimpleDialogBase(this, rows, UTF8_STR("状态:"), fields_, combo);
    status_combo_ = combo;

    if (ic_id) LoadData();
    Bind(wxEVT_BUTTON, &ICEditDialog::OnSave, this, wxID_OK);
}

void ICEditDialog::LoadData() {
    ICLayout ic = db_->GetICById(ic_id_);
    SetFieldValue(fields_, "case_no", ic.case_no);
    SetFieldValue(fields_, "reg_no", ic.reg_no);
    SetFieldValue(fields_, "title", ic.title);
    SetFieldValue(fields_, "original_owner", ic.original_owner);
    SetFieldValue(fields_, "current_owner", ic.current_owner);
    SetFieldValue(fields_, "handler", ic.handler);
    SetFieldValue(fields_, "designer", ic.designer);
    SetFieldValue(fields_, "inventor", ic.inventor);
    SetFieldValue(fields_, "application_date", ic.application_date);
    SetFieldValue(fields_, "creation_date", ic.creation_date);
    SetFieldValue(fields_, "cert_date", ic.cert_date);
    SetFieldValue(fields_, "notes", ic.notes);
    status_combo_->SetValue(DB_STR(ic.application_status));
}

void ICEditDialog::OnSave(wxCommandEvent&) {
    ICLayout ic = ic_id_ ? db_->GetICById(ic_id_) : ICLayout();
    ic.case_no = FieldValue(fields_, "case_no");
    ic.reg_no = FieldValue(fields_, "reg_no");
    ic.title = FieldValue(fields_, "title");
    ic.original_owner = FieldValue(fields_, "original_owner");
    ic.current_owner = FieldValue(fields_, "current_owner");
    ic.application_status = ToStd(status_combo_->GetValue());
    ic.handler = FieldValue(fields_, "handler");
    ic.designer = FieldValue(fields_, "designer");
    ic.inventor = FieldValue(fields_, "inventor");
    ic.application_date = FieldValue(fields_, "application_date");
    ic.creation_date = FieldValue(fields_, "creation_date");
    ic.cert_date = FieldValue(fields_, "cert_date");
    ic.notes = FieldValue(fields_, "notes");

    if (ic.case_no.empty()) {
        wxMessageBox("Case No is required", "Error", wxOK | wxICON_ERROR);
        return;
    }

    bool success;
    if (ic_id_) {
        success = db_->UpdateIC(ic_id_, ic);
    } else {
        success = db_->InsertIC(ic) > 0;
    }
    if (success) EndModal(wxID_OK);
    else wxMessageBox("Failed to save: " + db_->LastError(), "Error", wxOK | wxICON_ERROR);
}

// ===========================================================================
// Foreign patent
// ===========================================================================

ForeignEditDialog::ForeignEditDialog(wxWindow* parent, Database* db, int fp_id)
    : wxDialog(parent, wxID_ANY, fp_id ? "Edit Foreign Patent" : "New Foreign Patent",
               wxDefaultPosition, wxSize(560, 640)),
      db_(db), fp_id_(fp_id) {
    std::vector<DialogRow> rows = {
        {"Case No:", "case_no", 120},
        {"PCT No:", "pct_no", 150},
        {"Country App No:", "country_app_no", 150},
        {"Title:", "title", 350},
        {"Owner:", "owner", 200},
        {"Handler:", "handler", 100},
        {"Inventor:", "inventor", 200},
        {"Country:", "country", 80},
        {"Application No:", "application_no", 150},
        {"Application Date:", "application_date", 100},
        {"Authorization Date:", "authorization_date", 100},
        {"Notes:", "notes", 350},
    };
    wxComboBox* combo = nullptr;
    BuildSimpleDialogBase(this, rows, UTF8_STR("状态:"), fields_, combo);
    status_combo_ = combo;

    if (fp_id) LoadData();
    Bind(wxEVT_BUTTON, &ForeignEditDialog::OnSave, this, wxID_OK);
}

void ForeignEditDialog::LoadData() {
    ForeignPatent f = db_->GetForeignById(fp_id_);
    SetFieldValue(fields_, "case_no", f.case_no);
    SetFieldValue(fields_, "pct_no", f.pct_no);
    SetFieldValue(fields_, "country_app_no", f.country_app_no);
    SetFieldValue(fields_, "title", f.title);
    SetFieldValue(fields_, "owner", f.owner);
    SetFieldValue(fields_, "handler", f.handler);
    SetFieldValue(fields_, "inventor", f.inventor);
    SetFieldValue(fields_, "country", f.country);
    SetFieldValue(fields_, "application_no", f.application_no);
    SetFieldValue(fields_, "application_date", f.application_date);
    SetFieldValue(fields_, "authorization_date", f.authorization_date);
    SetFieldValue(fields_, "notes", f.notes);
    status_combo_->SetValue(DB_STR(f.patent_status));
}

void ForeignEditDialog::OnSave(wxCommandEvent&) {
    ForeignPatent f = fp_id_ ? db_->GetForeignById(fp_id_) : ForeignPatent();
    f.case_no = FieldValue(fields_, "case_no");
    f.pct_no = FieldValue(fields_, "pct_no");
    f.country_app_no = FieldValue(fields_, "country_app_no");
    f.title = FieldValue(fields_, "title");
    f.owner = FieldValue(fields_, "owner");
    f.patent_status = ToStd(status_combo_->GetValue());
    f.handler = FieldValue(fields_, "handler");
    f.inventor = FieldValue(fields_, "inventor");
    f.country = FieldValue(fields_, "country");
    f.application_no = FieldValue(fields_, "application_no");
    f.application_date = FieldValue(fields_, "application_date");
    f.authorization_date = FieldValue(fields_, "authorization_date");
    f.notes = FieldValue(fields_, "notes");

    if (f.case_no.empty()) {
        wxMessageBox("Case No is required", "Error", wxOK | wxICON_ERROR);
        return;
    }

    bool success;
    if (fp_id_) {
        success = db_->UpdateForeign(fp_id_, f);
    } else {
        success = db_->InsertForeign(f) > 0;
    }
    if (success) EndModal(wxID_OK);
    else wxMessageBox("Failed to save: " + db_->LastError(), "Error", wxOK | wxICON_ERROR);
}

// ===========================================================================
// Deadline rule
// ===========================================================================

DeadlineRuleEditDialog::DeadlineRuleEditDialog(wxWindow* parent, Database* db, int rule_id)
    : wxDialog(parent, wxID_ANY, rule_id ? "Edit Deadline Rule" : "New Deadline Rule",
               wxDefaultPosition, wxSize(520, 480)),
      db_(db), rule_id_(rule_id) {
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    wxPanel* panel = new wxPanel(this);
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    auto add = [&](const wxString& label, wxTextCtrl** out, int w = 150) {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(panel, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        *out = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(w, -1));
        row->Add(*out, 1);
        sizer->Add(row, 0, wxEXPAND | wxALL, 4);
    };

    add("Jurisdiction (CN/US/PCT...):", &jurisdiction_field_, 80);
    add("Event Type (oa_response/...):", &event_type_field_, 150);
    add("Description:", &description_field_, 250);

    wxBoxSizer* row_m = new wxBoxSizer(wxHORIZONTAL);
    row_m->Add(new wxStaticText(panel, wxID_ANY, "Base months:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    months_spin_ = new wxSpinCtrl(panel, wxID_ANY, "0", wxDefaultPosition, wxDefaultSize,
                                  wxSP_ARROW_KEYS, 0, 60, 0);
    row_m->Add(months_spin_, 0, wxRIGHT, 20);
    row_m->Add(new wxStaticText(panel, wxID_ANY, "Base days:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    days_spin_ = new wxSpinCtrl(panel, wxID_ANY, "0", wxDefaultPosition, wxDefaultSize,
                                wxSP_ARROW_KEYS, 0, 365, 0);
    row_m->Add(days_spin_, 1);
    sizer->Add(row_m, 0, wxEXPAND | wxALL, 4);

    wxBoxSizer* row_e = new wxBoxSizer(wxHORIZONTAL);
    extendable_check_ = new wxCheckBox(panel, wxID_ANY, "Extendable");
    row_e->Add(extendable_check_, 0, wxRIGHT, 20);
    row_e->Add(new wxStaticText(panel, wxID_ANY, "Max extension (months):"), 0,
               wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    max_ext_spin_ = new wxSpinCtrl(panel, wxID_ANY, "0", wxDefaultPosition, wxDefaultSize,
                                   wxSP_ARROW_KEYS, 0, 24, 0);
    row_e->Add(max_ext_spin_, 0);
    sizer->Add(row_e, 0, wxALL, 4);

    enabled_check_ = new wxCheckBox(panel, wxID_ANY, "Enabled");
    sizer->Add(enabled_check_, 0, wxALL, 4);
    enabled_check_->SetValue(true);

    add("Notes:", &notes_field_, 250);
    notes_field_->SetSize(wxSize(250, 60));

    panel->SetSizer(sizer);
    main_sizer->Add(panel, 1, wxEXPAND | wxALL, 10);

    wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(new wxButton(this, wxID_OK, "Save"), 0, wxRIGHT, 10);
    btn_sizer->Add(new wxButton(this, wxID_CANCEL, "Cancel"), 0);
    main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

    SetSizer(main_sizer);
    Centre();
    Bind(wxEVT_BUTTON, &DeadlineRuleEditDialog::OnSave, this, wxID_OK);

    if (rule_id) LoadData();
}

void DeadlineRuleEditDialog::LoadData() {
    auto rules = db_->GetDeadlineRules();
    for (const auto& r : rules) {
        if (r.id == rule_id_) {
            jurisdiction_field_->SetValue(DB_STR(r.jurisdiction));
            event_type_field_->SetValue(DB_STR(r.event_type));
            description_field_->SetValue(DB_STR(r.rule_description));
            months_spin_->SetValue(r.base_months);
            days_spin_->SetValue(r.base_days);
            extendable_check_->SetValue(r.extendable);
            max_ext_spin_->SetValue(r.max_extension_months);
            enabled_check_->SetValue(r.enabled);
            notes_field_->SetValue(DB_STR(r.notes));
            return;
        }
    }
}

void DeadlineRuleEditDialog::OnSave(wxCommandEvent&) {
    DeadlineRule rule;
    rule.id = rule_id_;
    rule.jurisdiction = ToStd(jurisdiction_field_->GetValue());
    rule.event_type = ToStd(event_type_field_->GetValue());
    rule.rule_description = ToStd(description_field_->GetValue());
    rule.base_months = months_spin_->GetValue();
    rule.base_days = days_spin_->GetValue();
    rule.extendable = extendable_check_->GetValue();
    rule.max_extension_months = max_ext_spin_->GetValue();
    rule.enabled = enabled_check_->GetValue();
    rule.notes = ToStd(notes_field_->GetValue());

    if (rule.jurisdiction.empty() || rule.event_type.empty()) {
        wxMessageBox("Jurisdiction and event type are required", "Error", wxOK | wxICON_ERROR);
        return;
    }

    bool ok = rule_id_ ? db_->UpdateDeadlineRule(rule) : db_->InsertDeadlineRule(rule);
    if (ok) EndModal(wxID_OK);
    else wxMessageBox("Failed to save rule: " + db_->LastError(), "Error", wxOK | wxICON_ERROR);
}
