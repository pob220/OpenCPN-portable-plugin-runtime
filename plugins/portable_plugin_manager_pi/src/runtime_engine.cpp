#include "runtime_engine.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/log.h>
#include <wx/regex.h>
#include <wx/wfstream.h>

#include "ocpn_plugin.h"
#include "ocpn_portable_runtime.h"

namespace ppm {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kErrorCapacity = 4096;
constexpr std::size_t kSettingCapacity = 64 * 1024;
constexpr std::size_t kOverlayPointLimit = 1'000'000;
constexpr std::size_t kPrivateReadLimit = 8 * 1024 * 1024;

std::string Text(const char* value, std::size_t length) {
  return value && length ? std::string(value, length) : std::string();
}

void CopyError(const std::string& value, char* output, std::size_t capacity) {
  if (!output || capacity == 0) return;
  const std::size_t copied = std::min(value.size(), capacity - 1);
  if (copied != 0) std::memcpy(output, value.data(), copied);
  output[copied] = '\0';
}

bool IsSafeName(const std::string& value) {
  if (value.empty() || value.size() > 128) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isalnum(character) || character == '-' || character == '_' ||
           character == '.';
  });
}

bool IsPackageId(const wxString& value) {
  static const wxRegEx expression(
      "^[a-z0-9]+([.-][a-z0-9]+)+$");
  return value.length() <= 128 && expression.IsValid() &&
         expression.Matches(value);
}

bool IsSemanticVersion(const wxString& value) {
  static const wxRegEx expression(
      "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)"
      "(-[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?"
      "(\\+[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?$");
  return value.length() <= 128 && expression.IsValid() &&
         expression.Matches(value);
}

bool SafeRelativePath(const wxString& value) {
  if (value.empty() || value.Find('\\') != wxNOT_FOUND ||
      value.Find(':') != wxNOT_FOUND) {
    return false;
  }
  wxFileName path(value);
  if (path.IsAbsolute()) return false;
  for (const auto& directory : path.GetDirs()) {
    if (directory.empty() || directory == "." || directory == "..")
      return false;
  }
  return path.GetFullPath().Find("..") == wxNOT_FOUND;
}

std::string ReadSmallFile(const fs::path& path, std::size_t limit,
                          bool* okay) {
  *okay = false;
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error || size > limit) return {};
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  input.read(result.data(), static_cast<std::streamsize>(result.size()));
  if (!input && !result.empty()) return {};
  *okay = true;
  return result;
}

}  // namespace

class RuntimeEngine::Impl {
 public:
  struct Instance {
    Impl* owner = nullptr;
    std::string id;
    std::string name;
    std::string version;
    fs::path package_root;
    fs::path private_root;
    std::set<std::string> permissions;
    ocpn_portable_runtime* runtime = nullptr;
    mutable std::mutex runtime_mutex;
    bool enabled = false;
    bool failed = false;
    std::string diagnostic;
    std::map<std::string, OverlayScene> scenes;
  };

  Impl(std::string storage_root, RegisterAction register_action,
       StateChanged state_changed)
      : storage_root(std::move(storage_root)),
        register_action(std::move(register_action)),
        state_changed(std::move(state_changed)) {}

  ~Impl() { Shutdown(); }

  bool LoadInstalled(bool developer_mode);
  void Shutdown();
  bool HandleAction(const std::string& package_id,
                    const std::string& action_id);
  void Fail(Instance& instance, const std::string& operation,
            const std::string& diagnostic);
  bool Permitted(const Instance& instance,
                 const std::string& permission) const {
    return instance.permissions.count(permission) != 0;
  }

