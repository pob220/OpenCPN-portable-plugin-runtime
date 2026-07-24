#include "portable_plugin_manager_pi.h"

#include <wx/app.h>
#include <wx/dcmemory.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/utils.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

#if defined(__WXOSX__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "manager_dialog.h"
#include "environment_workbench.h"
#include "surface_dialog.h"
#include "weather_routing_host.h"
#include "window_activation.h"

#ifndef DECL_EXP
#ifdef __WXMSW__
#define DECL_EXP __declspec(dllexport)
#else
#define DECL_EXP __attribute__((visibility("default")))
#endif
#endif

namespace {

const char* const kManagerXpm[] = {"32 32 6 1",
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
  const wxString installed = GetPluginDataDir("portable_plugin_manager_pi");
  if (!installed.empty() &&
      wxFileName::FileExists(installed + wxFileName::GetPathSeparator() +
                             "manager.svg")) {
    return installed;
  }
  return wxString::FromUTF8(PPM_SOURCE_DATA_DIR);
}

wxString ResolveManagerIcon() {
  const wxString separator = wxFileName::GetPathSeparator();
  const wxString icon = ResolveManagerDataRoot() + separator + "manager.svg";
  return wxFileName::FileExists(icon) ? icon : wxString();
}

/**
 * Render the wxDC-based environmental workbench into the current desktop
 * OpenGL canvas without using any OpenCPN-private drawing classes.
 *
 * Stock desktop plugins (including grib_pi) use glDrawPixels for this
 * compatibility path.  A keyed background is used as a defensive fallback
 * because wxGTK backends differ in how reliably they preserve alpha while
 * drawing into a 32-bit wxBitmap.  Pixels whose alpha survives retain it;
 * drawn pixels with a zero alpha are promoted to opaque.
 */
bool RenderEnvironmentWithDesktopGl(PortableEnvironmentHost* workbench,
                                    PlugIn_ViewPort* viewport) {
  if (!workbench || !viewport || viewport->pix_width <= 0 ||
      viewport->pix_height <= 0)
    return false;

  // This compatibility path holds one wxBitmap, one wxImage and one RGBA
  // upload.  Keep its worst-case transient allocation reasonable on modest
  // navigation computers while still covering a 5K display.
  constexpr int kMaximumCanvasDimension = 8'192;
  constexpr std::uint64_t kMaximumCanvasPixels = 16'777'216;
  const std::uint64_t width =
      static_cast<std::uint64_t>(viewport->pix_width);
  const std::uint64_t height =
      static_cast<std::uint64_t>(viewport->pix_height);
  if (viewport->pix_width > kMaximumCanvasDimension ||
      viewport->pix_height > kMaximumCanvasDimension ||
      width * height > kMaximumCanvasPixels) {
    wxLogWarning(
        "PPM iGRIB OpenGL overlay skipped: canvas %dx%d exceeds the "
        "bounded compatibility surface",
        viewport->pix_width, viewport->pix_height);
    return false;
  }

  // This deliberately unusual colour is only a transparency key.  The
  // environmental palette never emits it, and exact equality prevents nearby
  // anti-aliased colours from being discarded.
  constexpr unsigned char kKeyRed = 1;
  constexpr unsigned char kKeyGreen = 2;
  constexpr unsigned char kKeyBlue = 3;
  wxBitmap bitmap(viewport->pix_width, viewport->pix_height, 32);
  if (!bitmap.IsOk()) return false;
  bitmap.UseAlpha();
  wxMemoryDC memory_dc;
  memory_dc.SelectObject(bitmap);
  memory_dc.SetBackground(
      wxBrush(wxColour(kKeyRed, kKeyGreen, kKeyBlue, 255)));
  memory_dc.Clear();
  const bool rendered = workbench->Render(memory_dc, viewport);
  memory_dc.SelectObject(wxNullBitmap);
  if (!rendered) return false;

  wxImage image = bitmap.ConvertToImage();
  if (!image.IsOk() || !image.GetData()) return false;
  const unsigned char* rgb = image.GetData();
  const unsigned char* source_alpha =
      image.HasAlpha() ? image.GetAlpha() : nullptr;
  std::vector<unsigned char> rgba;
  try {
    rgba.resize(width * height * 4);
  } catch (const std::bad_alloc&) {
    wxLogWarning(
        "PPM iGRIB OpenGL overlay skipped: could not allocate the bounded "
        "RGBA compatibility surface");
    return false;
  }
  for (std::uint64_t index = 0; index < width * height; ++index) {
    const unsigned char red = rgb[index * 3];
    const unsigned char green = rgb[index * 3 + 1];
    const unsigned char blue = rgb[index * 3 + 2];
    const bool background =
        red == kKeyRed && green == kKeyGreen && blue == kKeyBlue;
    rgba[index * 4] = red;
    rgba[index * 4 + 1] = green;
    rgba[index * 4 + 2] = blue;
    if (background) {
      rgba[index * 4 + 3] = 0;
    } else {
      const unsigned char alpha = source_alpha ? source_alpha[index] : 255;
      rgba[index * 4 + 3] = alpha == 0 ? 255 : alpha;
    }
  }

  glPushAttrib(GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_ENABLE_BIT |
               GL_PIXEL_MODE_BIT);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
  GLint previous_unpack_alignment = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  // OpenCPN's desktop overlay projection is top-left based.  The negative
  // pixel zoom is the same orientation used by stock grib_pi.
  glRasterPos2i(0, 0);
  glPixelZoom(1.0F, -1.0F);
  glDrawPixels(viewport->pix_width, viewport->pix_height, GL_RGBA,
               GL_UNSIGNED_BYTE, rgba.data());
  glPixelZoom(1.0F, 1.0F);
  glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
  glPopAttrib();
  return true;
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
           WANTS_NMEA_EVENTS | WANTS_NMEA_SENTENCES | WANTS_OVERLAY_CALLBACK |
           WANTS_OPENGL_OVERLAY_CALLBACK | WANTS_CURSOR_LATLON;
  }
  initialized_ = true;
  storage_root_ = ResolveStorageRoot();
  wxLogMessage("PPM event=init api=1.21 version=0.2.1");
  wxLogMessage("PPM event=storage-root path=%s", storage_root_);
  if (!RegisterManagerAction()) {
    wxLogError("PPM event=manager-action-registration-failed");
  }
  wxString developer;
  developer_mode_ =
      wxGetEnv("OCPN_PPM_DEVELOPER_MODE", &developer) && developer == "1";
  const wxString trust_root =
      ResolveManagerDataRoot() + wxFileName::GetPathSeparator() + "trust";
  package_store_ = std::make_unique<PackageStore>(storage_root_.ToStdString(),
                                                  trust_root.ToStdString());
  package_store_->SetDeveloperMode(developer_mode_);
  permission_store_ =
      std::make_unique<PermissionStore>(storage_root_.ToStdString());
  for (const auto& package : package_store_->Installed()) {
    if (!package.enabled) continue;
    const StoreResult audit = package_store_->AuditInstalled(package.id);
    if (!audit.okay) {
      package_store_->SetEnabled(package.id, false);
      wxLogError(
          "PPM event=installed-integrity-failed package=%s "
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
      [this]() { OnEngineStateChanged(); },
      [gate = (ui_callback_gate_ = std::make_shared<std::atomic_bool>(true))](
          std::function<void()> task) {
        if (!wxTheApp || !task) return;
        wxTheApp->CallAfter([gate, task = std::move(task)]() mutable {
          if (gate->load()) task();
        });
      });
  runtime_engine_->SetSurfaceOpenedCallback(
      [this](const std::string& package_id, const DeclarativeSurface& surface) {
        OpenPackageSurface(package_id, surface);
      });
  runtime_engine_->SetSurfaceResponseCallback(
      [this](const std::string& package_id, const std::string& surface_id,
             const std::string& control_id, const std::string& state_json,
             const std::string& diagnostic) {
        ApplySurfaceResponse(package_id, surface_id, control_id, state_json,
                             diagnostic);
      });
  runtime_engine_->SetRoutingProgressCallback(
      [this](const std::string& package_id, std::uint8_t percent,
             const std::string& message) {
        if (package_id == "org.opencpn.iweather-routing" &&
            weather_routing_host_) {
          weather_routing_host_->ReportProgress(
              percent, wxString::FromUTF8(message));
        }
      });
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
      wxLogWarning(
          "PPM event=startup-package-disabled package=%s "
          "diagnostic=%s%s",
          package.id, diagnostic, wxString::FromUTF8(runtime_diagnostic));
    }
  }
  if (developer_mode_) {
    wxString startup_action;
    if (wxGetEnv("OCPN_PPM_DEVELOPER_STARTUP_ACTION", &startup_action) &&
        !startup_action.empty()) {
      const int separator = startup_action.Find(':');
      if (separator > 0 && separator + 1 < static_cast<int>(startup_action.size())) {
        const std::string package_id =
            startup_action.Left(separator).ToStdString();
        const std::string action_id =
            startup_action.Mid(separator + 1).ToStdString();
        wxTheApp->CallAfter([this, package_id, action_id]() {
          if (!initialized_ || !runtime_engine_ ||
              !runtime_engine_->HandleAction(package_id, action_id)) {
            wxLogWarning(
                "PPM developer startup action failed package=%s action=%s",
                package_id, action_id);
          } else {
            wxLogMessage(
                "PPM developer startup action invoked package=%s action=%s",
                package_id, action_id);
          }
        });
      } else {
        wxLogWarning(
            "PPM ignored malformed OCPN_PPM_DEVELOPER_STARTUP_ACTION");
      }
    }
  }
  return WANTS_TOOLBAR_CALLBACK | INSTALLS_TOOLBAR_TOOL | WANTS_CONFIG |
         WANTS_NMEA_EVENTS | WANTS_NMEA_SENTENCES | WANTS_OVERLAY_CALLBACK |
         WANTS_OPENGL_OVERLAY_CALLBACK | WANTS_CURSOR_LATLON;
}

