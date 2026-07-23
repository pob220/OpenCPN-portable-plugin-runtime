#include "manager_dialog.h"

#include <algorithm>

#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>

#include <utility>

#include "runtime_engine.h"
#include "permission_store.h"

namespace ppm {
namespace {

enum : int {
  kInstall = wxID_HIGHEST + 260,
  kEnable,
  kDisable,
  kUnload,
  kRemove,
  kRollback,
  kRevokePermissions,
};

wxString RiskLabel(PermissionRisk risk) {
  switch (risk) {
    case PermissionRisk::kLow:
      return "Low";
    case PermissionRisk::kModerate:
      return "Moderate";
    case PermissionRisk::kHigh:
      return "High";
    case PermissionRisk::kNative:
      return "Native code";
  }
  return "Unknown";
}

}  // namespace

bool ConfirmPermissionApproval(wxWindow* parent,
                               const StoredPackage& package,
                               const PermissionEvaluation& evaluation) {
  wxDialog dialog(parent, wxID_ANY, "Portable package permissions",
                  wxDefaultPosition, wxSize(850, 560),
                  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* root = new wxBoxSizer(wxVERTICAL);
  auto* heading = new wxStaticText(
      &dialog, wxID_ANY,
      wxString::FromUTF8(package.name) + " is requesting host access");
  wxFont heading_font = heading->GetFont();
  heading_font.SetWeight(wxFONTWEIGHT_BOLD);
  heading->SetFont(heading_font);
  root->Add(heading, 0, wxALL, 12);
  root->Add(
      new wxStaticText(
          &dialog, wxID_ANY,
          "Portable packages run in WebAssembly, and only the approved "
          "capabilities below are exposed. All listed capabilities are "
          "required by this package."),
      0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

  auto* permissions = new wxListCtrl(
      &dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize,
      wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
  permissions->InsertColumn(0, "Access");
  permissions->InsertColumn(1, "Risk");
  permissions->InsertColumn(2, "What it permits");
  permissions->SetColumnWidth(0, 220);
  permissions->SetColumnWidth(1, 100);
  permissions->SetColumnWidth(2, 480);
  for (const auto& id : evaluation.requested) {
    const PermissionDescriptor* descriptor = FindPermission(id);
    if (!descriptor) continue;
    const bool added =
        std::find(evaluation.added.begin(), evaluation.added.end(), id) !=
        evaluation.added.end();
    const long row = permissions->InsertItem(
        permissions->GetItemCount(),
        (added ? "New — " : "") + wxString::FromUTF8(descriptor->label));
    permissions->SetItem(row, 1, RiskLabel(descriptor->risk));
    permissions->SetItem(row, 2,
                         wxString::FromUTF8(descriptor->description));
  }
  root->Add(permissions, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

  if (!evaluation.removed.empty()) {
    root->Add(
        new wxStaticText(
            &dialog, wxID_ANY,
            wxString::Format(
                "%zu previously approved %s will be removed.",
                evaluation.removed.size(),
                evaluation.removed.size() == 1 ? "capability"
                                               : "capabilities")),
        0, wxALL, 12);
  }
  root->Add(
      new wxStaticText(
          &dialog, wxID_ANY,
          "Approval is stored only for this package ID and exact capability "
          "set. Any future access expansion requires a new decision."),
      0, wxLEFT | wxRIGHT | wxTOP, 12);
  auto* buttons = dialog.CreateButtonSizer(wxOK | wxCANCEL);
  auto* approve = wxDynamicCast(dialog.FindWindow(wxID_OK), wxButton);
  if (approve) approve->SetLabel("Approve access");
  root->Add(buttons, 0, wxEXPAND | wxALL, 12);
  dialog.SetSizer(root);
  return dialog.ShowModal() == wxID_OK;
}

ManagerDialog::ManagerDialog(wxWindow* parent, ManagerCallbacks callbacks)
    : wxDialog(parent, wxID_ANY, "Portable Plugin Manager",
               wxDefaultPosition, wxSize(980, 540),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      callbacks_(std::move(callbacks)) {
  auto* root = new wxBoxSizer(wxVERTICAL);
  auto* heading = new wxStaticText(
      this, wxID_ANY,
      "Portable runtime packages run behind a capability-limited host.");
  wxFont heading_font = heading->GetFont();
  heading_font.SetWeight(wxFONTWEIGHT_BOLD);
  heading->SetFont(heading_font);
  root->Add(heading, 0, wxALL, 12);

  runtime_summary_ = new wxStaticText(
      this, wxID_ANY,
      "Host ready — no portable packages have been installed.");
  root->Add(runtime_summary_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

  packages_ = new wxListCtrl(
      this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
      wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
  packages_->InsertColumn(0, "Package");
  packages_->InsertColumn(1, "Version");
  packages_->InsertColumn(2, "State");
  packages_->InsertColumn(3, "Access");
  packages_->InsertColumn(4, "Details");
  packages_->SetColumnWidth(0, 270);
  packages_->SetColumnWidth(1, 90);
  packages_->SetColumnWidth(2, 120);
  packages_->SetColumnWidth(3, 140);
  packages_->SetColumnWidth(4, 250);
  root->Add(packages_, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

  auto* actions = new wxBoxSizer(wxHORIZONTAL);
  actions->Add(new wxButton(this, kInstall, "Install package…"), 0,
               wxRIGHT, 8);
  enable_ = new wxButton(this, kEnable, "Enable");
  disable_ = new wxButton(this, kDisable, "Disable");
  unload_ = new wxButton(this, kUnload, "Unload");
  remove_ = new wxButton(this, kRemove, "Remove…");
  rollback_ = new wxButton(this, kRollback, "Rollback…");
  revoke_permissions_ =
      new wxButton(this, kRevokePermissions, "Revoke access…");
  actions->Add(enable_, 0, wxRIGHT, 8);
  actions->Add(disable_, 0, wxRIGHT, 8);
  actions->Add(unload_, 0, wxRIGHT, 8);
  actions->Add(remove_, 0, wxRIGHT, 8);
  actions->Add(rollback_, 0, wxRIGHT, 8);
  actions->Add(revoke_permissions_, 0, wxRIGHT, 8);
  actions->AddStretchSpacer();
  actions->Add(new wxButton(this, wxID_CLOSE, "Close"), 0);
  root->Add(actions, 0, wxEXPAND | wxALL, 12);

  root->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
  status_ = new wxStaticText(
      this, wxID_ANY, "Ready.");
  status_->Wrap(930);
  root->Add(status_, 0, wxEXPAND | wxALL, 12);
  SetSizer(root);

  Bind(wxEVT_CLOSE_WINDOW, &ManagerDialog::OnClose, this);
  Bind(wxEVT_BUTTON, &ManagerDialog::OnInstall, this, kInstall);
  Bind(wxEVT_BUTTON, &ManagerDialog::OnEnable, this, kEnable);
  Bind(wxEVT_BUTTON, &ManagerDialog::OnDisable, this, kDisable);
  Bind(wxEVT_BUTTON, &ManagerDialog::OnUnload, this, kUnload);
  Bind(wxEVT_BUTTON, &ManagerDialog::OnRemove, this, kRemove);
  Bind(wxEVT_BUTTON, &ManagerDialog::OnRollback, this, kRollback);
  Bind(wxEVT_BUTTON, &ManagerDialog::OnRevokePermissions, this,
       kRevokePermissions);
  Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Hide(); }, wxID_CLOSE);
  packages_->Bind(wxEVT_LIST_ITEM_SELECTED,
                  &ManagerDialog::OnSelectionChanged, this);
  packages_->Bind(wxEVT_LIST_ITEM_DESELECTED,
                  &ManagerDialog::OnSelectionChanged, this);
  RefreshButtonState();
}

void ManagerDialog::SetRuntimeSummary(const wxString& summary) {
  runtime_summary_->SetLabel(summary);
  Layout();
}

void ManagerDialog::SetStatus(const wxString& status) {
  status_->SetLabel(status);
  status_->Wrap(GetClientSize().GetWidth() - 24);
  Layout();
}

void ManagerDialog::SetPackages(
    const std::vector<PackageSnapshot>& packages) {
  const std::string selected = SelectedPackageId();
  snapshots_ = packages;
  packages_->DeleteAllItems();
  for (std::size_t index = 0; index < packages.size(); ++index) {
    const auto& package = packages[index];
    const long row = packages_->InsertItem(
        packages_->GetItemCount(), wxString::FromUTF8(package.name));
    packages_->SetItem(row, 1, wxString::FromUTF8(package.version));
    packages_->SetItem(row, 2, wxString::FromUTF8(package.state));
    packages_->SetItem(row, 3, wxString::FromUTF8(package.access));
    packages_->SetItem(
        row, 4,
        package.diagnostic.empty() ? "Wasmtime component"
                                   : wxString::FromUTF8(package.diagnostic));
    packages_->SetItemData(row, static_cast<long>(index));
    if (package.id == selected)
      packages_->SetItemState(row, wxLIST_STATE_SELECTED,
                              wxLIST_STATE_SELECTED);
  }
  RefreshButtonState();
}

void ManagerDialog::OnClose(wxCloseEvent& event) {
  if (event.CanVeto()) {
    Hide();
    event.Veto();
  } else {
    event.Skip();
  }
}

void ManagerDialog::OnInstall(wxCommandEvent&) {
  wxFileDialog picker(
      this, "Install portable runtime package", wxEmptyString, wxEmptyString,
      "OpenCPN portable packages (*.ocpnp)|*.ocpnp|All files|*",
      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
  if (picker.ShowModal() != wxID_OK) return;
  if (callbacks_.install)
    callbacks_.install(picker.GetPath().ToStdString());
}

std::string ManagerDialog::SelectedPackageId() const {
  if (!packages_) return {};
  const long row =
      packages_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (row < 0) return {};
  const long index = packages_->GetItemData(row);
  if (index < 0 || static_cast<std::size_t>(index) >= snapshots_.size())
    return {};
  return snapshots_[static_cast<std::size_t>(index)].id;
}

void ManagerDialog::OnEnable(wxCommandEvent&) {
  const std::string id = SelectedPackageId();
  if (!id.empty() && callbacks_.enable) callbacks_.enable(id);
}

void ManagerDialog::OnDisable(wxCommandEvent&) {
  const std::string id = SelectedPackageId();
  if (!id.empty() && callbacks_.disable) callbacks_.disable(id);
}

void ManagerDialog::OnUnload(wxCommandEvent&) {
  const std::string id = SelectedPackageId();
  if (!id.empty() && callbacks_.unload) callbacks_.unload(id);
}

void ManagerDialog::OnRemove(wxCommandEvent&) {
  const std::string id = SelectedPackageId();
  if (id.empty() || !callbacks_.remove) return;
  if (wxMessageBox(
          "Remove " + wxString::FromUTF8(id) +
              "?\n\nThe package will be moved to recoverable storage. "
              "Its private data will be retained.",
          "Remove portable package", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
          this) == wxYES) {
    callbacks_.remove(id);
  }
}

void ManagerDialog::OnRollback(wxCommandEvent&) {
  const std::string id = SelectedPackageId();
  if (id.empty() || !callbacks_.rollback) return;
  if (wxMessageBox(
          "Replace " + wxString::FromUTF8(id) +
              " with its most recent retained version?\n\nThe package will "
              "remain disabled after rollback.",
          "Rollback portable package",
          wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) == wxYES) {
    callbacks_.rollback(id);
  }
}

void ManagerDialog::OnRevokePermissions(wxCommandEvent&) {
  const std::string id = SelectedPackageId();
  if (id.empty() || !callbacks_.revoke_permissions) return;
  if (wxMessageBox(
          "Revoke all approved access for " + wxString::FromUTF8(id) +
              "?\n\nThe package will be disabled and unloaded. It must ask "
              "again before it can run.",
          "Revoke portable package access",
          wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) == wxYES) {
    callbacks_.revoke_permissions(id);
  }
}

void ManagerDialog::OnSelectionChanged(wxListEvent&) { RefreshButtonState(); }

void ManagerDialog::RefreshButtonState() {
  const long row =
      packages_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  const bool selected = row >= 0;
  std::string state;
  if (selected) {
    const long index = packages_->GetItemData(row);
    if (index >= 0 && static_cast<std::size_t>(index) < snapshots_.size())
      state = snapshots_[static_cast<std::size_t>(index)].state;
  }
  enable_->Enable(selected && state != "Enabled");
  disable_->Enable(selected && state == "Enabled");
  unload_->Enable(selected && (state == "Enabled" || state == "Disabled"));
  remove_->Enable(selected);
  rollback_->Enable(selected);
  revoke_permissions_->Enable(selected);
}

}  // namespace ppm
