/***************************************************************************
 * Experimental portable plugin runtime for OpenCPN.
 *
 * No OpenCPN, wxWidgets, graphics or operating-system objects cross the
 * component boundary.  The C callback table below adapts versioned values to
 * existing core facilities while the public service layer is developed.
 ***************************************************************************/

#include "portable_plugin_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <curl/curl.h>

#include <wx/app.h>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/log.h>
#include <wx/regex.h>
#include <wx/wfstream.h>

#include "ocpn_portable_runtime.h"
#include "portable_environment_host.h"
#include "portable_service_version.h"
#include "portable_weather_routing_host.h"

#include "model/base_platform.h"
#include "model/svg_utils.h"
#include "model/own_ship.h"
#include "chartdb.h"
#include "navutil.h"
#include "ocpndc.h"
#include "ocpn_plugin.h"
#include "pluginmanager.h"
#include "top_frame.h"
#include "viewport.h"

namespace {

constexpr size_t kErrorBufferSize = 4096;
constexpr size_t kSettingValueLimit = 64 * 1024;
constexpr size_t kOverlayPointLimit = 1'000'000;
constexpr unsigned kMaximumWorkUnits = 10'000;
constexpr size_t kChartSegmentLimit = 10'000;
constexpr uint64_t kNetworkDownloadLimit = 512ULL * 1024ULL * 1024ULL;
constexpr size_t kPrivateReadLimit = 8U * 1024U * 1024U;
constexpr size_t kNavigationObjectLimit = 10'000;

wxString FromUtf8(const char* data, size_t length) {
  if (!data || length == 0) return wxString();
  return wxString::FromUTF8(data, length);
}

void CopyUtf8(const wxString& value, char* output, size_t capacity) {
  if (!output || capacity == 0) return;
  const wxScopedCharBuffer encoded = value.ToUTF8();
  const size_t available = encoded.data() ? encoded.length() : 0;
  const size_t copied = std::min(available, capacity - 1);
  if (copied) std::memcpy(output, encoded.data(), copied);
  output[copied] = '\0';
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

struct DownloadSink {
  FILE* file = nullptr;
  std::shared_ptr<Job> job;
  uint64_t maximum = 0;
  uint64_t written = 0;
};

size_t WriteDownload(char* data, size_t size, size_t count, void* user_data) {
  auto* sink = static_cast<DownloadSink*>(user_data);
  const size_t bytes = size * count;
  if (!sink || !sink->file || sink->job->cancel.load() ||
      bytes > sink->maximum - std::min(sink->written, sink->maximum))
    return 0;
  const size_t written = std::fwrite(data, 1, bytes, sink->file);
  sink->written += written;
  return written;
}

int DownloadProgress(void* user_data, curl_off_t, curl_off_t now, curl_off_t,
                     curl_off_t) {
  auto* sink = static_cast<DownloadSink*>(user_data);
  if (!sink || sink->job->cancel.load()) return 1;
  return now >= 0 && static_cast<uint64_t>(now) > sink->maximum ? 1 : 0;
}

}  // namespace

class PortablePluginManager::Impl {
public:
  struct Instance {
    Impl* owner = nullptr;
    wxString id;
    wxString name;
    wxString version;
    wxString package_root;
    wxString private_root;
    wxString environment_surface;
    std::set<wxString> permissions;
    std::set<wxString> provides;
    std::set<wxString> required_services;
    std::map<wxString, wxString> provided_service_versions;
    std::map<wxString, wxString> required_service_ranges;
    ocpn_portable_runtime* runtime = nullptr;
    std::shared_ptr<std::mutex> runtime_mutex = std::make_shared<std::mutex>();
    bool enabled = false;
    bool failed = false;
    std::map<wxString, Scene> scenes;
    std::map<wxString, std::shared_ptr<Job>> jobs;
    std::unique_ptr<PortableEnvironmentHost> environmental_host;
    bool environmental_viewer_pending = false;
    PortableEnvironmentHost* held_environment_host = nullptr;
    std::shared_ptr<const PortableEnvironmentDataset> held_environment_dataset;
    std::unique_ptr<PortableWeatherRoutingHost> weather_routing_host;
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
  void SetCursorPosition(double latitude, double longitude);

