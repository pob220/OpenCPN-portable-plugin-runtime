/***************************************************************************
 * Experimental portable plugin runtime for OpenCPN.
 *
 * No OpenCPN, wxWidgets, graphics or operating-system objects cross the
 * component boundary.  The C callback table below adapts versioned values to
 * existing core facilities while the public service layer is developed.
 ***************************************************************************/

#include "portable_plugin_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <wx/app.h>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/log.h>
#include <wx/regex.h>
#include <wx/wfstream.h>

#include "ocpn_portable_runtime.h"

#include "model/base_platform.h"
#include "model/own_ship.h"
#include "model/plugin_loader.h"
#include "navutil.h"
#include "ocpndc.h"
#include "pluginmanager.h"
#include "top_frame.h"
#include "viewport.h"

namespace {

constexpr size_t kErrorBufferSize = 4096;
constexpr size_t kSettingValueLimit = 64 * 1024;
constexpr size_t kOverlayPointLimit = 1'000'000;
constexpr unsigned kMaximumWorkUnits = 10'000;

wxString FromUtf8(const char* data, size_t length) {
  if (!data || length == 0) return wxString();
  return wxString::FromUTF8(data, length);
}

bool IsSafeName(const wxString& value) {
  if (value.empty() || value.length() > 128) return false;
  for (const auto ch : value) {
    if (!(wxIsalnum(ch) || ch == '-' || ch == '_' || ch == '.')) return false;
  }
  return true;
}

bool IsPackageId(const wxString& value) {
  if (value.empty() || value.length() > 128 || value.Find('.') == wxNOT_FOUND)
    return false;
  bool at_label_start = true;
  wxUniChar previous;
  for (const auto ch : value) {
    if (ch == '.') {
      if (at_label_start || previous == '-') return false;
      at_label_start = true;
      previous = ch;
      continue;
    }
    const bool alpha = ch >= 'a' && ch <= 'z';
    const bool digit = ch >= '0' && ch <= '9';
    if (!(alpha || digit || ch == '-') || (at_label_start && ch == '-'))
      return false;
    at_label_start = false;
    previous = ch;
  }
  return !at_label_start && previous != '-';
}

bool IsSemanticVersion(const wxString& value) {
  static const wxRegEx expression(
      "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)"
      "(-[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?"
      "(\\+[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?$");
  return value.length() <= 128 && expression.IsValid() &&
         expression.Matches(value);
}

bool IsSafeRelativePath(const wxString& value) {
  if (value.empty()) return false;
  wxFileName path(value);
  if (path.IsAbsolute()) return false;
  const wxArrayString dirs = path.GetDirs();
  for (const auto& dir : dirs) {
    if (dir == ".." || dir == ".") return false;
  }
  return true;
}

struct ScenePoint {
  double latitude;
  double longitude;
};

struct Scene {
  std::vector<ScenePoint> points;
  wxColour colour;
  float width;
};

struct Job {
  std::atomic<bool> cancel{false};
  std::thread worker;
};

}  // namespace

class PortablePluginManager::Impl {
public:
  struct Instance {
    Impl* owner = nullptr;
    wxString id;
    wxString name;
    wxString version;
    wxString package_root;
    std::set<wxString> permissions;
    ocpn_portable_runtime* runtime = nullptr;
    bool enabled = false;
    bool failed = false;
    std::map<wxString, Scene> scenes;
    std::map<wxString, std::shared_ptr<Job>> jobs;
  };

  struct Action {
    Instance* instance;
    wxString action_id;
  };

  explicit Impl(PlugInManager* plugin_manager)
      : plugin_manager(plugin_manager),
        alive(std::make_shared<std::atomic<bool>>(true)) {}

  ~Impl() { Shutdown(); }

  bool Load();
  void Shutdown();
  bool HandleToolbarAction(int toolbar_id);
  bool Render(ocpnDC& dc, const ViewPort& viewport, int priority);

  bool HasPermission(const Instance& instance, const wxString& permission) {
    return instance.permissions.count(permission) != 0;
  }

