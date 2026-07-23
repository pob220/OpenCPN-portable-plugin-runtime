#include "portable_plugin_manager_pi.h"

#include <wx/filename.h>
#include <wx/log.h>

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
    "32 32 5 1",
    "  c None",
    ". c #19324A",
    "+ c #2F80ED",
    "@ c #A7D5FF",
    "# c #FFFFFF",
    "                                ",
    "          ............          ",
    "       ....++++++++++....       ",
    "      ..++++++++++++++++..      ",
    "     ..+++++@@@@@@@@+++++..     ",
    "    ..++++@@@@@@@@@@@@++++..    ",
    "    .++++@@@########@@@++++.    ",
    "   ..+++@@############@@+++..   ",
    "   .+++@@###........###@@+++.   ",
    "  ..+++@###..++++++..###@+++..  ",
    "  .+++@@##..++++++++..##@@+++.  ",
    "  .+++@##..+++@@@@+++..##@+++.  ",
    "  .++@@##.+++@@##@@+++.##@@++.  ",
    " ..++@@#..++@@####@@++..#@@++.. ",
    " .+++@##.+++@##..##@+++.##@+++. ",
    " .+++@##.+++@#.++.#@+++.##@+++. ",
    " .+++@##.+++@#.++.#@+++.##@+++. ",
    " .+++@##.+++@##..##@+++.##@+++. ",
    " ..++@@#..++@@####@@++..#@@++.. ",
    "  .++@@##.+++@@##@@+++.##@@++.  ",
    "  .+++@##..+++@@@@+++..##@+++.  ",
    "  .+++@@##..++++++++..##@@+++.  ",
    "  ..+++@###..++++++..###@+++..  ",
    "   .+++@@###........###@@+++.   ",
    "   ..+++@@############@@+++..   ",
    "    .++++@@@########@@@++++.    ",
    "    ..++++@@@@@@@@@@@@++++..    ",
    "     ..+++++@@@@@@@@+++++..     ",
    "      ..++++++++++++++++..      ",
    "       ....++++++++++....       ",
    "          ............          ",
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
    return WANTS_TOOLBAR_CALLBACK | INSTALLS_TOOLBAR_TOOL | WANTS_CONFIG;
  }
  initialized_ = true;
  storage_root_ = ResolveStorageRoot();
  wxLogMessage("PPM event=init api=1.21 version=0.1.0");
  wxLogMessage("PPM event=storage-root path=%s", storage_root_);
  if (!RegisterManagerAction()) {
    wxLogError("PPM event=manager-action-registration-failed");
  }
  return WANTS_TOOLBAR_CALLBACK | INSTALLS_TOOLBAR_TOOL | WANTS_CONFIG;
}

bool PortablePluginManagerPi::DeInit() {
  if (!initialized_) return true;
  if (manager_dialog_) {
    manager_dialog_->Hide();
    manager_dialog_->Destroy();
    manager_dialog_.release();
  }
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
  const int tool_id = InsertPlugInTool(
      "Portable Plugin Manager", &plugin_bitmap_, &plugin_bitmap_,
      wxITEM_NORMAL, "Portable Plugin Manager",
      "Install and manage portable runtime plugins", nullptr, -1, 0, this);
  if (!actions_.Add(kManagerAction, tool_id)) {
    if (tool_id >= 0) RemovePlugInTool(tool_id);
    return false;
  }
  wxLogMessage("PPM event=action-registered package=%s action=%s tool=%d",
               kManagerAction.package_id, kManagerAction.action_id, tool_id);
  return true;
}

void PortablePluginManagerPi::RemoveAllActions() {
  for (const auto& action : actions_.Clear()) {
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
  if (action->key == kManagerAction) ShowManager(nullptr);
}

void PortablePluginManagerPi::ShowPreferencesDialog(wxWindow* parent) {
  ShowManager(parent);
}

void PortablePluginManagerPi::ShowManager(wxWindow* parent) {
  if (!manager_dialog_) {
    manager_dialog_ = std::make_unique<ManagerDialog>(parent);
    manager_dialog_->SetRuntimeSummary(
        "Host plugin loaded in stock OpenCPN — 0 packages installed.\n"
        "Package store: " +
        storage_root_);
  }
  manager_dialog_->Show();
  manager_dialog_->Raise();
  wxLogMessage("PPM event=manager-shown");
}

}  // namespace ppm