  bool HasPermission(const Instance& instance, const wxString& permission) {
    return instance.permissions.count(permission) != 0;
  }

  bool ProvidesCompatibleService(const Instance& provider,
                                 const Instance& consumer,
                                 const wxString& interface) const {
    const auto provided = provider.provided_service_versions.find(interface);
    if (provided == provider.provided_service_versions.end()) return false;
    const auto required = consumer.required_service_ranges.find(interface);
    if (required == consumer.required_service_ranges.end()) return true;
    return ocpn::portable::ServiceVersionSatisfies(
        provided->second.ToStdString(), required->second.ToStdString());
  }

  bool SampleEnvironment(
      Instance& consumer,
      const std::vector<PortableEnvironmentRequest>& requests,
      std::vector<PortableEnvironmentSample>* results, wxString* error);

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
  bool cursor_position_available = false;
  double cursor_latitude = 0.0;
  double cursor_longitude = 0.0;

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
  static int32_t OpenWeatherRouting(void* user_data);
  static int32_t EnvironmentSampleBatch(
      void* user_data, const ocpn_portable_environment_sample_request* requests,
      size_t request_count, ocpn_portable_environment_sample* results,
      size_t result_count, char* error, size_t error_capacity);
  static void RoutingProgress(void* user_data, uint8_t percent,
                              const char* message, size_t message_len);
  static uint8_t RoutingCancelled(void* user_data);
  static int32_t ChartsQuerySegments(
      void* user_data, const ocpn_portable_geo_segment* segments,
      size_t segment_count, ocpn_portable_chart_segment_result* results,
      size_t result_count);
  static int32_t NetworkGetToPrivate(void* user_data, const char* request_id,
                                     size_t request_id_len, const char* url,
                                     size_t url_len, const char* private_name,
                                     size_t private_name_len,
                                     uint64_t max_bytes);
  static int32_t StoragePrivateRead(void* user_data, const char* private_name,
                                    size_t private_name_len, uint8_t* value,
                                    size_t value_capacity, size_t* value_len);
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

  const wxString action_label = FromUtf8(label, label_len);
  const wxString action_tooltip = FromUtf8(tooltip, tooltip_len);
  int tool_id = -1;
  if (!icon.empty()) {
    // Portable actions deliberately have no native opencpn_plugin owner. The
    // legacy toolbar consequently does not classify them as plugin tools and
    // would ignore their SVG paths, displaying the default jigsaw instead.
    // Decode the package-owned SVG here and use the value-based bitmap API.
    wxBitmap bitmap = LoadSVG(icon, 32, 32);
    if (!bitmap.IsOk()) return -6;
    tool_id = instance->owner->plugin_manager->AddToolbarTool(
        action_label, &bitmap, &bitmap, wxITEM_NORMAL, action_tooltip,
        action_tooltip, nullptr, -1, 0, nullptr);
  } else {
    tool_id = instance->owner->plugin_manager->AddToolbarTool(
        action_label, icon, icon, icon, wxITEM_NORMAL, action_tooltip,
        action_tooltip, nullptr, -1, 0, nullptr);
  }
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
      !instance->owner->HasPermission(*instance, "environment.datasets"))
    return -1;
  if (!instance->environmental_host) {
    if (instance->environment_surface.empty()) {
      wxLogError("Portable plugin %s does not declare environment.viewer",
                 instance->id);
      return -2;
    }
    instance->environmental_host = std::make_unique<PortableEnvironmentHost>(
        instance->owner->plugin_manager->GetParentFrame(), instance->id,
        instance->package_root, instance->environment_surface,
        instance->owner->HasPermission(*instance, "credentials.provider"),
        instance->runtime, instance->runtime_mutex);
  }