  void DeliverJobEvent(const wxString& plugin_id, const wxString& job_id,
                       uint32_t kind, uint8_t progress,
                       const wxString& message = wxString());
  void StopJobs(Instance& instance);
  void Fail(Instance& instance, const wxString& operation,
            const wxString& error);

  PlugInManager* plugin_manager;
  std::vector<std::unique_ptr<Instance>> instances;
  std::map<int, Action> actions;
  std::shared_ptr<std::atomic<bool>> alive;
  bool stopped = false;

  static void Log(void* user_data, uint32_t level, const char* message,
                  size_t message_len);
  static int32_t RegisterAction(void* user_data, const char* action_id,
                                size_t action_id_len, const char* label,
                                size_t label_len, const char* tooltip,
                                size_t tooltip_len, const char* icon_resource,
                                size_t icon_resource_len,
                                uint32_t* host_action_id);
  static int32_t GetVesselPosition(void* user_data, double* latitude,
                                   double* longitude, double* cog,
                                   uint8_t* has_cog, double* sog,
                                   uint8_t* has_sog);
  static int32_t SettingGet(void* user_data, const char* key, size_t key_len,
                            char* value, size_t value_capacity,
                            size_t* value_len, uint8_t* found);
  static int32_t SettingSet(void* user_data, const char* key, size_t key_len,
                            const char* value, size_t value_len);
  static int32_t SubmitPolyline(void* user_data, const char* scene_id,
                                size_t scene_id_len,
                                const ocpn_portable_geo_point* points,
                                size_t point_count,
                                ocpn_portable_overlay_style style);
  static int32_t ClearScene(void* user_data, const char* scene_id,
                            size_t scene_id_len);
  static int32_t StartJob(void* user_data, const char* job_id,
                          size_t job_id_len, uint32_t work_units);
  static int32_t CancelJob(void* user_data, const char* job_id,
                           size_t job_id_len);
  static int32_t OpenEnvironmentalViewer(void* user_data);
};

void PortablePluginManager::Impl::Log(void* user_data, uint32_t level,
                                      const char* message, size_t message_len) {
  auto* instance = static_cast<Instance*>(user_data);
  const wxString text = wxString::Format("Portable plugin %s: %s", instance->id,
                                         FromUtf8(message, message_len));
  switch (level) {
    case 0:
      wxLogDebug("%s", text);
      break;
    case 2:
      wxLogWarning("%s", text);
      break;
    case 3:
      wxLogError("%s", text);
      break;
    default:
      wxLogMessage("%s", text);
      break;
  }
}

int32_t PortablePluginManager::Impl::RegisterAction(
    void* user_data, const char* action_id, size_t action_id_len,
    const char* label, size_t label_len, const char* tooltip,
    size_t tooltip_len, const char* icon_resource, size_t icon_resource_len,
    uint32_t* host_action_id) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner || !host_action_id ||
      !instance->owner->HasPermission(*instance, "ui.commands"))
    return -1;
  const wxString action = FromUtf8(action_id, action_id_len);
  if (!IsSafeName(action)) return -2;
  for (const auto& [tool_id, registered] : instance->owner->actions) {
    (void)tool_id;
    if (registered.instance == instance && registered.action_id == action)
      return -5;
  }

  wxString icon;
  const wxString resource = FromUtf8(icon_resource, icon_resource_len);
  if (!resource.empty()) {
    if (!IsSafeRelativePath(resource)) return -3;
    wxFileName icon_path(instance->package_root + wxFILE_SEP_PATH + resource);
    icon_path.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
    icon = icon_path.GetFullPath();
    if (!wxFileExists(icon)) return -4;
  }

  const int tool_id = instance->owner->plugin_manager->AddToolbarTool(
      FromUtf8(label, label_len), icon, icon, icon, wxITEM_NORMAL,
      FromUtf8(tooltip, tooltip_len), FromUtf8(tooltip, tooltip_len), nullptr,
      -1, 0, nullptr);
  instance->owner->actions.emplace(tool_id, Action{instance, action});
  *host_action_id = static_cast<uint32_t>(tool_id);
  return 0;
}