bool PortablePluginManagerPi::DeInit() {
  if (!initialized_) return true;
  if (manager_dialog_) {
    manager_dialog_->Hide();
    manager_dialog_->Destroy();
    manager_dialog_.release();
  }
  if (weather_routing_host_) {
    weather_routing_host_->Shutdown();
    weather_routing_host_.reset();
  }
  if (environment_workbench_) {
    environment_workbench_->Shutdown();
    environment_workbench_.reset();
  }
  surface_dialogs_.clear();
  if (runtime_engine_) {
    runtime_engine_->Shutdown();
    if (ui_callback_gate_) ui_callback_gate_->store(false);
    runtime_engine_.reset();
  }
  ui_callback_gate_.reset();
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
          ? InsertPlugInTool("Portable Plugin Manager", &plugin_bitmap_,
                             &plugin_bitmap_, wxITEM_NORMAL,
                             "Portable Plugin Manager",
                             "Install and manage portable runtime plugins",
                             nullptr, -1, 0, this)
          : InsertPlugInToolSVG("Portable Plugin Manager", icon, icon, icon,
                                wxITEM_NORMAL, "Portable Plugin Manager",
                                "Install and manage portable runtime plugins",
                                nullptr, -1, 0, this);
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
    tool_id =
        InsertPlugInTool(label, &plugin_bitmap_, &plugin_bitmap_, wxITEM_NORMAL,
                         tooltip, tooltip, nullptr, -1, 0, this);
  }
  if (!actions_.Add(key, tool_id)) {
    if (tool_id >= 0) RemovePlugInTool(tool_id);
    return -2;
  }
  SetToolbarToolViz(tool_id, true);
  *host_action_id = static_cast<std::uint32_t>(tool_id);
  wxLogMessage(
      "PPM event=package-action-registered package=%s action=%s tool=%d",
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
    callbacks.install = [this](const std::string& path) {
      InstallPackage(path);
    };
    callbacks.enable = [this](const std::string& id) { EnablePackage(id); };
    callbacks.disable = [this](const std::string& id) { DisablePackage(id); };
    callbacks.unload = [this](const std::string& id) { UnloadPackage(id); };
    callbacks.remove = [this](const std::string& id) { RemovePackage(id); };
    callbacks.rollback = [this](const std::string& id) { RollbackPackage(id); };
    callbacks.revoke_permissions = [this](const std::string& id) {
      RevokePackagePermissions(id);
    };
    manager_dialog_ = std::make_unique<ManagerDialog>(
        ResolveOpenCpnTopLevelParent(parent), std::move(callbacks));
    manager_dialog_->SetRuntimeSummary(
        "Host plugin loaded in stock OpenCPN.\nPackage store: " +
        storage_root_);
  }
  RefreshManager();
  ShowAndActivateWindow(manager_dialog_.get());
  wxLogMessage("PPM event=manager-shown");
}

