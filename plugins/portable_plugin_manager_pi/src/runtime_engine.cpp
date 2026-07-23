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
#include "permission_store.h"

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
    fs::path component_path;
    fs::path private_root;
    std::set<std::string> requested_permissions;
    std::set<std::string> permissions;
    ocpn_portable_runtime* runtime = nullptr;
    mutable std::mutex runtime_mutex;
    bool enabled = false;
    bool failed = false;
    bool loadable = false;
    std::string diagnostic;
    std::vector<RuntimeAction> registered_actions;
    std::map<std::string, OverlayScene> scenes;
  };

  Impl(std::string storage_root, RegisterAction register_action,
       RemoveActions remove_actions, StateChanged state_changed)
      : storage_root(std::move(storage_root)),
        register_action(std::move(register_action)),
        remove_actions(std::move(remove_actions)),
        state_changed(std::move(state_changed)) {}

  ~Impl() { Shutdown(); }

  bool LoadInstalled(bool developer_mode);
  bool RefreshPackage(const std::string& package_id, bool developer_mode,
                      std::string* diagnostic);
  bool Enable(const std::string& package_id, std::string* diagnostic);
  bool SetGrantedPermissions(
      const std::string& package_id,
      const std::vector<std::string>& granted_permissions,
      std::string* diagnostic);
  std::vector<std::string> RequestedPermissions(
      const std::string& package_id) const;
  bool Disable(const std::string& package_id, std::string* diagnostic);
  bool Unload(const std::string& package_id, std::string* diagnostic);
  bool IsEnabled(const std::string& package_id) const;
  void Shutdown();
  bool HandleAction(const std::string& package_id,
                    const std::string& action_id);
  void Fail(Instance& instance, const std::string& operation,
            const std::string& diagnostic);
  Instance* Find(const std::string& package_id);
  const Instance* Find(const std::string& package_id) const;
  bool LoadRoot(const fs::path& root, bool developer_mode, bool activate,
                std::string* diagnostic);
  bool Start(Instance& instance, std::string* diagnostic);
  bool Stop(Instance& instance, bool destroy, std::string* diagnostic);
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
  RemoveActions remove_actions;
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
  const std::int32_t result =
      instance->owner->register_action(action, host_action_id);
  if (result == 0) instance->registered_actions.push_back(std::move(action));
  return result;
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

RuntimeEngine::Impl::Instance* RuntimeEngine::Impl::Find(
    const std::string& package_id) {
  const auto item =
      std::find_if(instances.begin(), instances.end(), [&](const auto& value) {
        return value->id == package_id;
      });
  return item == instances.end() ? nullptr : item->get();
}

const RuntimeEngine::Impl::Instance* RuntimeEngine::Impl::Find(
    const std::string& package_id) const {
  const auto item =
      std::find_if(instances.begin(), instances.end(), [&](const auto& value) {
        return value->id == package_id;
      });
  return item == instances.end() ? nullptr : item->get();
}