int32_t PortablePluginManager::Impl::GetVesselPosition(
    void* user_data, double* latitude, double* longitude, double* cog,
    uint8_t* has_cog, double* sog, uint8_t* has_sog) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner ||
      !instance->owner->HasPermission(*instance, "navigation.position.read") ||
      !latitude || !longitude || !cog || !has_cog || !sog || !has_sog)
    return -1;
  if (!bGPSValid || !std::isfinite(gLat) || !std::isfinite(gLon)) return -2;
  *latitude = gLat;
  *longitude = gLon;
  *has_cog = std::isfinite(gCog) ? 1 : 0;
  *has_sog = std::isfinite(gSog) ? 1 : 0;
  *cog = *has_cog ? gCog : 0.0;
  *sog = *has_sog ? gSog : 0.0;
  return 0;
}

int32_t PortablePluginManager::Impl::SettingGet(
    void* user_data, const char* key, size_t key_len, char* value,
    size_t value_capacity, size_t* value_len, uint8_t* found) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner ||
      !instance->owner->HasPermission(*instance, "settings.read-write") ||
      !value_len || !found)
    return -1;
  const wxString setting = FromUtf8(key, key_len);
  if (!IsSafeName(setting)) return -2;
  const wxString old_path = pConfig->GetPath();
  pConfig->SetPath("/PortablePlugins/" + instance->id + "/Settings");
  wxString result;
  const bool exists = pConfig->Read(setting, &result);
  pConfig->SetPath(old_path);
  *found = exists ? 1 : 0;
  if (!exists) {
    *value_len = 0;
    return 0;
  }
  const wxScopedCharBuffer encoded = result.utf8_str();
  const size_t length = encoded.length();
  *value_len = length;
  if (length > value_capacity || (length && !value)) return -3;
  if (length) std::memcpy(value, encoded.data(), length);
  return 0;
}

int32_t PortablePluginManager::Impl::SettingSet(void* user_data,
                                                const char* key, size_t key_len,
                                                const char* value,
                                                size_t value_len) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner ||
      !instance->owner->HasPermission(*instance, "settings.read-write") ||
      value_len > kSettingValueLimit)
    return -1;
  const wxString setting = FromUtf8(key, key_len);
  if (!IsSafeName(setting)) return -2;
  const wxString old_path = pConfig->GetPath();
  pConfig->SetPath("/PortablePlugins/" + instance->id + "/Settings");
  const bool ok = pConfig->Write(setting, FromUtf8(value, value_len));
  pConfig->SetPath(old_path);
  return ok ? 0 : -3;
}

int32_t PortablePluginManager::Impl::SubmitPolyline(
    void* user_data, const char* scene_id, size_t scene_id_len,
    const ocpn_portable_geo_point* points, size_t point_count,
    ocpn_portable_overlay_style style) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner ||
      !instance->owner->HasPermission(*instance, "overlay.submit") || !points ||
      point_count < 2 || point_count > kOverlayPointLimit ||
      !std::isfinite(style.width_pixels) || style.width_pixels <= 0 ||
      style.width_pixels > 64)
    return -1;
  const wxString id = FromUtf8(scene_id, scene_id_len);
  if (!IsSafeName(id)) return -2;
  Scene scene;
  scene.points.reserve(point_count);
  for (size_t i = 0; i < point_count; ++i) {
    if (!std::isfinite(points[i].latitude) ||
        !std::isfinite(points[i].longitude) || points[i].latitude < -90 ||
        points[i].latitude > 90 || points[i].longitude < -180 ||
        points[i].longitude > 180)
      return -3;
    scene.points.push_back({points[i].latitude, points[i].longitude});
  }
  scene.colour = wxColour(style.red, style.green, style.blue, style.alpha);
  scene.width = style.width_pixels;
  instance->scenes[id] = std::move(scene);
  wxLogMessage("Portable plugin %s submitted overlay scene %s (%zu points)",
               instance->id, id, point_count);
  top_frame::Get()->RefreshAllCanvas(false);
  return 0;
}

