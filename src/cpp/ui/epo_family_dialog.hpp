#pragma once
#include <string>
class wxWindow;
class Database;
namespace webdossier { class Manager; }
void ShowEpoFamilyDialog(wxWindow* parent, Database& db, webdossier::Manager& manager,
                         const std::string& publication);
