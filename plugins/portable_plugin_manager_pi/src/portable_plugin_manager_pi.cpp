#include "portable_plugin_manager_pi.h"

#include <wx/filename.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/utils.h>

#include <algorithm>
#include <utility>

#if defined(__WXOSX__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "manager_dialog.h"

#ifndef DECL_EXP
#ifdef __WXMSW__
#define DECL_EXP __declspec(dllexport)
#else
#define DECL_EXP __attribute__((visibility("default")))
#endif
#endif

namespace {

const char* const kManagerXpm[] = {
    "32 32 6 1",
    "  c None",
    ". c #19324A",
    "+ c #126F89",
    "@ c #FFF7DF",
    "# c #E45B3C",
    "$ c #FFFFFF",
    "                                ",
    "           ..........           ",
    "        ...++++++++++...        ",
    "      ..++++++++++++++++..      ",
    "     .++++++++++++++++++++.     ",
    "    .++++++@@++++@@++++++++.    ",
    "   .+++++++@@++++@@+++++++++.   ",
    "  .++++++++@@++++@@++++++++++.  ",
    "  .++++++++@@++++@@++++++++++.  ",
    " .+++++++++@@++++@@+++++++++++. ",
    " .+++++..................+++++. ",
    " .++++.@@@@@@@@@@@@@@@@@@.++++. ",
    ".+++++.@@@@@@@@@@@@@@@@@@.+++++.",
    ".+++++.@@@@@###@@###@@@@@.+++++.",
    ".+++++.@@@@##@@@@@@##@@@@.+++++.",
    ".+++++.@@@@@##@@@@##@@@@@.+++++.",
    ".+++++.@@@@@@##@@##@@@@@@.+++++.",
    ".+++++.@@@@@@@@@@@@@@@@@@.+++++.",
    ".++++++.@@@@@@@@@@@@@@@@.++++++.",
    ".+++++++.@@@@@@@@@@@@@@.+++++++.",
    " .++++++..@@@@@@@@@@@@..++++++. ",
    " .++++++++..@@@@@@@@..++++++++. ",
    " .++++++++++........++++++++++. ",
    "  .++++++++++++@@++++++++++++.  ",
    "  .++++++++++++@@++++++++++++.  ",
    "   .+++++++++++@@+++++++++++.   ",
    "    .++++++++++@@++++++++++.    ",
    "     .+++++++++@@@@@++++++.     ",
    "      ..++++++++++@@@@@@..      ",
    "        ...++++++++++...        ",
    "           ..........           ",
    "                                "};

const ppm::ActionKey kManagerAction{"org.opencpn.portable-plugin-manager",
                                    "open-manager"};

wxString ResolveStorageRoot() {
  wxString configured;
  if (wxGetEnv("OCPN_PORTABLE_PLUGIN_ROOT", &configured) &&
      !configured.IsEmpty()) {
    wxFileName path = wxFileName::DirName(configured);
    path.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
    return path.GetFullPath();
  }
  return *GetpPrivateApplicationDataLocation() + wxFILE_SEP_PATH +
         "portable-plugin-manager";
}

wxString ResolveManagerDataRoot() {
  const wxString installed =
      GetPluginDataDir("portable_plugin_manager_pi");
  if (!installed.empty() &&
      wxFileName::FileExists(
          installed + wxFileName::GetPathSeparator() + "manager.svg")) {
    return installed;
  }
  return wxString::FromUTF8(PPM_SOURCE_DATA_DIR);
}

wxString ResolveManagerIcon() {
  const wxString separator = wxFileName::GetPathSeparator();
  const wxString icon = ResolveManagerDataRoot() + separator + "manager.svg";
  return wxFileName::FileExists(icon) ? icon : wxString();
}

}  // namespace

extern "C" DECL_EXP opencpn_plugin* create_pi(void* manager) {
  return new ppm::PortablePluginManagerPi(manager);
}

extern "C" DECL_EXP void destroy_pi(opencpn_plugin* plugin) { delete plugin; }

