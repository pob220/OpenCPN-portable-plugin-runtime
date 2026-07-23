#ifndef PORTABLE_PLUGIN_MANAGER_DIALOG_H
#define PORTABLE_PLUGIN_MANAGER_DIALOG_H

#include <wx/dialog.h>

#include <vector>

class wxButton;
class wxListCtrl;
class wxListEvent;
class wxStaticText;

namespace ppm {

struct PackageSnapshot;

class ManagerDialog final : public wxDialog {
 public:
  explicit ManagerDialog(wxWindow* parent);

  void SetRuntimeSummary(const wxString& summary);
  void SetStatus(const wxString& status);
  void SetPackages(const std::vector<PackageSnapshot>& packages);

 private:
  void OnClose(wxCloseEvent& event);
  void OnInstall(wxCommandEvent& event);
  void OnSelectionChanged(wxListEvent& event);
  void RefreshButtonState();

  wxListCtrl* packages_ = nullptr;
  wxStaticText* runtime_summary_ = nullptr;
  wxStaticText* status_ = nullptr;
  wxButton* enable_ = nullptr;
  wxButton* disable_ = nullptr;
  wxButton* unload_ = nullptr;
  wxButton* remove_ = nullptr;
};

}  // namespace ppm

#endif