int32_t PortablePluginManager::Impl::ClearScene(void* user_data,
                                                const char* scene_id,
                                                size_t scene_id_len) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner ||
      !instance->owner->HasPermission(*instance, "overlay.submit"))
    return -1;
  const wxString id = FromUtf8(scene_id, scene_id_len);
  if (!IsSafeName(id)) return -2;
  instance->scenes.erase(id);
  top_frame::Get()->RefreshAllCanvas(false);
  return 0;
}

int32_t PortablePluginManager::Impl::StartJob(void* user_data,
                                              const char* job_id,
                                              size_t job_id_len,
                                              uint32_t work_units) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner ||
      !instance->owner->HasPermission(*instance, "jobs.compute") ||
      work_units == 0 || work_units > kMaximumWorkUnits)
    return -1;
  const wxString id = FromUtf8(job_id, job_id_len);
  if (!IsSafeName(id) || instance->jobs.count(id)) return -2;
  auto job = std::make_shared<Job>();
  instance->jobs[id] = job;
  auto* owner = instance->owner;
  const wxString plugin_id = instance->id;
  auto alive_token = owner->alive;
  job->worker =
      std::thread([owner, alive_token, job, id, plugin_id, work_units] {
        for (uint32_t unit = 1; unit <= work_units; ++unit) {
          if (job->cancel.load()) {
            wxTheApp->CallAfter([owner, alive_token, plugin_id, id] {
              if (!alive_token->load()) return;
              owner->DeliverJobEvent(plugin_id, id, 2, 0);
            });
            return;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          const auto progress = static_cast<uint8_t>((unit * 100) / work_units);
          wxTheApp->CallAfter([owner, alive_token, plugin_id, id, progress] {
            if (!alive_token->load()) return;
            owner->DeliverJobEvent(plugin_id, id, 0, progress);
          });
        }
        wxTheApp->CallAfter([owner, alive_token, plugin_id, id] {
          if (!alive_token->load()) return;
          owner->DeliverJobEvent(plugin_id, id, 1, 100);
        });
      });
  return 0;
}

int32_t PortablePluginManager::Impl::CancelJob(void* user_data,
                                               const char* job_id,
                                               size_t job_id_len) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner ||
      !instance->owner->HasPermission(*instance, "jobs.compute"))
    return -1;
  const wxString id = FromUtf8(job_id, job_id_len);
  const auto found = instance->jobs.find(id);
  if (found == instance->jobs.end()) return -2;
  found->second->cancel.store(true);
  return 0;
}

int32_t PortablePluginManager::Impl::OpenEnvironmentalViewer(void* user_data) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner ||
      !instance->owner->HasPermission(*instance,
                                      "environment.compat.xgrib-viewer"))
    return -1;

  const auto* plugins = PluginLoader::GetInstance()->GetPlugInArray();
  for (auto* plugin : *plugins) {
    if (!plugin || !plugin->m_pplugin || !plugin->m_enabled ||
        !plugin->m_init_state || plugin->m_common_name.CmpNoCase("xGRIB") != 0)
      continue;
    for (auto* tool :
         instance->owner->plugin_manager->GetPluginToolbarToolArray()) {
      if (tool && tool->m_pplugin == plugin->m_pplugin) {
        wxLogMessage(
            "Portable plugin %s opened typed native service "
            "org.opencpn.xgrib.viewer",
            instance->id);
        plugin->m_pplugin->OnToolbarToolCallback(tool->id);
        return 0;
      }
    }
    return -3;
  }
  return -2;
}

