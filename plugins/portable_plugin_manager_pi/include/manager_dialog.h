#ifndef PORTABLE_PLUGIN_MANAGER_DIALOG_H
#define PORTABLE_PLUGIN_MANAGER_DIALOG_H

#include <wx/dialog.h>

class wxButton;
class wxListCtrl;
class wxListEvent;
class wxStaticText;

namespace ppm {

class ManagerDialog final : public wxDialog {
 public:
  explicit ManagerDialog(wxWindow* parent);

  void SetRuntimeSummary(const wxString& summary);
  void SetStatus(const wxString& status);

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