  // Host callbacks run while HandleToolbarAction owns runtime_mutex.  Opening
  // the viewer constructs its package-owned declarative surface and calls the
  // component controller, so doing it synchronously here would re-enter the
  // same runtime and deadlock the UI thread.  Queue the UI work after the
  // current component action has returned and released the runtime lock.
  if (instance->environmental_viewer_pending) return 0;
  instance->environmental_viewer_pending = true;
  auto* owner = instance->owner;
  auto* target = instance;
  const auto live = owner->alive;
  wxTheApp->CallAfter([owner, target, live] {
    if (!live->load() || owner->stopped) return;
    target->environmental_viewer_pending = false;
    if (!target->enabled || target->failed || !target->environmental_host)
      return;
    wxString error;
    if (!target->environmental_host->Show(&error)) {
      wxLogError("Portable plugin %s could not open environmental service: %s",
                 target->id, error);
      return;
    }
    wxLogMessage("Portable plugin %s opened host environmental service 0.1",
                 target->id);
  });
  return 0;
}

int32_t PortablePluginManager::Impl::OpenWeatherRouting(void* user_data) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner ||
      !instance->owner->HasPermission(*instance, "weather-routing.compute"))
    return -1;
  if (!instance->weather_routing_host) {
    auto summary = [owner = instance->owner, instance]() {
      for (const auto& candidate : owner->instances) {
        if (candidate->enabled && !candidate->failed &&
            owner->ProvidesCompatibleService(
                *candidate, *instance, "org.opencpn.environment.provider") &&
            candidate->environmental_host) {
          const wxString value =
              candidate->environmental_host->DatasetSummary();
          if (!value.StartsWith("No environmental")) return value;
        }
      }
      return wxString("No decoded environmental dataset is available");
    };
    auto waypoints = [owner = instance->owner, instance]() {
      std::vector<PortableNavigationPosition> result;
      if (!owner->HasPermission(*instance, "navigation.objects.read"))
        return result;
      const wxArrayString ids = GetWaypointGUIDArray();
      result.reserve(std::min(ids.size(), kNavigationObjectLimit));
      for (size_t index = 0;
           index < ids.size() && result.size() < kNavigationObjectLimit;
           ++index) {
        auto waypoint = GetWaypoint_Plugin(ids[index]);
        if (!waypoint || waypoint->m_GUID.empty() ||
            !std::isfinite(waypoint->m_lat) ||
            !std::isfinite(waypoint->m_lon) || waypoint->m_lat < -90.0 ||
            waypoint->m_lat > 90.0 || waypoint->m_lon < -180.0 ||
            waypoint->m_lon > 180.0)
          continue;
        wxString name = waypoint->m_MarkName;
        if (name.empty()) name = "Unnamed waypoint";
        result.push_back(
            {waypoint->m_GUID, name, waypoint->m_lat, waypoint->m_lon});
      }
      std::stable_sort(result.begin(), result.end(),
                       [](const auto& left, const auto& right) {
                         return left.name.CmpNoCase(right.name) < 0;
                       });
      return result;
    };
    auto vessel = [owner = instance->owner,
                   instance](PortableNavigationPosition* output) {
      if (!output ||
          !owner->HasPermission(*instance, "navigation.position.read") ||
          !bGPSValid || !std::isfinite(gLat) || !std::isfinite(gLon))
        return false;
      *output = {"opencpn:vessel", "Current vessel position", gLat, gLon};
      return true;
    };
    auto cursor = [owner = instance->owner,
                   instance](PortableNavigationPosition* output) {
      if (!output ||
          !owner->HasPermission(*instance, "navigation.position.read") ||
          !owner->cursor_position_available)
        return false;
      *output = {"opencpn:chart-cursor", "Latest chart cursor position",
                 owner->cursor_latitude, owner->cursor_longitude};
      return true;
    };
    auto displayed_time = [owner = instance->owner, instance](int64_t* output) {
      for (const auto& candidate : owner->instances) {
        if (candidate->enabled && !candidate->failed &&
            owner->ProvidesCompatibleService(
                *candidate, *instance, "org.opencpn.environment.provider") &&
            candidate->environmental_host &&
            candidate->environmental_host->DisplayedTime(output))
          return true;
      }
      return false;
    };
    auto preflight = [owner = instance->owner, instance](
                         double latitude, double longitude,
                         const std::vector<int64_t>& departure_times,
                         std::vector<uint8_t>* availability, wxString* error) {
      if (!availability) {
        if (error) *error = "invalid environmental preflight output";
        return false;
      }
      instance->held_environment_host = nullptr;
      instance->held_environment_dataset.reset();
      wxString acquisition_error =
          "no enabled portable environmental-data provider is available";
      for (const auto& provider : owner->instances) {
        if (!provider->enabled || provider->failed ||
            !owner->ProvidesCompatibleService(
                *provider, *instance, "org.opencpn.environment.provider") ||
            !provider->environmental_host)
          continue;
        wxString candidate_error;
        auto dataset =
            provider->environmental_host->AcquireDataset(&candidate_error);
        if (dataset) {
          instance->held_environment_host = provider->environmental_host.get();
          instance->held_environment_dataset = std::move(dataset);
          break;
        }
        if (!candidate_error.empty()) acquisition_error = candidate_error;
      }
      if (!instance->held_environment_dataset) {
        if (error) *error = acquisition_error;
        return false;
      }
      std::vector<PortableEnvironmentRequest> requests;
      requests.reserve(departure_times.size());
      for (const auto time : departure_times)
        requests.push_back({latitude, longitude, time});
      std::vector<PortableEnvironmentSample> samples;
      if (!owner->SampleEnvironment(*instance, requests, &samples, error))
        return false;
      availability->clear();
      availability->reserve(samples.size());
      for (const auto& sample : samples)
        availability->push_back(sample.available);
      return true;
    };
    const double latitude = std::isfinite(gLat) ? gLat : 53.0;
    const double longitude = std::isfinite(gLon) ? gLon : -5.0;
    instance->weather_routing_host =
        std::make_unique<PortableWeatherRoutingHost>(
            instance->owner->plugin_manager->GetParentFrame(),
            instance->runtime, instance->runtime_mutex, instance->package_root,
            std::move(summary), std::move(waypoints), std::move(vessel),
            std::move(cursor), std::move(displayed_time), std::move(preflight),
            latitude, longitude);
  }
  wxString error;
  return instance->weather_routing_host->Show(&error) ? 0 : -2;
}