void PortablePluginManager::Impl::DeliverJobEvent(const wxString& plugin_id,
                                                  const wxString& job_id,
                                                  uint32_t kind,
                                                  uint8_t progress,
                                                  const wxString& message) {
  for (const auto& instance : instances) {
    if (instance->id != plugin_id || !instance->enabled || instance->failed)
      continue;
    char error[kErrorBufferSize] = {};
    const auto jid = job_id.utf8_str();
    const auto msg = message.utf8_str();
    const int result = ocpn_portable_runtime_on_job_event(
        instance->runtime, jid.data(), jid.length(), kind, progress, msg.data(),
        msg.length(), error, sizeof(error));
    if (kind != 0) {
      auto found = instance->jobs.find(job_id);
      if (found != instance->jobs.end()) {
        if (found->second->worker.joinable()) found->second->worker.join();
        instance->jobs.erase(found);
      }
    }
    if (result != 0) Fail(*instance, "job event", wxString::FromUTF8(error));
    return;
  }
}

void PortablePluginManager::Impl::StopJobs(Instance& instance) {
  for (auto& [id, job] : instance.jobs) job->cancel.store(true);
  for (auto& [id, job] : instance.jobs) {
    if (job->worker.joinable()) job->worker.join();
  }
  instance.jobs.clear();
}

void PortablePluginManager::Impl::Fail(Instance& instance,
                                       const wxString& operation,
                                       const wxString& error) {
  instance.failed = true;
  instance.enabled = false;
  instance.scenes.clear();
  for (auto& [id, job] : instance.jobs) job->cancel.store(true);
  wxLogError("Portable plugin %s failed during %s; failure contained: %s",
             instance.id, operation, error);
  top_frame::Get()->RefreshAllCanvas(false);
}