bool RuntimeEngine::Impl::Start(Instance& instance,
                                std::string* diagnostic) {
  if (stopped) {
    if (diagnostic) *diagnostic = "runtime engine is shutting down";
    return false;
  }
  if (!instance.loadable) {
    if (diagnostic)
      *diagnostic = instance.diagnostic.empty()
                        ? "package failed static validation"
                        : instance.diagnostic;
    return false;
  }
  if (instance.permissions != instance.requested_permissions) {
    if (diagnostic) *diagnostic = "permission approval is required";
    instance.diagnostic = "permission approval is required";
    return false;
  }
  std::lock_guard<std::mutex> lock(instance.runtime_mutex);
  std::array<char, kErrorCapacity> error{};
  if (!instance.runtime) {
    instance.registered_actions.clear();
    ocpn_portable_host_callbacks callbacks{};
    callbacks.abi_version = OCPN_PORTABLE_HOST_ABI_VERSION;
    callbacks.user_data = &instance;
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
    instance.runtime = ocpn_portable_runtime_create(
        instance.component_path.c_str(), &callbacks, error.data(), error.size());
    if (!instance.runtime ||
        ocpn_portable_runtime_initialize(
            instance.runtime, instance.id.data(), instance.id.size(),
            instance.name.data(), instance.name.size(), instance.version.data(),
            instance.version.size(), error.data(), error.size()) != 0) {
      remove_actions(instance.id);
      if (instance.runtime)
        ocpn_portable_runtime_destroy(instance.runtime);
      instance.runtime = nullptr;
      instance.registered_actions.clear();
      instance.failed = true;
      instance.diagnostic =
          error[0] ? error.data() : "runtime initialization failed";
      if (diagnostic) *diagnostic = instance.diagnostic;
      return false;
    }
  } else {
    for (const auto& action : instance.registered_actions) {
      std::uint32_t ignored = 0;
      if (register_action(action, &ignored) != 0) {
        remove_actions(instance.id);
        instance.diagnostic = "could not restore package toolbar actions";
        if (diagnostic) *diagnostic = instance.diagnostic;
        return false;
      }
    }
  }
  if (ocpn_portable_runtime_enable(instance.runtime, error.data(),
                                   error.size()) != 0) {
    remove_actions(instance.id);
    ocpn_portable_runtime_destroy(instance.runtime);
    instance.runtime = nullptr;
    instance.registered_actions.clear();
    instance.failed = true;
    instance.diagnostic =
        error[0] ? error.data() : "runtime enable failed";
    if (diagnostic) *diagnostic = instance.diagnostic;
    return false;
  }
  instance.enabled = true;
  instance.failed = false;
  instance.diagnostic.clear();
  wxLogMessage("PPM package-enabled id=%s version=%s", instance.id,
               instance.version);
  return true;
}

bool RuntimeEngine::Impl::Stop(Instance& instance, bool destroy,
                               std::string* diagnostic) {
  std::lock_guard<std::mutex> lock(instance.runtime_mutex);
  bool okay = true;
  std::array<char, kErrorCapacity> error{};
  if (instance.runtime && instance.enabled &&
      ocpn_portable_runtime_disable(instance.runtime, error.data(),
                                    error.size()) != 0) {
    okay = false;
    instance.diagnostic =
        error[0] ? error.data() : "runtime disable failed";
    wxLogWarning("PPM package-disable-failed id=%s diagnostic=%s",
                 instance.id, instance.diagnostic);
    destroy = true;
  }
  instance.enabled = false;
  instance.scenes.clear();
  remove_actions(instance.id);
  if (destroy && instance.runtime) {
    ocpn_portable_runtime_destroy(instance.runtime);
    instance.runtime = nullptr;
    instance.registered_actions.clear();
  }
  if (okay && !instance.failed) instance.diagnostic.clear();
  if (!okay && diagnostic) *diagnostic = instance.diagnostic;
  return okay;
}