void PortablePluginManagerPi::SetManagerStatus(const wxString& status) {
  if (manager_dialog_) manager_dialog_->SetStatus(status);
  wxLogMessage("PPM event=manager-operation status=%s", status);
}

void PortablePluginManagerPi::OpenPackageSurface(
    const std::string& package_id, const DeclarativeSurface& surface) {
  if (package_id == "org.opencpn.igrib" &&
      surface.id == "environment.viewer") {
    if (!environment_workbench_) {
      const wxString separator = wxFileName::GetPathSeparator();
      const wxString package_root =
          storage_root_ + separator + "packages" + separator +
          wxString::FromUTF8(package_id);
      bool credential_access = false;
      if (package_store_) {
        for (const auto& package : package_store_->Installed()) {
          if (package.id == package_id) {
            credential_access =
                std::find(package.permissions.begin(),
                          package.permissions.end(),
                          "credentials.provider") != package.permissions.end();
            break;
          }
        }
      }
      environment_workbench_ = std::make_unique<PortableEnvironmentHost>(
          ResolveOpenCpnTopLevelParent(), wxString::FromUTF8(package_id),
          package_root,
          "ui/igrib-viewer.ui.json", credential_access,
          [this, package_id](const wxString& control_id,
                             const wxString& value_json, wxString* error,
                             wxString* accepted_state) {
            if (accepted_state) accepted_state->clear();
            const wxScopedCharBuffer control = control_id.utf8_str();
            const wxScopedCharBuffer value = value_json.utf8_str();
            if (!control || !value || !runtime_engine_ ||
                !runtime_engine_->HandleSurfaceEvent(
                    package_id, "environment.viewer",
                    std::string(control.data(), control.length()),
                    std::string(value.data(), value.length()))) {
              if (error)
                *error =
                    "portable environmental controller is unavailable";
              return false;
            }
            return true;
          },
          [this, package_id](const wxString& path) {
            const wxScopedCharBuffer value = path.utf8_str();
            if (!value || !runtime_engine_ ||
                !runtime_engine_->SelectEnvironmentDataset(
                    package_id,
                    {std::string(value.data(), value.length())})) {
              SetManagerStatus(
                  "iGRIB opened the dataset, but the typed routing provider "
                  "could not adopt it.");
            }
          },
          [this](double* west, double* south, double* east, double* north) {
            if (!view_bounds_valid_ || !west || !south || !east || !north)
              return false;
            *west = view_west_;
            *south = view_south_;
            *east = view_east_;
            *north = view_north_;
            return true;
          },
          []() { RequestRefresh(GetOCPNCanvasWindow()); },
          [this]() {
            std::vector<PortableEnvironmentPosition> result;
            for (const auto& waypoint : ListWaypoints()) {
              result.push_back({waypoint.id, waypoint.name, waypoint.latitude,
                                waypoint.longitude});
            }
            return result;
          },
          [this](PortableEnvironmentPosition* position) {
            if (!position || !vessel_position_valid_) return false;
            *position = {"opencpn:vessel", "Current boat position",
                         vessel_latitude_, vessel_longitude_};
            return true;
          });
    }
    wxString error;
    if (!environment_workbench_->Show(&error)) {
      SetManagerStatus("Could not open iGRIB: " + error);
      return;
    }
    if (developer_mode_ && !developer_smoke_fixture_opened_) {
      wxString fixture;
      if (wxGetEnv("OCPN_PPM_IGRIB_SMOKE_FIXTURE", &fixture) &&
          !fixture.empty()) {
        developer_smoke_fixture_opened_ = true;
        if (!environment_workbench_->OpenDataset({fixture}, &error)) {
          SetManagerStatus("Could not open the iGRIB smoke fixture: " + error);
        } else {
          wxLogMessage("PPM iGRIB developer smoke fixture requested: %s",
                       fixture);
        }
      }
    }
    return;
  }

  if (package_id == "org.opencpn.iweather-routing" &&
      surface.id == "routing.workbench") {
    if (!weather_routing_host_) {
      const wxString separator = wxFileName::GetPathSeparator();
      const wxString package_root =
          storage_root_ + separator + "packages" + separator +
          wxString::FromUTF8(package_id);
      weather_routing_host_ = std::make_unique<PortableWeatherRoutingHost>(
          ResolveOpenCpnTopLevelParent(), GetOCPNConfigObject(),
          [this, package_id](RoutingRequest request, RoutingOutcome* outcome,
                             std::string* diagnostic) {
            return runtime_engine_ &&
                   runtime_engine_->CalculateRouteBlocking(
                       package_id, std::move(request), outcome, diagnostic);
          },
          [this, package_id]() {
            if (runtime_engine_) runtime_engine_->CancelRoute(package_id);
          },
          package_root, wxString::FromUTF8(package_id),
          "ui/iweather-routing.ui.json",
          [this]() {
            return runtime_engine_
                       ? wxString::FromUTF8(runtime_engine_->EnvironmentSummary(
                             "org.opencpn.igrib"))
                       : wxString("Environmental provider unavailable");
          },
          [this]() { return ListWaypoints(); },
          [this]() { return ListRoutes(); },
          [this](const wxString& name,
                 const std::vector<PortableNavigationPosition>& points,
                 wxString* diagnostic) {
            return CreateOpenCpnRoute(name, points, diagnostic);
          },
          [this](PortableNavigationPosition* position) {
            if (!position || !vessel_position_valid_) return false;
            *position = {"vessel", "Vessel position", vessel_latitude_,
                         vessel_longitude_};
            return true;
          },
          [this](PortableNavigationPosition* position) {
            if (!position || !cursor_position_valid_) return false;
            *position = {"cursor", "Chart cursor", cursor_latitude_,
                         cursor_longitude_};
            return true;
          },
          [](std::int64_t*) { return false; },
          [this, package_id](
              double latitude, double longitude,
              const std::vector<std::int64_t>& unix_times,
              std::vector<std::uint8_t>* availability, wxString* diagnostic) {
            std::string error;
            const bool okay =
                runtime_engine_ &&
                runtime_engine_->PreflightEnvironment(
                    package_id, latitude, longitude, unix_times, availability,
                    &error);
            if (!okay && diagnostic) *diagnostic = wxString::FromUTF8(error);
            return okay;
          },
          vessel_position_valid_ ? vessel_latitude_ : 0.0,
          vessel_position_valid_ ? vessel_longitude_ : 0.0);
    }
    wxString error;
    if (!weather_routing_host_->Show(&error))
      SetManagerStatus("Could not open iWeatherRouting: " + error);
    return;
  }

  const std::string key = package_id + "\n" + surface.id;
  const auto existing = surface_dialogs_.find(key);
  if (existing != surface_dialogs_.end()) {
    ShowAndActivateWindow(existing->second.get());
    return;
  }
  auto dialog = std::make_unique<SurfaceDialog>(
      ResolveOpenCpnTopLevelParent(), surface,
      [this, package_id, surface_id = surface.id](
          const std::string& control_id, const std::string& value_json,
          const std::vector<SurfaceDialog::UserFileSelection>& selections) {
        std::string delivered_value = value_json;
        if (!selections.empty()) {
          if (package_id == "org.opencpn.igrib" &&
              surface_id == "environment.viewer" && control_id == "open") {
            std::vector<std::string> paths;
            paths.reserve(selections.size());
            for (const auto& selection : selections) {
              paths.push_back(selection.path);
            }
            if (!runtime_engine_ ||
                !runtime_engine_->SelectEnvironmentDataset(package_id, paths)) {
              ApplySurfaceResponse(
                  package_id, surface_id, control_id, {},
                  "Environmental provider is disabled or unavailable.");
              return;
            }
            // The component receives only an opaque host-state marker. GRIB
            // paths and bytes remain in the host-owned provider boundary.
            delivered_value = "\"host-environment-snapshot\"";
          } else {
            std::vector<std::string> tokens;
            tokens.reserve(selections.size());
            for (const auto& selection : selections) {
              std::string token;
              std::string diagnostic;
              if (!runtime_engine_ ||
                  !runtime_engine_->RegisterUserFileGrant(
                      package_id, selection.path, selection.writable, &token,
                      &diagnostic)) {
                ApplySurfaceResponse(package_id, surface_id, control_id, {},
                                     diagnostic.empty()
                                         ? "Could not grant access to the "
                                           "selected file."
                                         : diagnostic);
                return;
              }
              tokens.push_back(std::move(token));
            }
            if (tokens.size() == 1) {
              delivered_value = "\"" + tokens.front() + "\"";
            } else {
              delivered_value = "[";
              for (std::size_t index = 0; index < tokens.size(); ++index) {
                if (index != 0) delivered_value += ",";
                delivered_value += "\"" + tokens[index] + "\"";
              }
              delivered_value += "]";
            }
          }
        }
        if (!runtime_engine_ ||
            !runtime_engine_->HandleSurfaceEvent(package_id, surface_id,
                                                 control_id, delivered_value)) {
          ApplySurfaceResponse(package_id, surface_id, control_id, {},
                               "Package is disabled, busy, or unavailable.");
        }
      });
  ShowAndActivateWindow(dialog.get());
  surface_dialogs_.emplace(key, std::move(dialog));
  if (runtime_engine_)
    runtime_engine_->HandleSurfaceEvent(package_id, surface.id,
                                        "surface-opened", "null");
  wxLogMessage("PPM event=surface-opened package=%s surface=%s", package_id,
               surface.id);
}