bool PortablePluginManager::Impl::SampleEnvironment(
    Instance& consumer, const std::vector<PortableEnvironmentRequest>& requests,
    std::vector<PortableEnvironmentSample>* results, wxString* error) {
  if (!HasPermission(consumer, "environment.consume")) {
    if (error) *error = "environment.consume permission was not granted";
    return false;
  }
  if (consumer.held_environment_host && consumer.held_environment_dataset) {
    return consumer.held_environment_host->SampleBatch(
        consumer.held_environment_dataset, requests, results, error);
  }
  wxString last_error =
      "no enabled portable environmental-data provider is available";
  for (const auto& provider : instances) {
    if (!provider->enabled || provider->failed ||
        !ProvidesCompatibleService(*provider, consumer,
                                   "org.opencpn.environment.provider") ||
        !provider->environmental_host)
      continue;
    std::vector<PortableEnvironmentSample> sampled;
    wxString provider_error;
    if (!provider->environmental_host->SampleBatch(requests, &sampled,
                                                   &provider_error)) {
      if (!provider_error.empty()) last_error = provider_error;
      continue;
    }
    if (sampled.size() != requests.size()) {
      last_error = "environmental-data provider returned the wrong batch size";
      continue;
    }
    *results = std::move(sampled);
    return true;
  }
  if (error) *error = last_error;
  return false;
}