  static void Log(void* user_data, std::uint32_t level, const char* message,
                  std::size_t message_length);
  static std::int32_t RegisterRuntimeAction(
      void* user_data, const char* action_id, std::size_t action_id_length,
      const char* label, std::size_t label_length, const char* tooltip,
      std::size_t tooltip_length, const char* icon_resource,
      std::size_t icon_resource_length, std::uint32_t* host_action_id);
  static std::int32_t GetVesselPosition(
      void* user_data, double* latitude, double* longitude, double* cog,
      std::uint8_t* has_cog, double* sog, std::uint8_t* has_sog);
  static std::int32_t SettingGet(void* user_data, const char* key,
                                 std::size_t key_length, char* value,
                                 std::size_t value_capacity,
                                 std::size_t* value_length,
                                 std::uint8_t* found);
  static std::int32_t SettingSet(void* user_data, const char* key,
                                 std::size_t key_length, const char* value,
                                 std::size_t value_length);
  static std::int32_t SubmitPolyline(
      void* user_data, const char* scene_id, std::size_t scene_id_length,
      const ocpn_portable_geo_point* points, std::size_t point_count,
      ocpn_portable_overlay_style style);
  static std::int32_t ClearScene(void* user_data, const char* scene_id,
                                 std::size_t scene_id_length);
  static std::int32_t Unsupported(void*) { return -1; }
  static std::int32_t StartJob(void*, const char*, std::size_t,
                               std::uint32_t) {
    return -1;
  }
  static std::int32_t CancelJob(void*, const char*, std::size_t) { return -1; }
  static std::int32_t OpenEnvironmentalViewer(void*) { return -2; }
  static std::int32_t OpenWeatherRouting(void*) { return -2; }
  static std::int32_t EnvironmentSampleBatch(
      void*, const ocpn_portable_environment_sample_request*, std::size_t,
      ocpn_portable_environment_sample*, std::size_t, char* error,
      std::size_t error_capacity) {
    CopyError("environment provider adapter is not connected", error,
              error_capacity);
    return -2;
  }
  static void RoutingProgress(void*, std::uint8_t, const char*, std::size_t) {}
  static std::uint8_t RoutingCancelled(void*) { return 0; }
  static std::int32_t ChartsQuerySegments(
      void* user_data, const ocpn_portable_geo_segment* segments,
      std::size_t segment_count, ocpn_portable_chart_segment_result* results,
      std::size_t result_count);
  static std::int32_t NetworkGetToPrivate(
      void*, const char*, std::size_t, const char*, std::size_t, const char*,
      std::size_t, std::uint64_t) {
    return -1;
  }
  static std::int32_t StoragePrivateRead(
      void* user_data, const char* private_name,
      std::size_t private_name_length, std::uint8_t* value,
      std::size_t value_capacity, std::size_t* value_length);

  fs::path storage_root;
  RegisterAction register_action;
  StateChanged state_changed;
  std::vector<std::unique_ptr<Instance>> instances;
  bool stopped = false;
  bool position_valid = false;
  double latitude = 0.0;
  double longitude = 0.0;
  double cog = 0.0;
  double sog = 0.0;
  bool has_cog = false;
  bool has_sog = false;
};

void RuntimeEngine::Impl::Log(void* user_data, std::uint32_t level,
                              const char* message,
                              std::size_t message_length) {
  const auto* instance = static_cast<Instance*>(user_data);
  const wxString text = wxString::Format(
      "Portable package %s: %s", instance ? instance->id : "unknown",
      wxString::FromUTF8(Text(message, message_length)));
  if (level >= 3)
    wxLogError("%s", text);
  else if (level == 2)
    wxLogWarning("%s", text);
  else if (level == 0)
    wxLogDebug("%s", text);
  else
    wxLogMessage("%s", text);
}

std::int32_t RuntimeEngine::Impl::RegisterRuntimeAction(
    void* user_data, const char* action_id, std::size_t action_id_length,
    const char* label, std::size_t label_length, const char* tooltip,
    std::size_t tooltip_length, const char* icon_resource,
    std::size_t icon_resource_length, std::uint32_t* host_action_id) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner || !host_action_id ||
      !instance->owner->Permitted(*instance, "ui.commands")) {
    return -1;
  }
  RuntimeAction action;
  action.package_id = instance->id;
  action.action_id = Text(action_id, action_id_length);
  action.label = Text(label, label_length);
  action.tooltip = Text(tooltip, tooltip_length);
  const wxString resource =
      wxString::FromUTF8(Text(icon_resource, icon_resource_length));
  if (!IsSafeName(action.action_id) || action.label.empty()) return -2;
  if (!resource.empty()) {
    if (!SafeRelativePath(resource)) return -3;
    action.icon_path = (instance->package_root /
                        fs::path(resource.ToStdString()))
                           .lexically_normal()
                           .string();
    std::error_code error;
    if (!fs::is_regular_file(action.icon_path, error)) return -4;
  }
  return instance->owner->register_action(action, host_action_id);
}