bool PortablePluginManager::Impl::Load() {
  if (stopped) return false;
  bool enabled = false;
  bool developer_mode = false;
  wxString developer_startup_action;
  const wxString old_path = pConfig->GetPath();
  pConfig->SetPath("/PortablePlugins");
  pConfig->Read("EnableExperimental", &enabled, false);
  pConfig->Read("DeveloperMode", &developer_mode, false);
  pConfig->Read("DeveloperStartupAction", &developer_startup_action,
                wxEmptyString);
  pConfig->SetPath(old_path);
  if (!enabled) {
    wxLogMessage("Experimental portable plugin runtime is disabled");
    return true;
  }

  const wxString package_dir = g_BasePlatform->GetPrivateDataDir() +
                               wxFILE_SEP_PATH + "portable-plugins";
  wxDir directory(package_dir);
  if (!directory.IsOpened()) {
    wxLogMessage("Portable plugin directory does not exist: %s", package_dir);
    return true;
  }

  const std::set<wxString> known_permissions = {
      "ui.commands",         "navigation.position.read",
      "settings.read-write", "overlay.submit",
      "jobs.compute",        "environment.compat.xgrib-viewer"};
  wxString entry;
  bool more = directory.GetFirst(&entry, wxEmptyString, wxDIR_DIRS);
  while (more) {
    const wxString root_path = package_dir + wxFILE_SEP_PATH + entry;
    const wxString manifest_path =
        root_path + wxFILE_SEP_PATH + "manifest.json";
    wxFileInputStream input(manifest_path);
    wxJSONValue manifest;
    wxJSONReader reader;
    if (!input.IsOk() || reader.Parse(input, &manifest) != 0 ||
        !manifest.IsObject()) {
      wxLogError("Portable plugin manifest is invalid: %s", manifest_path);
      more = directory.GetNext(&entry);
      continue;
    }

    const int format = manifest["format_version"].AsInt();
    const wxString id = manifest["id"].AsString();
    const wxString name = manifest["name"].AsString();
    const wxString version = manifest["version"].AsString();
    const wxString component = manifest["component"].AsString();
    const wxString runtime_range = manifest["runtime"].AsString();
    const wxString api_range = manifest["portable_api"].AsString();
    const bool typed_manifest =
        manifest["format_version"].IsInt() && manifest["id"].IsString() &&
        manifest["name"].IsString() && manifest["version"].IsString() &&
        manifest["component"].IsString() && manifest["runtime"].IsString() &&
        manifest["portable_api"].IsString() &&
        manifest["development"].IsBool() && manifest["permissions"].IsArray();
    if (!typed_manifest || format != 1 || !IsPackageId(id) || name.empty() ||
        !IsSemanticVersion(version) || !IsSafeRelativePath(component) ||
        runtime_range != ">=0.1.0 <0.2.0" || api_range != ">=0.1.0 <0.2.0") {
      wxLogError("Portable plugin %s is incompatible with runtime/API 0.1", id);
      more = directory.GetNext(&entry);
      continue;
    }
    if (manifest["development"].AsBool() && !developer_mode) {
      wxLogWarning(
          "Portable development package %s skipped; developer mode is off", id);
      more = directory.GetNext(&entry);
      continue;
    }

    auto instance = std::make_unique<Instance>();
    instance->owner = this;
    instance->id = id;
    instance->name = name;
    instance->version = version;
    instance->package_root = root_path;
    bool permission_error = false;
    wxJSONValue requested = manifest["permissions"];
    for (int i = 0; i < requested.Size(); ++i) {
      if (!requested[i].IsString()) {
        wxLogError("Portable plugin %s has a non-string permission", id);
        permission_error = true;
        continue;
      }
      const wxString permission = requested[i].AsString();
      if (!known_permissions.count(permission)) {
        wxLogError("Portable plugin %s requests unknown permission %s", id,
                   permission);
        permission_error = true;
      } else {
        instance->permissions.insert(permission);
      }
    }
    if (permission_error) {
      more = directory.GetNext(&entry);
      continue;
    }

    const wxString component_path = root_path + wxFILE_SEP_PATH + component;
    if (!wxFileExists(component_path)) {
      wxLogError("Portable plugin component is missing: %s", component_path);
      more = directory.GetNext(&entry);
      continue;
    }
    ocpn_portable_host_callbacks callbacks{};
    callbacks.abi_version = OCPN_PORTABLE_HOST_ABI_VERSION;
    callbacks.user_data = instance.get();
    callbacks.log = Log;
    callbacks.register_action = RegisterAction;
    callbacks.get_vessel_position = GetVesselPosition;
    callbacks.setting_get = SettingGet;
    callbacks.setting_set = SettingSet;
    callbacks.submit_polyline = SubmitPolyline;
    callbacks.clear_scene = ClearScene;
    callbacks.start_job = StartJob;
    callbacks.cancel_job = CancelJob;
    callbacks.open_environmental_viewer = OpenEnvironmentalViewer;
    char error[kErrorBufferSize] = {};
    const auto native_path = component_path.utf8_str();
    instance->runtime = ocpn_portable_runtime_create(
        native_path.data(), &callbacks, error, sizeof(error));
    const auto expected_id = id.utf8_str();
    const auto expected_name = name.utf8_str();
    const auto expected_version = version.utf8_str();
    if (!instance->runtime ||
        ocpn_portable_runtime_initialize(
            instance->runtime, expected_id.data(), expected_id.length(),
            expected_name.data(), expected_name.length(),
            expected_version.data(), expected_version.length(), error,
            sizeof(error)) != 0 ||
        ocpn_portable_runtime_enable(instance->runtime, error, sizeof(error)) !=
            0) {
      wxLogError("Portable plugin %s could not start: %s", id,
                 wxString::FromUTF8(error));
      if (instance->runtime) ocpn_portable_runtime_destroy(instance->runtime);
      instance->runtime = nullptr;
      for (auto it = actions.begin(); it != actions.end();) {
        if (it->second.instance == instance.get()) {
          plugin_manager->RemoveToolbarTool(it->first);
          it = actions.erase(it);
        } else {
          ++it;
        }
      }
      more = directory.GetNext(&entry);
      continue;
    }
    instance->enabled = true;
    wxLogMessage("Portable plugin loaded: %s %s (%s)", name, version, id);
    instances.push_back(std::move(instance));
    more = directory.GetNext(&entry);
  }
  if (!actions.empty())
    plugin_manager->GetParentFrame()->RequestNewToolbars(true);
  if (developer_mode && !developer_startup_action.empty()) {
    const auto requested =
        std::find_if(actions.begin(), actions.end(), [&](const auto& entry) {
          return entry.second.action_id == developer_startup_action;
        });
    if (requested == actions.end()) {
      wxLogError("Portable developer startup action was not found: %s",
                 developer_startup_action);
    } else {
      const int tool_id = requested->first;
      const auto live = alive;
      wxTheApp->CallAfter([this, live, tool_id, developer_startup_action]() {
        if (!live->load() || stopped) return;
        wxLogMessage("Invoking portable developer startup action: %s",
                     developer_startup_action);
        HandleToolbarAction(tool_id);
      });
    }
  }
  return true;
}