int32_t PortablePluginManager::Impl::EnvironmentSampleBatch(
    void* user_data, const ocpn_portable_environment_sample_request* requests,
    size_t request_count, ocpn_portable_environment_sample* results,
    size_t result_count, char* error, size_t error_capacity) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner || request_count != result_count ||
      (!requests && request_count) || (!results && result_count) ||
      request_count > 100000) {
    CopyUtf8("invalid environmental sample request", error, error_capacity);
    return -1;
  }
  std::vector<PortableEnvironmentRequest> input;
  input.reserve(request_count);
  for (size_t i = 0; i < request_count; ++i)
    input.push_back(
        {requests[i].latitude, requests[i].longitude, requests[i].unix_time});
  std::vector<PortableEnvironmentSample> sampled;
  wxString provider_error;
  if (instance->owner->SampleEnvironment(*instance, input, &sampled,
                                         &provider_error)) {
    for (size_t i = 0; i < result_count; ++i) {
      results[i] = {sampled[i].wind_u_knots,       sampled[i].wind_v_knots,
                    sampled[i].current_u_knots,    sampled[i].current_v_knots,
                    sampled[i].wave_height_metres, sampled[i].available};
    }
    return 0;
  }
  CopyUtf8(provider_error, error, error_capacity);
  wxLogWarning("Portable environmental sample batch failed for %s: %s",
               instance->id, provider_error);
  return -2;
}

void PortablePluginManager::Impl::RoutingProgress(void* user_data,
                                                  uint8_t percent,
                                                  const char* message,
                                                  size_t message_len) {
  auto* instance = static_cast<Instance*>(user_data);
  if (instance && instance->weather_routing_host)
    instance->weather_routing_host->ReportProgress(
        percent, FromUtf8(message, message_len));
}

uint8_t PortablePluginManager::Impl::RoutingCancelled(void* user_data) {
  auto* instance = static_cast<Instance*>(user_data);
  return instance && instance->weather_routing_host &&
                 instance->weather_routing_host->Cancelled()
             ? 1
             : 0;
}

int32_t PortablePluginManager::Impl::ChartsQuerySegments(
    void* user_data, const ocpn_portable_geo_segment* segments,
    size_t segment_count, ocpn_portable_chart_segment_result* results,
    size_t result_count) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner ||
      !instance->owner->HasPermission(*instance, "charts.coverage") ||
      (!segments && segment_count) || (!results && result_count) ||
      result_count != segment_count || segment_count > kChartSegmentLimit)
    return -1;
  auto valid_point = [](const ocpn_portable_geo_point& point) {
    return std::isfinite(point.latitude) && std::isfinite(point.longitude) &&
           point.latitude >= -90.0 && point.latitude <= 90.0 &&
           point.longitude >= -180.0 && point.longitude <= 180.0;
  };
  for (size_t i = 0; i < segment_count; ++i) {
    results[i] = {3, 0};
    if (!valid_point(segments[i].start) || !valid_point(segments[i].end))
      continue;
    static std::mutex gshhs_mutex;
    std::lock_guard<std::mutex> lock(gshhs_mutex);
    results[i].state = PlugIn_GSHHS_CrossesLand(segments[i].start.latitude,
                                                segments[i].start.longitude,
                                                segments[i].end.latitude,
                                                segments[i].end.longitude)
                           ? 1
                           : 0;
    results[i].charts_considered = 1;
  }
  return 0;
}

