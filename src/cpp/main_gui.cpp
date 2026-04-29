// patX GUI - wxWidgets
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/artprov.h>

// PatXFrame - Main window
class PatXFrame : public wxFrame {
public:
    PatXFrame() 
        : wxFrame(nullptr, wxID_ANY, "patX - Patent Manager", 
                  wxDefaultPosition, wxSize(900, 600)) {
        
        // Menu
        wxMenuBar* menuBar = new wxMenuBar;
        wxMenu* menuFile = new wxMenu;
        menuFile->Append(wxID_EXIT, "E&xit\tAlt+F4");
        wxMenu* menuHelp = new wxMenu;
        menuHelp->Append(wxID_ABOUT, "&About");
        menuBar->Append(menuFile, "&File");
        menuBar->Append(menuHelp, "&Help");
        SetMenuBar(menuBar);
        
        // Toolbar
        wxToolBar* toolbar = CreateToolBar();
        toolbar->AddTool(wxID_NEW, "Add Patent", wxArtProvider::GetBitmap(wxART_NEW));
        toolbar->Realize();
        
        // Status bar
        CreateStatusBar();
        SetStatusText("Ready");
        
        // Main panel
        wxPanel* panel = new wxPanel(this);
        wxBoxSizer* topSizer = new wxBoxSizer(wxVERTICAL);
        
        // Search bar
        wxBoxSizer* searchSizer = new wxBoxSizer(wxHORIZONTAL);
        searchSizer->Add(new wxStaticText(panel, wxID_ANY, "Search: "), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        wxTextCtrl* searchCtrl = new wxTextCtrl(panel, wxID_FIND, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        searchSizer->Add(searchCtrl, 1, wxEXPAND);
        topSizer->Add(searchSizer, 0, wxEXPAND | wxALL, 5);
        
        // List control for patents
        wxListCtrl* list = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                          wxLC_REPORT | wxLC_SINGLE_SEL);
        list->AppendColumn("ID", wxLIST_FORMAT_LEFT, 60);
        list->AppendColumn("GEKE Code", wxLIST_FORMAT_LEFT, 100);
        list->AppendColumn("Application No", wxLIST_FORMAT_LEFT, 180);
        list->AppendColumn("Title", wxLIST_FORMAT_LEFT, 250);
        list->AppendColumn("Applicant", wxLIST_FORMAT_LEFT, 150);
        list->AppendColumn("Status", wxLIST_FORMAT_LEFT, 80);
        
        // Add sample data
        long idx = list->InsertItem(0, "1");
        list->SetItem(idx, 1, "GC-0001");
        list->SetItem(idx, 2, "CN202310000001.X");
        list->SetItem(idx, 3, "Test Patent 1");
        list->SetItem(idx, 4, "Company A");
        list->SetItem(idx, 5, "Active");
        
        idx = list->InsertItem(1, "2");
        list->SetItem(idx, 1, "GC-0002");
        list->SetItem(idx, 2, "CN202310000002.X");
        list->SetItem(idx, 3, "Test Patent 2");
        list->SetItem(idx, 4, "Company B");
        list->SetItem(idx, 5, "Pending");
        
        topSizer->Add(list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
        
        panel->SetSizer(topSizer);
        
        // Bind events
        Bind(wxEVT_MENU, &PatXFrame::OnExit, this, wxID_EXIT);
        Bind(wxEVT_MENU, &PatXFrame::OnAbout, this, wxID_ABOUT);
        
        SetStatusText("2 patents loaded");
    }
    
private:
    void OnExit(wxCommandEvent&) {
        Close(true);
    }
    
    void OnAbout(wxCommandEvent&) {
        wxMessageBox("patX v0.1.0\n\nHigh Performance Patent Manager\nC++/Rust Hybrid\n\nGreen, Portable, Fast.",
                    "About patX", wxOK | wxICON_INFORMATION);
    }
};

// Application
class PatXApp : public wxApp {
public:
    bool OnInit() override {
        PatXFrame* frame = new PatXFrame();
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(PatXApp);