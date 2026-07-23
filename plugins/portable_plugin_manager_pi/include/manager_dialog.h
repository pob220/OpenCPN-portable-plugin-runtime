#ifndef PORTABLE_PLUGIN_MANAGER_DIALOG_H
#define PORTABLE_PLUGIN_MANAGER_DIALOG_H

#include <wx/dialog.h>

#include <functional>
#include <string>
#include <vector>

class wxButton;
class wxListCtrl;
class wxListEvent;
class wxStaticText;

namespace ppm {

struct PackageSnapshot;
struct PermissionEvaluation;
struct StoredPackage;

bool ConfirmPermissionApproval(wxWindow* parent, const StoredPackage& package,
                               const PermissionEvaluation& evaluation);

struct ManagerCallbacks {
  std::function<void(const std::string&)> install;
  std::function<void(const std::string&)> enable;
  std::function<void(const std::string&)> disable;
  std::function<void(const std::string&)> unload;
  std::function<void(const std::string&)> remove;
  std::function<void(const std::string&)> rollback;
  std::function<void(const std::string&)> revoke_permissions;
};

class ManagerDialog final : public wxDialog {
 public:
  ManagerDialog(wxWindow* parent, ManagerCallbacks callbacks);

  void SetRuntimeSummary(const wxString& summary);
  void SetStatus(const wxString& status);
  void SetPackages(const std::vector<PackageSnapshot>& packages);

 private:
  void OnClose(wxCloseEvent& event);
  void OnInstall(wxCommandEvent& event);
  void OnEnable(wxCommandEvent& event);
  void OnDisable(wxCommandEvent& event);
  void OnUnload(wxCommandEvent& event);
  void OnRemove(wxCommandEvent& event);
  void OnRollback(wxCommandEvent& event);
  void OnRevokePermissions(wxCommandEvent& event);
  void OnSelectionChanged(wxListEvent& event);
  void RefreshButtonState();
  std::string SelectedPackageId() const;

  ManagerCallbacks callbacks_;
  std::vector<PackageSnapshot> snapshots_;
  wxListCtrl* packages_ = nullptr;
  wxStaticText* runtime_summary_ = nullptr;
  wxStaticText* status_ = nullptr;
  wxButton* enable_ = nullptr;
  wxButton* disable_ = nullptr;
  wxButton* unload_ = nullptr;
  wxButton* remove_ = nullptr;
  wxButton* rollback_ = nullptr;
  wxButton* revoke_permissions_ = nullptr;
};

}  // namespace ppm

#endif