int32_t PortablePluginManager::Impl::NetworkGetToPrivate(
    void* user_data, const char* request_id, size_t request_id_len,
    const char* url, size_t url_len, const char* private_name,
    size_t private_name_len, uint64_t max_bytes) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner ||
      !instance->owner->HasPermission(*instance, "network.http") ||
      !instance->owner->HasPermission(*instance, "storage.private") ||
      max_bytes == 0 || max_bytes > kNetworkDownloadLimit)
    return -1;
  const wxString id = FromUtf8(request_id, request_id_len);
  const wxString address = FromUtf8(url, url_len);
  const wxString name = FromUtf8(private_name, private_name_len);
  if (!IsSafeName(id) || !IsSafeName(name) || address.length() > 4096 ||
      !(address.StartsWith("https://") || address.StartsWith("http://")) ||
      instance->jobs.count(id))
    return -2;

  auto job = std::make_shared<Job>();
  instance->jobs[id] = job;
  const std::string target =
      (instance->private_root + wxFILE_SEP_PATH + name).ToStdString();
  const std::string temporary = target + ".part-" + id.ToStdString();
  const std::string request_url = address.ToStdString();
  auto* owner = instance->owner;
  const wxString plugin_id = instance->id;
  auto alive_token = owner->alive;
  job->worker = std::thread([owner, alive_token, job, id, plugin_id, target,
                             temporary, request_url, max_bytes] {
    wxString failure;
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) {
      failure = "could not create private download file";
    } else {
      DownloadSink sink{file, job, max_bytes, 0};
      CURL* curl = curl_easy_init();
      if (!curl) {
        failure = "could not initialize host HTTP client";
      } else {
        curl_easy_setopt(curl, CURLOPT_URL, request_url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 128L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         "OpenCPN-portable-runtime/0.1");
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                         CURLPROTO_HTTP | CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                         CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteDownload);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, DownloadProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &sink);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        const CURLcode code = curl_easy_perform(curl);
        if (code != CURLE_OK)
          failure = job->cancel.load()
                        ? "HTTP request cancelled"
                        : wxString::FromUTF8(curl_easy_strerror(code));
        curl_easy_cleanup(curl);
      }
      if (std::fclose(file) != 0 && failure.empty())
        failure = "could not flush private download file";
    }
    if (failure.empty()) {
      std::remove(target.c_str());
      if (std::rename(temporary.c_str(), target.c_str()) != 0)
        failure = "could not publish private download file";
    }
    if (!failure.empty()) std::remove(temporary.c_str());
    wxTheApp->CallAfter([owner, alive_token, plugin_id, id, failure] {
      if (!alive_token->load()) return;
      owner->DeliverJobEvent(plugin_id, id, failure.empty() ? 1 : 3,
                             failure.empty() ? 100 : 0, failure);
    });
  });
  return 0;
}