std::int32_t RuntimeEngine::Impl::GetVesselPosition(
    void* user_data, double* latitude_out, double* longitude_out,
    double* cog_out, std::uint8_t* has_cog_out, double* sog_out,
    std::uint8_t* has_sog_out) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner || !latitude_out || !longitude_out ||
      !cog_out || !has_cog_out || !sog_out || !has_sog_out ||
      !instance->owner->Permitted(*instance, "navigation.position.read")) {
    return -1;
  }
  const auto& owner = *instance->owner;
  if (!owner.position_valid) return -2;
  *latitude_out = owner.latitude;
  *longitude_out = owner.longitude;
  *cog_out = owner.cog;
  *sog_out = owner.sog;
  *has_cog_out = owner.has_cog ? 1 : 0;
  *has_sog_out = owner.has_sog ? 1 : 0;
  return 0;
}

std::int32_t RuntimeEngine::Impl::SettingGet(
    void* user_data, const char* key, std::size_t key_length, char* value,
    std::size_t value_capacity, std::size_t* value_length,
    std::uint8_t* found) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner || !value_length || !found ||
      !instance->owner->Permitted(*instance, "settings.read-write")) {
    return -1;
  }
  const std::string name = Text(key, key_length);
  if (!IsSafeName(name)) return -2;
  const fs::path path = instance->private_root / "settings" / name;
  std::error_code error;
  if (!fs::exists(path, error)) {
    *found = 0;
    *value_length = 0;
    return 0;
  }
  bool okay = false;
  const std::string contents = ReadSmallFile(path, kSettingCapacity, &okay);
  if (!okay) return -3;
  *found = 1;
  *value_length = contents.size();
  if (contents.size() > value_capacity || (!value && !contents.empty()))
    return -4;
  if (!contents.empty()) std::memcpy(value, contents.data(), contents.size());
  return 0;
}

std::int32_t RuntimeEngine::Impl::SettingSet(
    void* user_data, const char* key, std::size_t key_length,
    const char* value, std::size_t value_length) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner ||
      !instance->owner->Permitted(*instance, "settings.read-write") ||
      value_length > kSettingCapacity || (value_length != 0 && !value)) {
    return -1;
  }
  const std::string name = Text(key, key_length);
  if (!IsSafeName(name)) return -2;
  std::error_code error;
  const fs::path directory = instance->private_root / "settings";
  fs::create_directories(directory, error);
  if (error) return -3;
  const fs::path target = directory / name;
  const fs::path temporary = directory / (name + ".tmp");
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return -3;
    output.write(value, static_cast<std::streamsize>(value_length));
    if (!output) return -3;
  }
  fs::rename(temporary, target, error);
  if (error) {
    fs::remove(target, error);
    error.clear();
    fs::rename(temporary, target, error);
  }
  return error ? -3 : 0;
}

std::int32_t RuntimeEngine::Impl::SubmitPolyline(
    void* user_data, const char* scene_id, std::size_t scene_id_length,
    const ocpn_portable_geo_point* points, std::size_t point_count,
    ocpn_portable_overlay_style style) {
  auto* instance = static_cast<Instance*>(user_data);
  const std::string id = Text(scene_id, scene_id_length);
  if (!instance || !instance->owner ||
      !instance->owner->Permitted(*instance, "overlay.submit") ||
      !IsSafeName(id) || !points || point_count < 2 ||
      point_count > kOverlayPointLimit || !std::isfinite(style.width_pixels) ||
      style.width_pixels < 0.5F || style.width_pixels > 32.0F) {
    return -1;
  }
  OverlayScene scene;
  scene.package_id = instance->id;
  scene.scene_id = id;
  scene.red = style.red;
  scene.green = style.green;
  scene.blue = style.blue;
  scene.alpha = style.alpha;
  scene.width_pixels = style.width_pixels;
  scene.points.reserve(point_count);
  for (std::size_t index = 0; index < point_count; ++index) {
    if (!std::isfinite(points[index].latitude) ||
        !std::isfinite(points[index].longitude) ||
        points[index].latitude < -90.0 || points[index].latitude > 90.0 ||
        points[index].longitude < -180.0 ||
        points[index].longitude > 180.0) {
      return -2;
    }
    scene.points.push_back({points[index].latitude, points[index].longitude});
  }
  instance->scenes[id] = std::move(scene);
  instance->owner->state_changed();
  return 0;
}

