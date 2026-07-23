#include "manager_dialog.h"

#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>

namespace ppm {
namespace {

enum : int {
  kInstall = wxID_HIGHEST + 260,
  kEnable,
  kDisable,
  kUnload,
  kRemove,
};

}  // namespace

ManagerDialog::ManagerDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Portable Plugin Manager",
               wxDefaultPosition, wxSize(820, 520),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
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
  packages_->InsertColumn(3, "Runtime");
  packages_->SetColumnWidth(0, 270);
  packages_->SetColumnWidth(1, 90);
  packages_->SetColumnWidth(2, 120);
  packages_->SetColumnWidth(3, 250);
  root->Add(packages_, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

  auto* actions = new wxBoxSizer(wxHORIZONTAL);
  actions->Add(new wxButton(this, kInstall, "Install package…"), 0,
               wxRIGHT, 8);
  enable_ = new wxButton(this, kEnable, "Enable");
  disable_ = new wxButton(this, kDisable, "Disable");
  unload_ = new wxButton(this, kUnload, "Unload");
  remove_ = new wxButton(this, kRemove, "Remove…");
  actions->Add(enable_, 0, wxRIGHT, 8);
  actions->Add(disable_, 0, wxRIGHT, 8);
  actions->Add(unload_, 0, wxRIGHT, 8);
  actions->Add(remove_, 0, wxRIGHT, 8);
  actions->AddStretchSpacer();
  actions->Add(new wxButton(this, wxID_CLOSE, "Close"), 0);
  root->Add(actions, 0, wxEXPAND | wxALL, 12);

  root->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
  status_ = new wxStaticText(
      this, wxID_ANY,
      "Ready. Install, verification and lifecycle controls are being "
      "connected in the next implementation slice.");
  status_->Wrap(770);
  root->Add(status_, 0, wxEXPAND | wxALL, 12);
  SetSizer(root);

  Bind(wxEVT_CLOSE_WINDOW, &ManagerDialog::OnClose, this);
  Bind(wxEVT_BUTTON, &ManagerDialog::OnInstall, this, kInstall);
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
  SetStatus("Selected " + picker.GetPath() +
            ". Package verification is not enabled in this host-shell "
            "milestone, so no files were changed.");
}

void ManagerDialog::OnSelectionChanged(wxListEvent&) { RefreshButtonState(); }

void ManagerDialog::RefreshButtonState() {
  const bool selected =
      packages_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) >= 0;
  enable_->Enable(selected);
  disable_->Enable(selected);
  unload_->Enable(selected);
  remove_->Enable(selected);
}

}  // namespace ppm