int32_t PortablePluginManager::Impl::StoragePrivateRead(
    void* user_data, const char* private_name, size_t private_name_len,
    uint8_t* value, size_t value_capacity, size_t* value_len) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner || !value_len ||
      !instance->owner->HasPermission(*instance, "storage.private"))
    return -1;
  const wxString name = FromUtf8(private_name, private_name_len);
  if (!IsSafeName(name)) return -2;
  const std::filesystem::path path =
      (instance->private_root + wxFILE_SEP_PATH + name).ToStdString();
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) return -3;
  const auto bytes = std::filesystem::file_size(path, error);
  if (error || bytes > kPrivateReadLimit || bytes > value_capacity ||
      (bytes && !value))
    return -4;
  std::ifstream input(path, std::ios::binary);
  if (!input) return -5;
  input.read(reinterpret_cast<char*>(value),
             static_cast<std::streamsize>(bytes));
  if (!input) return -5;
  *value_len = static_cast<size_t>(bytes);
  return 0;
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
  if (instance.weather_routing_host) instance.weather_routing_host->Shutdown();
  if (instance.environmental_host) instance.environmental_host->Shutdown();
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

  const std::set<wxString> known_permissions = {"ui.commands",
                                                "navigation.position.read",
                                                "navigation.objects.read",
                                                "settings.read-write",
                                                "overlay.submit",
                                                "jobs.compute",
                                                "environment.datasets",
                                                "storage.user-selected",
                                                "network.providers",
                                                "helpers.environment.decode",
                                                "helpers.environment.generate",
                                                "charts.coverage",
                                                "network.http",
                                                "storage.private",
                                                "credentials.provider",
                                                "weather-routing.compute",
                                                "environment.consume",
                                                "navigation.routes.write"};
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
    if (manifest["surfaces"].IsObject()) {
      const wxString surface =
          manifest["surfaces"]["environment.viewer"].AsString();
      if (!surface.empty() && IsSafeRelativePath(surface))
        instance->environment_surface = surface;
    }
    instance->private_root = g_BasePlatform->GetPrivateDataDir() +
                             wxFILE_SEP_PATH + "portable-plugin-data" +
                             wxFILE_SEP_PATH + id;
    if (!wxDirExists(instance->private_root) &&
        !wxFileName::Mkdir(instance->private_root, 0700, wxPATH_MKDIR_FULL)) {
      wxLogError("Portable plugin %s private storage could not be created", id);
      more = directory.GetNext(&entry);
      continue;
    }
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
    const std::set<wxString> known_services = {
        "org.opencpn.environment.provider"};
    auto read_services = [&](const char* member, const char* version_member,
                             std::set<wxString>* names,
                             std::map<wxString, wxString>* versions) {
      wxJSONValue services = manifest[member];
      if (!services.IsArray()) return true;
      bool valid = true;
      for (int i = 0; i < services.Size(); ++i) {
        wxJSONValue service = services[i];
        const wxString interface = service["interface"].AsString();
        const wxString service_version = service[version_member].AsString();
        ocpn::portable::ServiceVersion parsed_service_version;
        const bool valid_version =
            wxString(version_member) == "version"
                ? ocpn::portable::ParseServiceVersion(
                      service_version.ToStdString(), &parsed_service_version)
                : ocpn::portable::IsServiceVersionRange(
                      service_version.ToStdString());
        if (!service.IsObject() || !known_services.count(interface) ||
            !valid_version || versions->count(interface)) {
          wxLogError("Portable plugin %s has invalid %s service metadata", id,
                     member);
          valid = false;
        } else {
          names->insert(interface);
          (*versions)[interface] = service_version;
        }
      }
      return valid;
    };
    if (!read_services("provides", "version", &instance->provides,
                       &instance->provided_service_versions) ||
        !read_services("requires", "range", &instance->required_services,
                       &instance->required_service_ranges)) {
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
    callbacks.open_weather_routing = OpenWeatherRouting;
    callbacks.environment_sample_batch = EnvironmentSampleBatch;
    callbacks.routing_progress = RoutingProgress;
    callbacks.routing_cancelled = RoutingCancelled;
    callbacks.charts_query_segments = ChartsQuerySegments;
    callbacks.network_get_to_private = NetworkGetToPrivate;
    callbacks.storage_private_read = StoragePrivateRead;
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
  for (const auto& instance : instances) {
    for (const auto& required : instance->required_services) {
      const auto provider = std::find_if(
          instances.begin(), instances.end(), [&](const auto& candidate) {
            return candidate->enabled && !candidate->failed &&
                   ProvidesCompatibleService(*candidate, *instance, required);
          });
      if (provider == instances.end())
        wxLogWarning(
            "Portable plugin %s has no compatible provider for runtime "
            "service %s (%s); "
            "dependent operations will fail closed",
            instance->id, required,
            instance->required_service_ranges[required]);
      else
        wxLogMessage("Portable plugin %s discovered service %s %s from %s",
                     instance->id, required,
                     (*provider)->provided_service_versions[required],
                     (*provider)->id);
    }
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
    if (instance->weather_routing_host)
      instance->weather_routing_host->Shutdown();
  }
  for (auto& instance : instances) {
    if (instance->environmental_host) instance->environmental_host->Shutdown();
  }
  for (auto& instance : instances) {
    std::lock_guard<std::mutex> lock(*instance->runtime_mutex);
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
  std::unique_lock<std::mutex> lock(*instance.runtime_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    wxLogMessage("Portable plugin %s is busy; action %s was not re-entered",
                 instance.id, found->second.action_id);
    return true;
  }
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
    if (instance->environmental_host &&
        instance->environmental_host->Render(dc, viewport))
      rendered = true;
    if (instance->weather_routing_host &&
        instance->weather_routing_host->Render(dc, viewport))
      rendered = true;
  }
  return rendered;
}

void PortablePluginManager::Impl::SetCursorPosition(double latitude,
                                                    double longitude) {
  if (std::isfinite(latitude) && std::isfinite(longitude) &&
      latitude >= -90.0 && latitude <= 90.0 && longitude >= -180.0 &&
      longitude <= 180.0) {
    cursor_position_available = true;
    cursor_latitude = latitude;
    cursor_longitude = longitude;
  }
  for (const auto& instance : instances) {
    if (instance->enabled && !instance->failed && instance->environmental_host)
      instance->environmental_host->SetCursorPosition(latitude, longitude);
  }
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

void PortablePluginManager::SetCursorPosition(double latitude,
                                              double longitude) {
  m_impl->SetCursorPosition(latitude, longitude);
}