std::int32_t RuntimeEngine::Impl::ClearScene(void* user_data,
                                             const char* scene_id,
                                             std::size_t scene_id_length) {
  auto* instance = static_cast<Instance*>(user_data);
  const std::string id = Text(scene_id, scene_id_length);
  if (!instance || !instance->owner || !IsSafeName(id)) return -1;
  instance->scenes.erase(id);
  instance->owner->state_changed();
  return 0;
}

std::int32_t RuntimeEngine::Impl::ChartsQuerySegments(
    void* user_data, const ocpn_portable_geo_segment* segments,
    std::size_t segment_count, ocpn_portable_chart_segment_result* results,
    std::size_t result_count) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner || !segments || !results ||
      result_count != segment_count || segment_count > 10'000 ||
      !instance->owner->Permitted(*instance, "charts.coverage")) {
    return -1;
  }
  for (std::size_t index = 0; index < segment_count; ++index) {
    const auto& segment = segments[index];
    results[index] = {
        PlugIn_GSHHS_CrossesLand(
            segment.start.latitude, segment.start.longitude,
            segment.end.latitude, segment.end.longitude)
            ? 1U
            : 0U,
        1U};
  }
  return 0;
}

std::int32_t RuntimeEngine::Impl::StoragePrivateRead(
    void* user_data, const char* private_name,
    std::size_t private_name_length, std::uint8_t* value,
    std::size_t value_capacity, std::size_t* value_length) {
  auto* instance = static_cast<Instance*>(user_data);
  const std::string name = Text(private_name, private_name_length);
  if (!instance || !instance->owner || !value_length || !IsSafeName(name) ||
      !instance->owner->Permitted(*instance, "storage.private")) {
    return -1;
  }
  bool okay = false;
  const std::string contents =
      ReadSmallFile(instance->private_root / name, kPrivateReadLimit, &okay);
  if (!okay) return -2;
  *value_length = contents.size();
  if (contents.size() > value_capacity || (!value && !contents.empty()))
    return -3;
  if (!contents.empty()) std::memcpy(value, contents.data(), contents.size());
  return 0;
}