bool RuntimeEngine::Impl::LoadRoot(const fs::path& root, bool developer_mode,
                                   bool activate,
                                   std::string* diagnostic) {
  const fs::path manifest_path = root / "manifest.json";
  wxFileInputStream input(wxString::FromUTF8(manifest_path.string()));
  wxJSONValue manifest;
  wxJSONReader reader;
  if (!input.IsOk() || reader.Parse(input, &manifest) != 0 ||
      !manifest.IsObject()) {
    if (diagnostic) *diagnostic = "invalid installed manifest";
    wxLogError("PPM invalid installed manifest: %s", manifest_path.string());
    return false;
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
      manifest["portable_api"].AsString() != ">=0.1.0 <0.2.0" ||
      root.filename() != id.ToStdString()) {
    if (diagnostic) *diagnostic = "incompatible installed package";
    wxLogError("PPM incompatible installed package at %s", root.string());
    return false;
  }

  auto instance = std::make_unique<Instance>();
  instance->owner = this;
  instance->id = id.ToStdString();
  instance->name = name.ToStdString();
  instance->version = version.ToStdString();
  instance->package_root = root;
  instance->component_path =
      (root / component.ToStdString()).lexically_normal();
  instance->private_root = storage_root / "data" / instance->id;
  if (development && !developer_mode) {
    instance->failed = true;
    instance->diagnostic =
        "Development package blocked because developer mode is off";
    if (diagnostic) *diagnostic = instance->diagnostic;
    instances.push_back(std::move(instance));
    return false;
  }

  wxJSONValue requested = manifest["permissions"];
  for (int index = 0; index < requested.Size(); ++index) {
    if (!requested[index].IsString()) {
      instance->failed = true;
      instance->diagnostic = "manifest permission is not a string";
      break;
    }
    const std::string permission = requested[index].AsString().ToStdString();
    if (!FindPermission(permission)) {
      instance->failed = true;
      instance->diagnostic = "unknown permission: " + permission;
      break;
    }
    instance->requested_permissions.insert(permission);
  }
  std::error_code filesystem_error;
  fs::create_directories(instance->private_root, filesystem_error);
  if (filesystem_error) {
    instance->failed = true;
    instance->diagnostic = filesystem_error.message();
  } else if (!fs::is_regular_file(instance->component_path)) {
    instance->failed = true;
    instance->diagnostic = "declared component is missing";
  }
  Instance* loaded = instance.get();
  if (!instance->failed) instance->loadable = true;
  instances.push_back(std::move(instance));
  if (loaded->failed) {
    if (diagnostic) *diagnostic = loaded->diagnostic;
    return false;
  }
  if (activate && !Start(*loaded, diagnostic)) return false;
  return true;
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
  std::vector<fs::path> roots;
  for (const auto& item :
       fs::directory_iterator(packages_root, filesystem_error)) {
    if (item.is_directory()) roots.push_back(item.path());
  }
  if (filesystem_error) {
    wxLogError("PPM package scan failed: %s", filesystem_error.message());
    return false;
  }
  std::sort(roots.begin(), roots.end());
  bool okay = true;
  for (const auto& root : roots) {
    std::string diagnostic;
    if (!LoadRoot(root, developer_mode, false, &diagnostic)) okay = false;
  }
  state_changed();
  return okay;
}

bool RuntimeEngine::Impl::RefreshPackage(const std::string& package_id,
                                         bool developer_mode,
                                         std::string* diagnostic) {
  if (stopped || !IsSafeName(package_id)) {
    if (diagnostic) *diagnostic = "invalid package or stopped runtime";
    return false;
  }
  for (auto item = instances.begin(); item != instances.end(); ++item) {
    if ((*item)->id != package_id) continue;
    std::string ignored;
    Stop(**item, true, &ignored);
    instances.erase(item);
    break;
  }
  const fs::path root = storage_root / "packages" / package_id;
  std::error_code error;
  if (!fs::exists(root, error)) {
    state_changed();
    return !error;
  }
  const bool result =
      LoadRoot(root, developer_mode, false, diagnostic);
  state_changed();
  return result;
}

bool RuntimeEngine::Impl::Enable(const std::string& package_id,
                                 std::string* diagnostic) {
  Instance* instance = Find(package_id);
  if (!instance) {
    if (diagnostic) *diagnostic = "package is not loaded";
    return false;
  }
  if (instance->enabled) return true;
  const bool result = Start(*instance, diagnostic);
  state_changed();
  return result;
}

bool RuntimeEngine::Impl::SetGrantedPermissions(
    const std::string& package_id,
    const std::vector<std::string>& granted_permissions,
    std::string* diagnostic) {
  Instance* instance = Find(package_id);
  if (!instance) {
    if (diagnostic) *diagnostic = "package is not loaded";
    return false;
  }
  std::set<std::string> granted;
  for (const auto& permission : granted_permissions) {
    if (!FindPermission(permission) ||
        instance->requested_permissions.count(permission) == 0 ||
        !granted.insert(permission).second) {
      if (diagnostic)
        *diagnostic = "grant contains an unknown, unrequested or duplicate "
                      "permission";
      return false;
    }
  }
  if (granted != instance->requested_permissions) {
    if (diagnostic)
      *diagnostic = "grant does not cover all requested permissions";
    return false;
  }
  instance->permissions = std::move(granted);
  if (!instance->failed) instance->diagnostic.clear();
  state_changed();
  return true;
}

