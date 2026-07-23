#include "portable_plugin_manager_pi.h"

#include <wx/filename.h>
#include <wx/log.h>

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
    return WANTS_TOOLBAR_CALLBACK | INSTALLS_TOOLBAR_TOOL | WANTS_CONFIG |
           WANTS_NMEA_EVENTS | WANTS_OVERLAY_CALLBACK |
           WANTS_OPENGL_OVERLAY_CALLBACK;
  }
  initialized_ = true;
  storage_root_ = ResolveStorageRoot();
  wxLogMessage("PPM event=init api=1.21 version=0.1.0");
  wxLogMessage("PPM event=storage-root path=%s", storage_root_);
  if (!RegisterManagerAction()) {
    wxLogError("PPM event=manager-action-registration-failed");
  }
  runtime_engine_ = std::make_unique<RuntimeEngine>(
      storage_root_.ToStdString(),
      [this](const RuntimeAction& action, std::uint32_t* host_action_id) {
        return RegisterPortableAction(action, host_action_id);
      },
      [this]() { OnEngineStateChanged(); });
  wxString developer;
  const bool developer_mode =
      wxGetEnv("OCPN_PPM_DEVELOPER_MODE", &developer) && developer == "1";
  if (!runtime_engine_->LoadInstalled(developer_mode)) {
    wxLogError("PPM event=runtime-engine-load-failed");
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
    manager_dialog_ = std::make_unique<ManagerDialog>(parent);
    manager_dialog_->SetRuntimeSummary(
        "Host plugin loaded in stock OpenCPN.\nPackage store: " +
        storage_root_);
  }
  RefreshManager();
  manager_dialog_->Show();
  manager_dialog_->Raise();
  wxLogMessage("PPM event=manager-shown");
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
  RefreshManager();
  RequestRefresh(GetOCPNCanvasWindow());
}

void PortablePluginManagerPi::RefreshManager() {
  if (manager_dialog_ && runtime_engine_) {
    const auto packages = runtime_engine_->Packages();
    manager_dialog_->SetPackages(packages);
    manager_dialog_->SetStatus(
        wxString::Format("%zu installed package%s.", packages.size(),
                         packages.size() == 1 ? "" : "s"));
  }
}

}  // namespace ppm