bool RuntimeEngine::Impl::LoadInstalled(bool developer_mode) {
  if (stopped) return false;
  const fs::path packages_root = storage_root / "packages";
  std::error_code filesystem_error;
  fs::create_directories(packages_root, filesystem_error);
  fs::create_directories(storage_root / "data", filesystem_error);
  if (filesystem_error) {
    wxLogError("PPM runtime store is unavailable: %s",
               filesystem_error.message());
    return false;
  }

  const std::set<std::string> known_permissions = {
      "ui.commands",          "navigation.position.read",
      "navigation.objects.read", "settings.read-write",
      "overlay.submit",       "jobs.compute",
      "environment.datasets", "storage.user-selected",
      "network.providers",    "helpers.environment.decode",
      "helpers.environment.generate", "charts.coverage",
      "network.http",         "storage.private",
      "credentials.provider", "weather-routing.compute",
      "environment.consume",  "navigation.routes.write"};

  std::vector<fs::path> roots;
  for (const auto& item : fs::directory_iterator(packages_root)) {
    if (item.is_directory()) roots.push_back(item.path());
  }
  std::sort(roots.begin(), roots.end());
  for (const auto& root : roots) {
    const fs::path manifest_path = root / "manifest.json";
    wxFileInputStream input(wxString::FromUTF8(manifest_path.string()));
    wxJSONValue manifest;
    wxJSONReader reader;
    if (!input.IsOk() || reader.Parse(input, &manifest) != 0 ||
        !manifest.IsObject()) {
      wxLogError("PPM invalid installed manifest: %s",
                 manifest_path.string());
      continue;
    }
    const wxString id = manifest["id"].AsString();
    const wxString name = manifest["name"].AsString();
    const wxString version = manifest["version"].AsString();
    const wxString component = manifest["component"].AsString();
    const bool development = manifest["development"].AsBool();
    const bool typed =
        manifest["format_version"].IsInt() &&
        manifest["format_version"].AsInt() == 1 && manifest["id"].IsString() &&
        manifest["name"].IsString() && manifest["version"].IsString() &&
        manifest["component"].IsString() && manifest["runtime"].IsString() &&
        manifest["portable_api"].IsString() &&
        manifest["permissions"].IsArray() &&
        manifest["development"].IsBool();
    if (!typed || !IsPackageId(id) || name.empty() ||
        !IsSemanticVersion(version) || !SafeRelativePath(component) ||
        manifest["runtime"].AsString() != ">=0.1.0 <0.2.0" ||
        manifest["portable_api"].AsString() != ">=0.1.0 <0.2.0") {
      wxLogError("PPM incompatible installed package at %s", root.string());
      continue;
    }
    if (development && !developer_mode) {
      wxLogWarning("PPM skipped development package %s", id);
      continue;
    }
    auto instance = std::make_unique<Instance>();
    instance->owner = this;
    instance->id = id.ToStdString();
    instance->name = name.ToStdString();
    instance->version = version.ToStdString();
    instance->package_root = root;
    instance->private_root = storage_root / "data" / instance->id;
    fs::create_directories(instance->private_root, filesystem_error);
    if (filesystem_error) {
      instance->diagnostic = filesystem_error.message();
      instances.push_back(std::move(instance));
      continue;
    }
    bool permission_error = false;
    wxJSONValue requested = manifest["permissions"];
    for (int index = 0; index < requested.Size(); ++index) {
      if (!requested[index].IsString()) {
        permission_error = true;
        break;
      }
      const std::string permission = requested[index].AsString().ToStdString();
      if (known_permissions.count(permission) == 0) {
        permission_error = true;
        instance->diagnostic = "unknown permission: " + permission;
        break;
      }
      instance->permissions.insert(permission);
    }
    const fs::path component_path =
        (root / component.ToStdString()).lexically_normal();
    if (permission_error || !fs::is_regular_file(component_path)) {
      if (instance->diagnostic.empty())
        instance->diagnostic = "component or permissions are invalid";
      instances.push_back(std::move(instance));
      continue;
    }

    ocpn_portable_host_callbacks callbacks{};
    callbacks.abi_version = OCPN_PORTABLE_HOST_ABI_VERSION;
    callbacks.user_data = instance.get();
    callbacks.log = Log;
    callbacks.register_action = RegisterRuntimeAction;
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
    std::array<char, kErrorCapacity> error{};
    instance->runtime = ocpn_portable_runtime_create(
        component_path.c_str(), &callbacks, error.data(), error.size());
    if (!instance->runtime ||
        ocpn_portable_runtime_initialize(
            instance->runtime, instance->id.data(), instance->id.size(),
            instance->name.data(), instance->name.size(),
            instance->version.data(), instance->version.size(), error.data(),
            error.size()) != 0 ||
        ocpn_portable_runtime_enable(instance->runtime, error.data(),
                                     error.size()) != 0) {
      instance->diagnostic = error.data();
      if (instance->runtime) {
        ocpn_portable_runtime_destroy(instance->runtime);
        instance->runtime = nullptr;
      }
      instances.push_back(std::move(instance));
      continue;
    }
    instance->enabled = true;
    wxLogMessage("PPM package-enabled id=%s version=%s", instance->id,
                 instance->version);
    instances.push_back(std::move(instance));
  }
  state_changed();
  return true;
}