void PortablePluginManager::Impl::Shutdown() {
  if (stopped) return;
  stopped = true;
  alive->store(false);
  for (auto& instance : instances) {
    StopJobs(*instance);
    if (instance->runtime && instance->enabled && !instance->failed) {
      char error[kErrorBufferSize] = {};
      if (ocpn_portable_runtime_disable(instance->runtime, error,
                                        sizeof(error)) != 0)
        wxLogWarning("Portable plugin %s did not disable cleanly: %s",
                     instance->id, wxString::FromUTF8(error));
    }
    if (instance->runtime) {
      ocpn_portable_runtime_destroy(instance->runtime);
      instance->runtime = nullptr;
    }
  }
  for (const auto& [tool_id, action] : actions)
    plugin_manager->RemoveToolbarTool(tool_id);
  actions.clear();
  instances.clear();
}

bool PortablePluginManager::Impl::HandleToolbarAction(int toolbar_id) {
  const auto found = actions.find(toolbar_id);
  if (found == actions.end()) return false;
  Instance& instance = *found->second.instance;
  if (!instance.enabled || instance.failed) {
    wxLogWarning("Portable plugin action ignored because %s is not operational",
                 instance.id);
    return true;
  }
  wxLogMessage("Portable plugin %s handling action %s", instance.id,
               found->second.action_id);
  char error[kErrorBufferSize] = {};
  const auto action = found->second.action_id.utf8_str();
  if (ocpn_portable_runtime_on_action(instance.runtime, action.data(),
                                      action.length(), error,
                                      sizeof(error)) != 0)
    Fail(instance, "action " + found->second.action_id,
         wxString::FromUTF8(error));
  return true;
}

bool PortablePluginManager::Impl::Render(ocpnDC& dc, const ViewPort& viewport,
                                         int priority) {
  if (priority != 0) return false;
  bool rendered = false;
  ViewPort projection = viewport;
  for (const auto& instance : instances) {
    if (!instance->enabled || instance->failed) continue;
    for (const auto& [id, scene] : instance->scenes) {
      if (scene.points.size() < 2) continue;
      std::vector<wxPoint> points;
      points.reserve(scene.points.size());
      for (const auto& point : scene.points)
        points.push_back(
            projection.GetPixFromLL(point.latitude, point.longitude));
      dc.SetPen(wxPen(scene.colour, std::max(1, static_cast<int>(scene.width)),
                      wxPENSTYLE_SOLID));
      dc.DrawLines(static_cast<int>(points.size()), points.data());
      rendered = true;
    }
  }
  return rendered;
}

PortablePluginManager::PortablePluginManager(PlugInManager* plugin_manager)
    : m_impl(std::make_unique<Impl>(plugin_manager)) {}

PortablePluginManager::~PortablePluginManager() = default;

bool PortablePluginManager::Load() { return m_impl->Load(); }

void PortablePluginManager::Shutdown() { m_impl->Shutdown(); }

bool PortablePluginManager::HandleToolbarAction(int toolbar_id) {
  return m_impl->HandleToolbarAction(toolbar_id);
}

bool PortablePluginManager::Render(ocpnDC& dc, const ViewPort& viewport,
                                   int priority) {
  return m_impl->Render(dc, viewport, priority);
}