namespace ppm {

PortablePluginManagerPi::PortablePluginManagerPi(void* manager)
    : opencpn_plugin_121(manager), plugin_bitmap_(kManagerXpm) {}

PortablePluginManagerPi::~PortablePluginManagerPi() {
  if (initialized_) DeInit();
}

int PortablePluginManagerPi::Init() {
  if (initialized_) {
    wxLogWarning("PPM event=duplicate-init");
    return WANTS_TOOLBAR_CALLBACK | INSTALLS_TOOLBAR_TOOL | WANTS_CONFIG |
           WANTS_NMEA_EVENTS | WANTS_OVERLAY_CALLBACK |
           WANTS_OPENGL_OVERLAY_CALLBACK;
  }
  initialized_ = true;
  storage_root_ = ResolveStorageRoot();
  wxLogMessage("PPM event=init api=1.21 version=0.2.0");
  wxLogMessage("PPM event=storage-root path=%s", storage_root_);
  if (!RegisterManagerAction()) {
    wxLogError("PPM event=manager-action-registration-failed");
  }
  wxString developer;
  developer_mode_ =
      wxGetEnv("OCPN_PPM_DEVELOPER_MODE", &developer) && developer == "1";
  const wxString trust_root =
      ResolveManagerDataRoot() + wxFileName::GetPathSeparator() + "trust";
  package_store_ = std::make_unique<PackageStore>(
      storage_root_.ToStdString(), trust_root.ToStdString());
  package_store_->SetDeveloperMode(developer_mode_);
  permission_store_ =
      std::make_unique<PermissionStore>(storage_root_.ToStdString());
  for (const auto& package : package_store_->Installed()) {
    if (!package.enabled) continue;
    const StoreResult audit = package_store_->AuditInstalled(package.id);
    if (!audit.okay) {
      package_store_->SetEnabled(package.id, false);
      wxLogError("PPM event=installed-integrity-failed package=%s "
                 "diagnostic=%s",
                 package.id, audit.message);
    }
  }
  runtime_engine_ = std::make_unique<RuntimeEngine>(
      storage_root_.ToStdString(),
      [this](const RuntimeAction& action, std::uint32_t* host_action_id) {
        return RegisterPortableAction(action, host_action_id);
      },
      [this](const std::string& package_id) {
        RemovePackageActions(package_id);
      },
      [this]() { OnEngineStateChanged(); });
  if (!runtime_engine_->LoadInstalled(developer_mode_)) {
    wxLogWarning(
        "PPM event=runtime-engine-load-completed-with-package-failures");
  }
  for (const auto& package : package_store_->Installed()) {
    if (!package.enabled) continue;
    wxString diagnostic;
    std::string runtime_diagnostic;
    if (!PreparePermissions(package.id, false, &diagnostic) ||
        !runtime_engine_->Enable(package.id, &runtime_diagnostic)) {
      package_store_->SetEnabled(package.id, false);
      wxLogWarning("PPM event=startup-package-disabled package=%s "
                   "diagnostic=%s%s",
                   package.id, diagnostic,
                   wxString::FromUTF8(runtime_diagnostic));
    }
  }
  return WANTS_TOOLBAR_CALLBACK | INSTALLS_TOOLBAR_TOOL | WANTS_CONFIG |
         WANTS_NMEA_EVENTS | WANTS_OVERLAY_CALLBACK |
         WANTS_OPENGL_OVERLAY_CALLBACK;
}

bool PortablePluginManagerPi::DeInit() {
  if (!initialized_) return true;
  if (manager_dialog_) {
    manager_dialog_->Hide();
    manager_dialog_->Destroy();
    manager_dialog_.release();
  }
  if (runtime_engine_) {
    runtime_engine_->Shutdown();
    runtime_engine_.reset();
  }
  permission_store_.reset();
  package_store_.reset();
  RemoveAllActions();
  initialized_ = false;
  wxLogMessage("PPM event=deinit remaining-actions=%zu", actions_.Size());
  return true;
}

wxString PortablePluginManagerPi::GetShortDescription() {
  return "Installs and manages capability-limited portable runtime plugins.";
}

wxString PortablePluginManagerPi::GetLongDescription() {
  return "A conventional OpenCPN plugin which verifies, installs, enables, "
         "disables and unloads portable WebAssembly packages without changes "
         "to OpenCPN core.";
}

bool PortablePluginManagerPi::RegisterManagerAction() {
  const wxString icon = ResolveManagerIcon();
  const int tool_id =
      icon.empty()
          ? InsertPlugInTool(
                "Portable Plugin Manager", &plugin_bitmap_, &plugin_bitmap_,
                wxITEM_NORMAL, "Portable Plugin Manager",
                "Install and manage portable runtime plugins", nullptr, -1, 0,
                this)
          : InsertPlugInToolSVG(
                "Portable Plugin Manager", icon, icon, icon, wxITEM_NORMAL,
                "Portable Plugin Manager",
                "Install and manage portable runtime plugins", nullptr, -1, 0,
                this);
  if (!actions_.Add(kManagerAction, tool_id)) {
    if (tool_id >= 0) RemovePlugInTool(tool_id);
    return false;
  }
  wxLogMessage("PPM event=action-registered package=%s action=%s tool=%d",
               kManagerAction.package_id, kManagerAction.action_id, tool_id);
  return true;
}

int PortablePluginManagerPi::RegisterPortableAction(
    const RuntimeAction& action, std::uint32_t* host_action_id) {
  if (!host_action_id) return -1;
  const ActionKey key{action.package_id, action.action_id};
  if (actions_.Find(key)) return -5;
  int tool_id = -1;
  const wxString label = wxString::FromUTF8(action.label);
  const wxString tooltip = wxString::FromUTF8(action.tooltip);
  if (!action.icon_path.empty()) {
    const wxString icon = wxString::FromUTF8(action.icon_path);
    tool_id = InsertPlugInToolSVG(label, icon, icon, icon, wxITEM_NORMAL,
                                  tooltip, tooltip, nullptr, -1, 0, this);
  } else {
    tool_id = InsertPlugInTool(label, &plugin_bitmap_, &plugin_bitmap_,
                               wxITEM_NORMAL, tooltip, tooltip, nullptr, -1, 0,
                               this);
  }
  if (!actions_.Add(key, tool_id)) {
    if (tool_id >= 0) RemovePlugInTool(tool_id);
    return -2;
  }
  SetToolbarToolViz(tool_id, true);
  *host_action_id = static_cast<std::uint32_t>(tool_id);
  wxLogMessage("PPM event=package-action-registered package=%s action=%s tool=%d",
               action.package_id, action.action_id, tool_id);
  return 0;
}

void PortablePluginManagerPi::RemoveAllActions() {
  for (const auto& action : actions_.Clear()) {
    RemovePlugInTool(action.tool_id);
    wxLogMessage("PPM event=action-removed package=%s action=%s tool=%d",
                 action.key.package_id, action.key.action_id, action.tool_id);
  }
}

void PortablePluginManagerPi::RemovePackageActions(
    const std::string& package_id) {
  for (const auto& action : actions_.RemovePackage(package_id)) {
    RemovePlugInTool(action.tool_id);
    wxLogMessage("PPM event=action-removed package=%s action=%s tool=%d",
                 action.key.package_id, action.key.action_id, action.tool_id);
  }
}

void PortablePluginManagerPi::OnToolbarToolCallback(int id) {
  const auto action = actions_.FindByToolId(id);
  if (!action || !action->dispatchable) {
    wxLogWarning("PPM event=unmapped-or-blocked-toolbar-click tool=%d", id);
    return;
  }
  if (action->key == kManagerAction) {
    ShowManager(nullptr);
  } else if (runtime_engine_) {
    runtime_engine_->HandleAction(action->key.package_id,
                                  action->key.action_id);
  }
}

void PortablePluginManagerPi::ShowPreferencesDialog(wxWindow* parent) {
  ShowManager(parent);
}

void PortablePluginManagerPi::ShowManager(wxWindow* parent) {
  if (!manager_dialog_) {
    ManagerCallbacks callbacks;
    callbacks.install =
        [this](const std::string& path) { InstallPackage(path); };
    callbacks.enable =
        [this](const std::string& id) { EnablePackage(id); };
    callbacks.disable =
        [this](const std::string& id) { DisablePackage(id); };
    callbacks.unload =
        [this](const std::string& id) { UnloadPackage(id); };
    callbacks.remove =
        [this](const std::string& id) { RemovePackage(id); };
    callbacks.rollback =
        [this](const std::string& id) { RollbackPackage(id); };
    callbacks.revoke_permissions =
        [this](const std::string& id) { RevokePackagePermissions(id); };
    manager_dialog_ =
        std::make_unique<ManagerDialog>(parent, std::move(callbacks));
    manager_dialog_->SetRuntimeSummary(
        "Host plugin loaded in stock OpenCPN.\nPackage store: " +
        storage_root_);
  }
  RefreshManager();
  manager_dialog_->Show();
  manager_dialog_->Raise();
  wxLogMessage("PPM event=manager-shown");
}

void PortablePluginManagerPi::SetManagerStatus(const wxString& status) {
  if (manager_dialog_) manager_dialog_->SetStatus(status);
  wxLogMessage("PPM event=manager-operation status=%s", status);
}

bool PortablePluginManagerPi::PreparePermissions(
    const std::string& package_id, bool interactive,
    wxString* diagnostic) {
  const auto installed = package_store_->Installed();
  const auto item =
      std::find_if(installed.begin(), installed.end(), [&](const auto& value) {
        return value.id == package_id;
      });
  if (item == installed.end()) {
    if (diagnostic) *diagnostic = "Package is not installed.";
    return false;
  }
  const PermissionEvaluation evaluation = permission_store_->Evaluate(*item);
  if (!evaluation.okay) {
    if (diagnostic)
      *diagnostic = wxString::FromUTF8(evaluation.message);
    return false;
  }
  if (!evaluation.added.empty()) {
    if (!interactive) {
      if (diagnostic)
        *diagnostic = "Permission approval is required before this package "
                      "can run.";
      return false;
    }
    if (!ConfirmPermissionApproval(manager_dialog_.get(), *item, evaluation)) {
      if (diagnostic) *diagnostic = "Permission approval was cancelled.";
      return false;
    }
  }
  if (!evaluation.current || !evaluation.manifest_current) {
    const StoreResult granted = permission_store_->Grant(*item);
    if (!granted.okay) {
      if (diagnostic)
        *diagnostic = "Could not save permission approval: " +
                      wxString::FromUTF8(granted.message);
      return false;
    }
  }
  std::string runtime_diagnostic;
  if (!runtime_engine_->SetGrantedPermissions(
          package_id, item->permissions, &runtime_diagnostic)) {
    if (diagnostic)
      *diagnostic = wxString::FromUTF8(runtime_diagnostic);
    return false;
  }
  return true;
}

bool PortablePluginManagerPi::RestorePreviousPackage(
    const std::string& package_id, bool enable_after_restore,
    wxString* diagnostic) {
  const StoreResult rollback = package_store_->Rollback(package_id);
  if (!rollback.okay) {
    if (diagnostic)
      *diagnostic = "Rollback failed: " +
                    wxString::FromUTF8(rollback.message);
    return false;
  }
  const StoreResult audit = package_store_->AuditInstalled(package_id);
  if (!audit.okay) {
    if (diagnostic)
      *diagnostic = "Previous package was restored but failed integrity "
                    "verification: " +
                    wxString::FromUTF8(audit.message);
    return false;
  }
  std::string runtime_diagnostic;
  if (!runtime_engine_->RefreshPackage(package_id, developer_mode_,
                                       &runtime_diagnostic)) {
    if (diagnostic)
      *diagnostic = "Previous package was restored on disk but could not be "
                    "loaded: " +
                    wxString::FromUTF8(runtime_diagnostic);
    return false;
  }
  if (enable_after_restore) {
    wxString permission_diagnostic;
    if (!PreparePermissions(package_id, false, &permission_diagnostic) ||
        !runtime_engine_->Enable(package_id, &runtime_diagnostic) ||
        !package_store_->SetEnabled(package_id, true).okay) {
      if (diagnostic)
        *diagnostic = "Previous package was restored but could not be "
                      "re-enabled: " +
                      permission_diagnostic +
                      wxString::FromUTF8(runtime_diagnostic);
      return false;
    }
  }
  return true;
}

void PortablePluginManagerPi::InstallPackage(
    const std::string& archive_path) {
  if (!package_store_ || !runtime_engine_) return;
  wxBusyCursor busy;
  SetManagerStatus("Verifying package signature, manifest and contents…");
  const StoreResult inspected = package_store_->Inspect(archive_path);
  if (!inspected.okay) {
    SetManagerStatus("Package rejected: " +
                     wxString::FromUTF8(inspected.message));
    return;
  }
  const auto installed = package_store_->Installed();
  const bool replacing =
      std::any_of(installed.begin(), installed.end(), [&](const auto& package) {
        return package.id == inspected.package_id;
      });
  if (replacing &&
      wxMessageBox(
          "A version of " + wxString::FromUTF8(inspected.package_id) +
              " is already installed.\n\nVerify and install this package as "
              "an update? The current version will be retained for rollback.",
          "Update portable package",
          wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, manager_dialog_.get()) !=
          wxYES) {
    SetManagerStatus("Update cancelled; no files were changed.");
    return;
  }

  const bool was_enabled =
      replacing && runtime_engine_->IsEnabled(inspected.package_id);
  if (replacing) {
    package_store_->SetEnabled(inspected.package_id, false);
    std::string ignored;
    runtime_engine_->Unload(inspected.package_id, &ignored);
  }
  const StoreResult result =
      package_store_->Install(archive_path, replacing);
  if (!result.okay) {
    if (replacing) {
      std::string refresh_diagnostic;
      runtime_engine_->RefreshPackage(inspected.package_id, developer_mode_,
                                      &refresh_diagnostic);
      wxString permission_diagnostic;
      if (was_enabled &&
          PreparePermissions(inspected.package_id, false,
                             &permission_diagnostic) &&
          runtime_engine_->Enable(inspected.package_id, &refresh_diagnostic)) {
        package_store_->SetEnabled(inspected.package_id, true);
      }
    }
    SetManagerStatus("Installation failed without replacing the current "
                     "package: " +
                     wxString::FromUTF8(result.message));
    return;
  }

  const StoreResult audit = package_store_->AuditInstalled(result.package_id);
  if (!audit.okay) {
    wxString recovery;
    if (replacing)
      RestorePreviousPackage(result.package_id, was_enabled, &recovery);
    else
      package_store_->Remove(result.package_id);
    SetManagerStatus(
        "Installed package failed the independent on-disk integrity audit: " +
        wxString::FromUTF8(audit.message) +
        (recovery.empty() ? wxString() : "\n" + recovery));
    return;
  }

  std::string runtime_diagnostic;
  if (!runtime_engine_->RefreshPackage(result.package_id, developer_mode_,
                                       &runtime_diagnostic)) {
    wxString recovery;
    if (replacing) {
      RestorePreviousPackage(result.package_id, was_enabled, &recovery);
    } else {
      package_store_->Remove(result.package_id);
      runtime_engine_->RefreshPackage(result.package_id, developer_mode_,
                                      &runtime_diagnostic);
    }
    SetManagerStatus(
        "The package archive was valid, but its runtime could not be loaded: " +
        wxString::FromUTF8(runtime_diagnostic) +
        (recovery.empty() ? wxString() : "\n" + recovery));
    return;
  }

  if (was_enabled) {
    wxString permission_diagnostic;
    if (!PreparePermissions(result.package_id, true,
                            &permission_diagnostic)) {
      SetManagerStatus(
          "Package updated and left disabled: " + permission_diagnostic);
      return;
    }
    if (!runtime_engine_->Enable(result.package_id, &runtime_diagnostic)) {
      wxString recovery;
      RestorePreviousPackage(result.package_id, true, &recovery);
      SetManagerStatus("Updated runtime failed to enable; the previous "
                       "version was restored. " +
                       wxString::FromUTF8(runtime_diagnostic) +
                       (recovery.empty() ? wxString() : "\n" + recovery));
      return;
    }
    const StoreResult persisted =
        package_store_->SetEnabled(result.package_id, true);
    if (!persisted.okay) {
      runtime_engine_->Disable(result.package_id, &runtime_diagnostic);
      SetManagerStatus("Package updated but was left disabled because its "
                       "state could not be saved: " +
                       wxString::FromUTF8(persisted.message));
      return;
    }
  }
  RefreshManager();
  SetManagerStatus(
      wxString::FromUTF8(result.message) +
      (was_enabled ? " and re-enabled." : ". It is disabled by default."));
}

void PortablePluginManagerPi::EnablePackage(const std::string& package_id) {
  const StoreResult audit = package_store_->AuditInstalled(package_id);
  if (!audit.okay) {
    package_store_->SetEnabled(package_id, false);
    SetManagerStatus("Refusing to enable " + wxString::FromUTF8(package_id) +
                     ": installed-package integrity check failed: " +
                     wxString::FromUTF8(audit.message));
    return;
  }
  wxString permission_diagnostic;
  if (!PreparePermissions(package_id, true, &permission_diagnostic)) {
    package_store_->SetEnabled(package_id, false);
    SetManagerStatus("Package remains disabled: " + permission_diagnostic);
    return;
  }
  std::string diagnostic;
  if (!runtime_engine_->Enable(package_id, &diagnostic)) {
    package_store_->SetEnabled(package_id, false);
    SetManagerStatus("Could not enable " + wxString::FromUTF8(package_id) +
                     ": " + wxString::FromUTF8(diagnostic));
    return;
  }
  const StoreResult persisted = package_store_->SetEnabled(package_id, true);
  if (!persisted.okay) {
    runtime_engine_->Disable(package_id, &diagnostic);
    SetManagerStatus("The package started, but was stopped because its "
                     "enabled state could not be saved: " +
                     wxString::FromUTF8(persisted.message));
    return;
  }
  RefreshManager();
  SetManagerStatus(wxString::FromUTF8(package_id) + " enabled.");
}

void PortablePluginManagerPi::DisablePackage(const std::string& package_id) {
  const StoreResult persisted = package_store_->SetEnabled(package_id, false);
  if (!persisted.okay) {
    SetManagerStatus("Could not safely disable package: " +
                     wxString::FromUTF8(persisted.message));
    return;
  }
  std::string diagnostic;
  const bool clean = runtime_engine_->Disable(package_id, &diagnostic);
  RefreshManager();
  SetManagerStatus(
      wxString::FromUTF8(package_id) +
      (clean ? " disabled; its runtime remains resident for quick restart."
             : " was forcibly stopped after its disable callback failed: " +
                   wxString::FromUTF8(diagnostic)));
}

void PortablePluginManagerPi::UnloadPackage(const std::string& package_id) {
  const StoreResult persisted = package_store_->SetEnabled(package_id, false);
  if (!persisted.okay) {
    SetManagerStatus("Could not safely unload package: " +
                     wxString::FromUTF8(persisted.message));
    return;
  }
  std::string diagnostic;
  const bool clean = runtime_engine_->Unload(package_id, &diagnostic);
  RefreshManager();
  SetManagerStatus(
      wxString::FromUTF8(package_id) +
      (clean ? " unloaded; its Wasmtime memory has been released."
             : " was forcibly unloaded after an error: " +
                   wxString::FromUTF8(diagnostic)));
}

void PortablePluginManagerPi::RemovePackage(const std::string& package_id) {
  package_store_->SetEnabled(package_id, false);
  std::string ignored;
  runtime_engine_->Unload(package_id, &ignored);
  const StoreResult removed = package_store_->Remove(package_id);
  std::string refresh_diagnostic;
  runtime_engine_->RefreshPackage(package_id, developer_mode_,
                                  &refresh_diagnostic);
  RefreshManager();
  SetManagerStatus(
      removed.okay
          ? wxString::FromUTF8(package_id) +
                " removed to recoverable storage; private data was retained."
          : "Removal failed: " + wxString::FromUTF8(removed.message));
}

void PortablePluginManagerPi::RollbackPackage(
    const std::string& package_id) {
  package_store_->SetEnabled(package_id, false);
  std::string ignored;
  runtime_engine_->Unload(package_id, &ignored);
  const StoreResult rolled_back = package_store_->Rollback(package_id);
  std::string runtime_diagnostic;
  const StoreResult audit =
      rolled_back.okay ? package_store_->AuditInstalled(package_id)
                       : StoreResult{};
  const bool loaded =
      rolled_back.okay && audit.okay &&
      runtime_engine_->RefreshPackage(package_id, developer_mode_,
                                      &runtime_diagnostic);
  RefreshManager();
  SetManagerStatus(
      loaded ? wxString::FromUTF8(package_id) +
                   " rolled back and left disabled for review."
             : "Rollback failed: " +
                   wxString::FromUTF8(
                       !rolled_back.okay
                           ? rolled_back.message
                           : !audit.okay ? audit.message
                                         : runtime_diagnostic));
}

void PortablePluginManagerPi::RevokePackagePermissions(
    const std::string& package_id) {
  package_store_->SetEnabled(package_id, false);
  std::string ignored;
  runtime_engine_->Unload(package_id, &ignored);
  const StoreResult revoked = permission_store_->Revoke(package_id);
  RefreshManager();
  SetManagerStatus(
      revoked.okay
          ? wxString::FromUTF8(package_id) +
                " disabled and unloaded; all stored access approval was "
                "revoked."
          : "Could not revoke package access: " +
                wxString::FromUTF8(revoked.message));
}

void PortablePluginManagerPi::SetPositionFixEx(PlugIn_Position_Fix_Ex& fix) {
  if (runtime_engine_) runtime_engine_->SetPositionFix(fix);
}

bool PortablePluginManagerPi::RenderOverlayMultiCanvas(
    wxDC& dc, PlugIn_ViewPort* viewport, int, int priority) {
  if (!runtime_engine_ || !viewport || priority != 0) return false;
  bool rendered = false;
  for (const auto& scene : runtime_engine_->Scenes()) {
    if (scene.points.size() < 2) continue;
    dc.SetPen(wxPen(wxColour(scene.red, scene.green, scene.blue, scene.alpha),
                    std::max(1, static_cast<int>(scene.width_pixels))));
    wxPoint previous;
    GetCanvasPixLL(viewport, &previous, scene.points.front().latitude,
                   scene.points.front().longitude);
    for (std::size_t index = 1; index < scene.points.size(); ++index) {
      wxPoint next;
      GetCanvasPixLL(viewport, &next, scene.points[index].latitude,
                     scene.points[index].longitude);
      dc.DrawLine(previous, next);
      previous = next;
    }
    rendered = true;
  }
  return rendered;
}

bool PortablePluginManagerPi::RenderGLOverlayMultiCanvas(
    wxGLContext*, PlugIn_ViewPort* viewport, int, int priority) {
  if (!runtime_engine_ || !viewport || priority != 0) return false;
  bool rendered = false;
  for (const auto& scene : runtime_engine_->Scenes()) {
    if (scene.points.size() < 2) continue;
    glPushAttrib(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT | GL_LINE_BIT);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4ub(scene.red, scene.green, scene.blue, scene.alpha);
    glLineWidth(scene.width_pixels);
    glBegin(GL_LINE_STRIP);
    for (const auto& point : scene.points) {
      wxPoint pixel;
      GetCanvasPixLL(viewport, &pixel, point.latitude, point.longitude);
      glVertex2i(pixel.x, viewport->pix_height - pixel.y);
    }
    glEnd();
    glPopAttrib();
    rendered = true;
  }
  return rendered;
}

void PortablePluginManagerPi::OnEngineStateChanged() {
  if (package_store_ && runtime_engine_) {
    for (const auto& package : runtime_engine_->Packages()) {
      if (package.state == "Failed")
        package_store_->SetEnabled(package.id, false);
    }
  }
  RefreshManager();
  RequestRefresh(GetOCPNCanvasWindow());
}

void PortablePluginManagerPi::RefreshManager() {
  if (manager_dialog_ && runtime_engine_) {
    auto packages = runtime_engine_->Packages();
    const auto installed = package_store_->Installed();
    for (auto& package : packages) {
      const auto item = std::find_if(
          installed.begin(), installed.end(),
          [&](const auto& value) { return value.id == package.id; });
      if (item == installed.end()) {
        package.access = "Unknown";
        continue;
      }
      const PermissionEvaluation access = permission_store_->Evaluate(*item);
      package.access =
          !access.okay
              ? "Blocked"
              : access.current
                    ? "Approved"
                    : access.added.empty() ? "Access reduced"
                                           : "Approval required";
    }
    manager_dialog_->SetPackages(packages);
    const auto failures =
        std::count_if(packages.begin(), packages.end(), [](const auto& value) {
          return value.state == "Failed";
        });
    manager_dialog_->SetStatus(failures == 0
                                   ? wxString::Format(
                                         "%zu installed package%s.",
                                         packages.size(),
                                         packages.size() == 1 ? "" : "s")
                                   : wxString::Format(
                                         "%zu installed package%s; %zu "
                                         "requires attention.",
                                         packages.size(),
                                         packages.size() == 1 ? "" : "s",
                                         failures));
  }
}

}  // namespace ppm