void RuntimeEngine::Impl::Fail(Instance& instance,
                               const std::string& operation,
                               const std::string& diagnostic) {
  instance.failed = true;
  instance.enabled = false;
  instance.diagnostic = operation + ": " + diagnostic;
  instance.scenes.clear();
  wxLogError("PPM package-failed id=%s diagnostic=%s", instance.id,
             instance.diagnostic);
  state_changed();
}

bool RuntimeEngine::Impl::HandleAction(const std::string& package_id,
                                       const std::string& action_id) {
  const auto item = std::find_if(
      instances.begin(), instances.end(), [&](const auto& candidate) {
        return candidate->id == package_id;
      });
  if (item == instances.end()) return false;
  Instance& instance = **item;
  if (!instance.enabled || instance.failed || !instance.runtime) return true;
  std::unique_lock<std::mutex> lock(instance.runtime_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    wxLogWarning("PPM package-busy id=%s action=%s", package_id, action_id);
    return true;
  }
  std::array<char, kErrorCapacity> error{};
  if (ocpn_portable_runtime_on_action(
          instance.runtime, action_id.data(), action_id.size(), error.data(),
          error.size()) != 0) {
    Fail(instance, "action " + action_id, error.data());
  }
  return true;
}

void RuntimeEngine::Impl::Shutdown() {
  if (stopped) return;
  stopped = true;
  for (auto& instance : instances) {
    std::lock_guard<std::mutex> lock(instance->runtime_mutex);
    if (!instance->runtime) continue;
    std::array<char, kErrorCapacity> error{};
    if (instance->enabled &&
        ocpn_portable_runtime_disable(instance->runtime, error.data(),
                                      error.size()) != 0) {
      wxLogWarning("PPM package-disable-failed id=%s diagnostic=%s",
                   instance->id, error.data());
    }
    ocpn_portable_runtime_destroy(instance->runtime);
    instance->runtime = nullptr;
    instance->enabled = false;
    instance->scenes.clear();
  }
  state_changed();
}

RuntimeEngine::RuntimeEngine(std::string storage_root,
                             RegisterAction register_action,
                             StateChanged state_changed)
    : impl_(std::make_unique<Impl>(storage_root, std::move(register_action),
                                  std::move(state_changed))),
      storage_root_(std::move(storage_root)) {}

RuntimeEngine::~RuntimeEngine() = default;

bool RuntimeEngine::LoadInstalled(bool developer_mode) {
  return impl_->LoadInstalled(developer_mode);
}

void RuntimeEngine::Shutdown() { impl_->Shutdown(); }

bool RuntimeEngine::HandleAction(const std::string& package_id,
                                 const std::string& action_id) {
  return impl_->HandleAction(package_id, action_id);
}

void RuntimeEngine::SetPositionFix(const PlugIn_Position_Fix_Ex& fix) {
  impl_->position_valid =
      std::isfinite(fix.Lat) && std::isfinite(fix.Lon) &&
      fix.Lat >= -90.0 && fix.Lat <= 90.0 && fix.Lon >= -180.0 &&
      fix.Lon <= 180.0;
  impl_->latitude = fix.Lat;
  impl_->longitude = fix.Lon;
  impl_->has_cog = std::isfinite(fix.Cog);
  impl_->has_sog = std::isfinite(fix.Sog);
  impl_->cog = impl_->has_cog ? fix.Cog : 0.0;
  impl_->sog = impl_->has_sog ? fix.Sog : 0.0;
}

std::vector<PackageSnapshot> RuntimeEngine::Packages() const {
  std::vector<PackageSnapshot> result;
  result.reserve(impl_->instances.size());
  for (const auto& instance : impl_->instances) {
    result.push_back(
        {instance->id, instance->name, instance->version,
         instance->failed ? "Failed"
         : instance->enabled ? "Enabled"
                             : "Disabled",
         instance->diagnostic});
  }
  return result;
}

std::vector<OverlayScene> RuntimeEngine::Scenes() const {
  std::vector<OverlayScene> result;
  for (const auto& instance : impl_->instances) {
    if (!instance->enabled || instance->failed) continue;
    for (const auto& item : instance->scenes) result.push_back(item.second);
  }
  return result;
}

}  // namespace ppm