std::vector<std::string> RuntimeEngine::Impl::RequestedPermissions(
    const std::string& package_id) const {
  const Instance* instance = Find(package_id);
  if (!instance) return {};
  return {instance->requested_permissions.begin(),
          instance->requested_permissions.end()};
}

bool RuntimeEngine::Impl::Disable(const std::string& package_id,
                                  std::string* diagnostic) {
  Instance* instance = Find(package_id);
  if (!instance) {
    if (diagnostic) *diagnostic = "package is not loaded";
    return false;
  }
  const bool result = Stop(*instance, false, diagnostic);
  state_changed();
  return result;
}

bool RuntimeEngine::Impl::Unload(const std::string& package_id,
                                 std::string* diagnostic) {
  Instance* instance = Find(package_id);
  if (!instance) {
    if (diagnostic) *diagnostic = "package is not loaded";
    return false;
  }
  const bool result = Stop(*instance, true, diagnostic);
  state_changed();
  return result;
}

bool RuntimeEngine::Impl::IsEnabled(const std::string& package_id) const {
  const Instance* instance = Find(package_id);
  return instance && instance->enabled && !instance->failed;
}

void RuntimeEngine::Impl::Fail(Instance& instance,
                               const std::string& operation,
                               const std::string& diagnostic) {
  instance.failed = true;
  instance.enabled = false;
  instance.diagnostic = operation + ": " + diagnostic;
  instance.scenes.clear();
  remove_actions(instance.id);
  if (instance.runtime) {
    ocpn_portable_runtime_destroy(instance.runtime);
    instance.runtime = nullptr;
  }
  instance.registered_actions.clear();
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
    std::string ignored;
    Stop(*instance, true, &ignored);
  }
  state_changed();
}

RuntimeEngine::RuntimeEngine(std::string storage_root,
                             RegisterAction register_action,
                             RemoveActions remove_actions,
                             StateChanged state_changed)
    : impl_(std::make_unique<Impl>(storage_root, std::move(register_action),
                                  std::move(remove_actions),
                                  std::move(state_changed))),
      storage_root_(std::move(storage_root)) {}

RuntimeEngine::~RuntimeEngine() = default;

bool RuntimeEngine::LoadInstalled(bool developer_mode) {
  return impl_->LoadInstalled(developer_mode);
}

bool RuntimeEngine::RefreshPackage(const std::string& package_id,
                                   bool developer_mode,
                                   std::string* diagnostic) {
  return impl_->RefreshPackage(package_id, developer_mode, diagnostic);
}

bool RuntimeEngine::Enable(const std::string& package_id,
                           std::string* diagnostic) {
  return impl_->Enable(package_id, diagnostic);
}

bool RuntimeEngine::SetGrantedPermissions(
    const std::string& package_id,
    const std::vector<std::string>& granted_permissions,
    std::string* diagnostic) {
  return impl_->SetGrantedPermissions(package_id, granted_permissions,
                                      diagnostic);
}

std::vector<std::string> RuntimeEngine::RequestedPermissions(
    const std::string& package_id) const {
  return impl_->RequestedPermissions(package_id);
}

bool RuntimeEngine::Disable(const std::string& package_id,
                            std::string* diagnostic) {
  return impl_->Disable(package_id, diagnostic);
}

bool RuntimeEngine::Unload(const std::string& package_id,
                           std::string* diagnostic) {
  return impl_->Unload(package_id, diagnostic);
}

bool RuntimeEngine::IsEnabled(const std::string& package_id) const {
  return impl_->IsEnabled(package_id);
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
         : instance->runtime ? "Disabled"
                             : "Unloaded",
         "Unknown",
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