void PortablePluginManagerPi::ApplySurfaceResponse(
    const std::string& package_id, const std::string& surface_id,
    const std::string& control_id, const std::string& state_json,
    const std::string& diagnostic) {
  const auto item = surface_dialogs_.find(package_id + "\n" + surface_id);
  if (item == surface_dialogs_.end()) return;
  item->second->ApplyResponse(control_id, state_json, diagnostic);
}

void PortablePluginManagerPi::ClosePackageSurfaces(
    const std::string& package_id) {
  if (package_id == "org.opencpn.igrib" && environment_workbench_) {
    environment_workbench_->Shutdown();
    environment_workbench_.reset();
  }
  if (package_id == "org.opencpn.iweather-routing" &&
      weather_routing_host_) {
    weather_routing_host_->Shutdown();
    weather_routing_host_.reset();
  }
  const std::string prefix = package_id + "\n";
  for (auto item = surface_dialogs_.begin(); item != surface_dialogs_.end();) {
    if (item->first.rfind(prefix, 0) == 0)
      item = surface_dialogs_.erase(item);
    else
      ++item;
  }
}

bool PortablePluginManagerPi::PreparePermissions(const std::string& package_id,
                                                 bool interactive,
                                                 wxString* diagnostic) {
  const auto installed = package_store_->Installed();
  const auto item =
      std::find_if(installed.begin(), installed.end(),
                   [&](const auto& value) { return value.id == package_id; });
  if (item == installed.end()) {
    if (diagnostic) *diagnostic = "Package is not installed.";
    return false;
  }
  const PermissionEvaluation evaluation = permission_store_->Evaluate(*item);
  if (!evaluation.okay) {
    if (diagnostic) *diagnostic = wxString::FromUTF8(evaluation.message);
    return false;
  }
  if (!evaluation.added.empty()) {
    if (!interactive) {
      if (diagnostic)
        *diagnostic =
            "Permission approval is required before this package "
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
  if (!runtime_engine_->SetGrantedPermissions(package_id, item->permissions,
                                              &runtime_diagnostic)) {
    if (diagnostic) *diagnostic = wxString::FromUTF8(runtime_diagnostic);
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
      *diagnostic = "Rollback failed: " + wxString::FromUTF8(rollback.message);
    return false;
  }
  const StoreResult audit = package_store_->AuditInstalled(package_id);
  if (!audit.okay) {
    if (diagnostic)
      *diagnostic =
          "Previous package was restored but failed integrity "
          "verification: " +
          wxString::FromUTF8(audit.message);
    return false;
  }
  std::string runtime_diagnostic;
  if (!runtime_engine_->RefreshPackage(package_id, developer_mode_,
                                       &runtime_diagnostic)) {
    if (diagnostic)
      *diagnostic =
          "Previous package was restored on disk but could not be "
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
        *diagnostic =
            "Previous package was restored but could not be "
            "re-enabled: " +
            permission_diagnostic + wxString::FromUTF8(runtime_diagnostic);
      return false;
    }
  }
  return true;
}

void PortablePluginManagerPi::InstallPackage(const std::string& archive_path) {
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
  const bool replacing = std::any_of(
      installed.begin(), installed.end(),
      [&](const auto& package) { return package.id == inspected.package_id; });
  if (replacing &&
      wxMessageBox(
          "A version of " + wxString::FromUTF8(inspected.package_id) +
              " is already installed.\n\nVerify and install this package as "
              "an update? The current version will be retained for rollback.",
          "Update portable package", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION,
          manager_dialog_.get()) != wxYES) {
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
  const StoreResult result = package_store_->Install(archive_path, replacing);
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
    SetManagerStatus(
        "Installation failed without replacing the current "
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
    if (!PreparePermissions(result.package_id, true, &permission_diagnostic)) {
      SetManagerStatus("Package updated and left disabled: " +
                       permission_diagnostic);
      return;
    }
    if (!runtime_engine_->Enable(result.package_id, &runtime_diagnostic)) {
      wxString recovery;
      RestorePreviousPackage(result.package_id, true, &recovery);
      SetManagerStatus(
          "Updated runtime failed to enable; the previous "
          "version was restored. " +
          wxString::FromUTF8(runtime_diagnostic) +
          (recovery.empty() ? wxString() : "\n" + recovery));
      return;
    }
    const StoreResult persisted =
        package_store_->SetEnabled(result.package_id, true);
    if (!persisted.okay) {
      runtime_engine_->Disable(result.package_id, &runtime_diagnostic);
      SetManagerStatus(
          "Package updated but was left disabled because its "
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
    SetManagerStatus(
        "The package started, but was stopped because its "
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
  SetManagerStatus(wxString::FromUTF8(package_id) +
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

void PortablePluginManagerPi::RollbackPackage(const std::string& package_id) {
  package_store_->SetEnabled(package_id, false);
  std::string ignored;
  runtime_engine_->Unload(package_id, &ignored);
  const StoreResult rolled_back = package_store_->Rollback(package_id);
  std::string runtime_diagnostic;
  const StoreResult audit = rolled_back.okay
                                ? package_store_->AuditInstalled(package_id)
                                : StoreResult{};
  const bool loaded = rolled_back.okay && audit.okay &&
                      runtime_engine_->RefreshPackage(
                          package_id, developer_mode_, &runtime_diagnostic);
  RefreshManager();
  SetManagerStatus(
      loaded ? wxString::FromUTF8(package_id) +
                   " rolled back and left disabled for review."
             : "Rollback failed: " +
                   wxString::FromUTF8(!rolled_back.okay ? rolled_back.message
                                      : !audit.okay     ? audit.message
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
  vessel_position_valid_ =
      std::isfinite(fix.Lat) && std::isfinite(fix.Lon) &&
      fix.Lat >= -90.0 && fix.Lat <= 90.0 &&
      fix.Lon >= -180.0 && fix.Lon <= 180.0;
  if (vessel_position_valid_) {
    vessel_latitude_ = fix.Lat;
    vessel_longitude_ = fix.Lon;
  }
  if (runtime_engine_) runtime_engine_->SetPositionFix(fix);
}

void PortablePluginManagerPi::SetCursorLatLon(double latitude,
                                               double longitude) {
  cursor_position_valid_ =
      std::isfinite(latitude) && std::isfinite(longitude) &&
      latitude >= -90.0 && latitude <= 90.0 &&
      longitude >= -180.0 && longitude <= 180.0;
  if (cursor_position_valid_) {
    cursor_latitude_ = latitude;
    cursor_longitude_ = longitude;
    if (environment_workbench_)
      environment_workbench_->SetCursorPosition(latitude, longitude);
  }
}

std::vector<PortableNavigationPosition>
PortablePluginManagerPi::ListWaypoints() const {
  std::vector<PortableNavigationPosition> result;
  const wxArrayString identifiers = GetWaypointGUIDArray();
  result.reserve(std::min<std::size_t>(identifiers.size(), 50'000));
  for (std::size_t index = 0;
       index < identifiers.size() && result.size() < 50'000; ++index) {
    const auto waypoint = GetWaypoint_Plugin(identifiers[index]);
    if (!waypoint || !std::isfinite(waypoint->m_lat) ||
        !std::isfinite(waypoint->m_lon) ||
        std::abs(waypoint->m_lat) > 90.0 ||
        std::abs(waypoint->m_lon) > 180.0) {
      continue;
    }
    result.push_back(
        {waypoint->m_GUID,
         waypoint->m_MarkName.empty() ? waypoint->m_GUID
                                      : waypoint->m_MarkName,
         waypoint->m_lat, waypoint->m_lon});
  }
  return result;
}

std::vector<PortableNavigationRoute>
PortablePluginManagerPi::ListRoutes() const {
  std::vector<PortableNavigationRoute> result;
  const wxArrayString identifiers = GetRouteGUIDArray();
  result.reserve(std::min<std::size_t>(identifiers.size(), 10'000));
  for (std::size_t index = 0;
       index < identifiers.size() && result.size() < 10'000; ++index) {
    const auto route = GetRoute_Plugin(identifiers[index]);
    if (!route || !route->pWaypointList) continue;
    PortableNavigationRoute copied;
    copied.id = route->m_GUID;
    copied.name =
        route->m_NameString.empty() ? route->m_GUID : route->m_NameString;
    for (auto node = route->pWaypointList->GetFirst();
         node && copied.points.size() < 20'000; node = node->GetNext()) {
      const PlugIn_Waypoint* waypoint = node->GetData();
      if (!waypoint || !std::isfinite(waypoint->m_lat) ||
          !std::isfinite(waypoint->m_lon) ||
          std::abs(waypoint->m_lat) > 90.0 ||
          std::abs(waypoint->m_lon) > 180.0) {
        continue;
      }
      copied.points.push_back(
          {waypoint->m_GUID,
           waypoint->m_MarkName.empty() ? waypoint->m_GUID
                                        : waypoint->m_MarkName,
           waypoint->m_lat, waypoint->m_lon});
    }
    if (copied.points.size() >= 2) result.push_back(std::move(copied));
  }
  return result;
}

bool PortablePluginManagerPi::CreateOpenCpnRoute(
    const wxString& name,
    const std::vector<PortableNavigationPosition>& points,
    wxString* diagnostic) {
  if (points.size() < 2 || points.size() > 2'000) {
    if (diagnostic)
      *diagnostic = "A route must contain between 2 and 2,000 points.";
    return false;
  }
  PlugIn_Route route;
  route.m_NameString = name;
  route.m_StartString = points.front().name;
  route.m_EndString = points.back().name;
  for (const auto& point : points) {
    if (!std::isfinite(point.latitude) || !std::isfinite(point.longitude) ||
        std::abs(point.latitude) > 90.0 ||
        std::abs(point.longitude) > 180.0) {
      if (diagnostic) *diagnostic = "The route contains an invalid position.";
      return false;
    }
    route.pWaypointList->Append(new PlugIn_Waypoint(
        point.latitude, point.longitude, "circle", point.name));
  }
  if (!AddPlugInRoute(&route, true)) {
    if (diagnostic) *diagnostic = "OpenCPN rejected the generated route.";
    return false;
  }
  return true;
}

void PortablePluginManagerPi::SetNMEASentence(wxString& sentence) {
  if (!runtime_engine_) return;
  const wxScopedCharBuffer value = sentence.utf8_str();
  if (value)
    runtime_engine_->DeliverNavigationSentence(
        std::string(value.data(), value.length()));
}

bool PortablePluginManagerPi::RenderOverlayMultiCanvas(
    wxDC& dc, PlugIn_ViewPort* viewport, int, int priority) {
  if (!runtime_engine_ || !viewport || priority != 0) return false;
  view_bounds_valid_ =
      viewport->bValid && std::isfinite(viewport->lon_min) &&
      std::isfinite(viewport->lat_min) && std::isfinite(viewport->lon_max) &&
      std::isfinite(viewport->lat_max) && viewport->lon_min < viewport->lon_max &&
      viewport->lat_min < viewport->lat_max;
  if (view_bounds_valid_) {
    view_west_ = viewport->lon_min;
    view_south_ = viewport->lat_min;
    view_east_ = viewport->lon_max;
    view_north_ = viewport->lat_max;
  }
  bool rendered =
      weather_routing_host_ &&
      weather_routing_host_->Render(dc, viewport);
  const bool environment_rendered =
      environment_workbench_ && environment_workbench_->Render(dc, viewport);
  rendered = environment_rendered || rendered;
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
  if (developer_mode_ && environment_rendered &&
      !developer_software_overlay_logged_) {
    wxString dataset_error;
    if (environment_workbench_->AcquireDataset(&dataset_error)) {
      developer_software_overlay_logged_ = true;
      wxLogMessage(
          "PPM event=environment-overlay-rendered path=wxdc dataset=ready "
          "renderer-compatible=software,vulkan");
    }
  }
  return rendered;
}

bool PortablePluginManagerPi::RenderGLOverlayMultiCanvas(
    wxGLContext*, PlugIn_ViewPort* viewport, int, int priority) {
  if (!runtime_engine_ || !viewport || priority != 0) return false;
  view_bounds_valid_ =
      viewport->bValid && std::isfinite(viewport->lon_min) &&
      std::isfinite(viewport->lat_min) && std::isfinite(viewport->lon_max) &&
      std::isfinite(viewport->lat_max) && viewport->lon_min < viewport->lon_max &&
      viewport->lat_min < viewport->lat_max;
  if (view_bounds_valid_) {
    view_west_ = viewport->lon_min;
    view_south_ = viewport->lat_min;
    view_east_ = viewport->lon_max;
    view_north_ = viewport->lat_max;
  }
  bool rendered =
      weather_routing_host_ && weather_routing_host_->RenderGL(viewport);
  const bool environment_rendered =
      environment_workbench_ &&
      RenderEnvironmentWithDesktopGl(environment_workbench_.get(), viewport);
  rendered = environment_rendered || rendered;
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
      glVertex2i(pixel.x, pixel.y);
    }
    glEnd();
    glPopAttrib();
    rendered = true;
  }
  if (developer_mode_ && environment_rendered &&
      !developer_opengl_overlay_logged_) {
    wxString dataset_error;
    if (environment_workbench_->AcquireDataset(&dataset_error)) {
      developer_opengl_overlay_logged_ = true;
      wxLogMessage(
          "PPM event=environment-overlay-rendered "
          "path=opengl-compatibility dataset=ready");
    }
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
      package.access = !access.okay           ? "Blocked"
                       : access.current       ? "Approved"
                       : access.added.empty() ? "Access reduced"
                                              : "Approval required";
    }
    manager_dialog_->SetPackages(packages);
    const auto failures = std::count_if(
        packages.begin(), packages.end(),
        [](const auto& value) { return value.state == "Failed"; });
    manager_dialog_->SetStatus(
        failures == 0
            ? wxString::Format("%zu installed package%s.", packages.size(),
                               packages.size() == 1 ? "" : "s")
            : wxString::Format("%zu installed package%s; %zu "
                               "requires attention.",
                               packages.size(), packages.size() == 1 ? "" : "s",
                               failures));
  }
  if (runtime_engine_) {
    for (const auto& package : runtime_engine_->Packages()) {
      if (package.state != "Enabled") ClosePackageSurfaces(package.id);
    }
  }
}

}  // namespace ppm
