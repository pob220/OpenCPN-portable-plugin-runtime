#include "runtime_engine.h"

#include <algorithm>
#include <atomic>
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
#include <wx/sstream.h>
#include <wx/wfstream.h>

#include "ocpn_plugin.h"
#include "ocpn_portable_runtime.h"
#include "declarative_ui.h"
#include "permission_store.h"
#include "serial_executor.h"

namespace ppm {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kErrorCapacity = 4096;
constexpr std::size_t kSettingCapacity = 64 * 1024;
constexpr std::size_t kOverlayPointLimit = 1'000'000;
constexpr std::size_t kPrivateReadLimit = 8 * 1024 * 1024;
constexpr std::size_t kSurfaceDocumentLimit = 1024 * 1024;
constexpr std::size_t kSurfaceStateLimit = 64 * 1024;

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
    mutable std::mutex state_mutex;
    SerialExecutor executor;
    std::atomic_bool enabled{false};
    std::atomic_bool failed{false};
    std::atomic_uint64_t enable_count{0};
    std::atomic_uint64_t disable_count{0};
    bool loadable = false;
    std::string diagnostic;
    std::vector<RuntimeAction> registered_actions;
    std::map<std::string, OverlayScene> scenes;
    std::map<std::string, DeclarativeSurface> surfaces;
  };

  Impl(std::string storage_root, RegisterAction register_action,
       RemoveActions remove_actions, StateChanged state_changed,
       UiDispatch ui_dispatch)
      : storage_root(std::move(storage_root)),
        register_action(std::move(register_action)),
        remove_actions(std::move(remove_actions)),
        state_changed(std::move(state_changed)),
        ui_dispatch(std::move(ui_dispatch)) {}

  ~Impl() { Shutdown(); }

  bool LoadInstalled(bool developer_mode);
  void SetSurfaceOpenedCallback(SurfaceOpened callback) {
    surface_opened = std::move(callback);
  }
  void SetSurfaceResponseCallback(SurfaceResponse callback) {
    surface_response = std::move(callback);
  }
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
  bool HandleSurfaceEvent(const std::string& package_id,
                          const std::string& surface_id,
                          const std::string& control_id,
                          const std::string& value_json);
  bool WaitForIdle(const std::string& package_id,
                   std::chrono::milliseconds timeout);
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
  void Publish(std::function<void()> task) {
    if (ui_dispatch)
      ui_dispatch(std::move(task));
    else
      task();
  }
  void PublishStateChanged() {
    const auto changed = state_changed;
    Publish([changed]() { changed(); });
  }
  void PublishRemoveActions(const std::string& package_id) {
    const auto remove = remove_actions;
    Publish([remove, package_id]() { remove(package_id); });
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
  static std::int32_t OpenEnvironmentalViewer(void* user_data) {
    return OpenNamedSurface(user_data, "environment.viewer");
  }
  static std::int32_t OpenWeatherRouting(void* user_data) {
    return OpenNamedSurface(user_data, "routing.workbench");
  }
  static std::int32_t OpenNamedSurface(void* user_data,
                                       const std::string& surface_id);
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
  UiDispatch ui_dispatch;
  SurfaceOpened surface_opened;
  SurfaceResponse surface_response;
  std::vector<std::unique_ptr<Instance>> instances;
  bool stopped = false;
  mutable std::mutex position_mutex;
  bool position_valid = false;
  double latitude = 0.0;
  double longitude = 0.0;
  double cog = 0.0;
  double sog = 0.0;
  bool has_cog = false;
  bool has_sog = false;
};

std::int32_t RuntimeEngine::Impl::OpenNamedSurface(
    void* user_data, const std::string& surface_id) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner ||
      !instance->owner->Permitted(*instance, "ui.commands") ||
      !instance->enabled || instance->failed) {
    return -1;
  }
  const auto item = instance->surfaces.find(surface_id);
  if (item == instance->surfaces.end() || !instance->owner->surface_opened)
    return -2;
  const std::string package_id = instance->id;
  const DeclarativeSurface surface = item->second;
  const auto opened = instance->owner->surface_opened;
  instance->owner->Publish([opened, package_id, surface]() {
    opened(package_id, surface);
  });
  return 0;
}

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
  std::lock_guard<std::mutex> lock(owner.position_mutex);
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
  {
    std::lock_guard<std::mutex> lock(instance->state_mutex);
    instance->scenes[id] = std::move(scene);
  }
  instance->owner->PublishStateChanged();
  return 0;
}

std::int32_t RuntimeEngine::Impl::ClearScene(void* user_data,
                                             const char* scene_id,
                                             std::size_t scene_id_length) {
  auto* instance = static_cast<Instance*>(user_data);
  const std::string id = Text(scene_id, scene_id_length);
  if (!instance || !instance->owner || !IsSafeName(id)) return -1;
  {
    std::lock_guard<std::mutex> lock(instance->state_mutex);
    instance->scenes.erase(id);
  }
  instance->owner->PublishStateChanged();
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
  ++instance.enable_count;
  instance.diagnostic.clear();
  wxLogMessage("PPM package-enabled id=%s version=%s", instance.id,
               instance.version);
  return true;
}

bool RuntimeEngine::Impl::Stop(Instance& instance, bool destroy,
                               std::string* diagnostic) {
  const bool was_enabled = instance.enabled.exchange(false);
  instance.executor.AdvanceGeneration();
  if (!instance.executor.WaitIdle(std::chrono::seconds(6))) {
    const std::string message =
        "portable component did not reach its six-second action shutdown "
        "barrier";
    {
      std::lock_guard<std::mutex> state_lock(instance.state_mutex);
      instance.diagnostic = message;
    }
    remove_actions(instance.id);
    if (diagnostic) *diagnostic = message;
    return false;
  }
  std::lock_guard<std::mutex> lock(instance.runtime_mutex);
  bool okay = true;
  std::array<char, kErrorCapacity> error{};
  if (instance.runtime && was_enabled) {
    if (ocpn_portable_runtime_disable(instance.runtime, error.data(),
                                      error.size()) != 0) {
      okay = false;
      instance.diagnostic =
          error[0] ? error.data() : "runtime disable failed";
      wxLogWarning("PPM package-disable-failed id=%s diagnostic=%s",
                   instance.id, instance.diagnostic);
      destroy = true;
    } else {
      ++instance.disable_count;
    }
  }
  {
    std::lock_guard<std::mutex> state_lock(instance.state_mutex);
    instance.scenes.clear();
  }
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
  if (!instance->failed && manifest.HasMember("surfaces")) {
    const wxJSONValue declared_surfaces = manifest["surfaces"];
    if (!declared_surfaces.IsObject() ||
        declared_surfaces.Size() == 0 || declared_surfaces.Size() > 16) {
      instance->failed = true;
      instance->diagnostic = "manifest surfaces are invalid or exceed policy";
    } else {
      const wxArrayString names = declared_surfaces.GetMemberNames();
      for (const auto& name : names) {
        const std::string surface_id = name.ToStdString();
        const wxJSONValue resource_value =
            declared_surfaces.ItemAt(name);
        if (!IsSafeName(surface_id) || !resource_value.IsString() ||
            !SafeRelativePath(resource_value.AsString())) {
          instance->failed = true;
          instance->diagnostic = "manifest surface identity/path is invalid";
          break;
        }
        const fs::path surface_path =
            (root / resource_value.AsString().ToStdString())
                .lexically_normal();
        bool read = false;
        const std::string contents =
            ReadSmallFile(surface_path, kSurfaceDocumentLimit, &read);
        wxJSONValue document;
        wxJSONReader surface_reader;
        wxStringInputStream stream(wxString::FromUTF8(contents));
        DeclarativeSurface surface;
        std::string surface_diagnostic;
        const std::string expected =
            surface_id == "routing.workbench" ? std::string() : surface_id;
        if (!read || surface_reader.Parse(stream, &document) != 0 ||
            !ParseDeclarativeSurface(document, expected, &surface,
                                     &surface_diagnostic) ||
            (surface_id == "routing.workbench" &&
             surface.id != "routing.workbench" &&
             surface.id != "routing-workbench")) {
          instance->failed = true;
          instance->diagnostic =
              "invalid declarative surface " + surface_id + ": " +
              (surface_diagnostic.empty() ? "JSON parse or identity failure"
                                          : surface_diagnostic);
          break;
        }
        surface.id = surface_id;
        instance->surfaces.emplace(surface_id, std::move(surface));
      }
    }
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
    if (!Stop(**item, true, diagnostic)) {
      state_changed();
      return false;
    }
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
  {
    std::lock_guard<std::mutex> state_lock(instance.state_mutex);
    instance.diagnostic = operation + ": " + diagnostic;
    instance.scenes.clear();
  }
  if (instance.runtime) {
    ocpn_portable_runtime_destroy(instance.runtime);
    instance.runtime = nullptr;
  }
  instance.registered_actions.clear();
  const std::string id = instance.id;
  const std::string message = operation + ": " + diagnostic;
  wxLogError("PPM package-failed id=%s diagnostic=%s", id, message);
  PublishRemoveActions(id);
  PublishStateChanged();
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
  const std::uint64_t generation = instance.executor.Generation();
  const auto posted = instance.executor.Post(
      generation, [&instance, action_id](std::uint64_t task_generation) {
        if (task_generation != instance.executor.Generation() ||
            !instance.enabled || instance.failed) {
          return;
        }
        std::lock_guard<std::mutex> lock(instance.runtime_mutex);
        if (task_generation != instance.executor.Generation() ||
            !instance.enabled || instance.failed || !instance.runtime) {
          return;
        }
        std::array<char, kErrorCapacity> error{};
        if (ocpn_portable_runtime_on_action(
                instance.runtime, action_id.data(), action_id.size(),
                error.data(), error.size()) != 0) {
          instance.owner->Fail(
              instance, "action " + action_id,
              error[0] ? error.data() : "portable component action failed");
        }
      });
  if (posted != SerialExecutor::PostResult::kAccepted) {
    wxLogWarning("PPM package-action-not-queued id=%s action=%s reason=%d",
                 package_id, action_id, static_cast<int>(posted));
  }
  return true;
}

bool RuntimeEngine::Impl::HandleSurfaceEvent(
    const std::string& package_id, const std::string& surface_id,
    const std::string& control_id, const std::string& value_json) {
  Instance* instance = Find(package_id);
  if (!instance || !instance->enabled || instance->failed ||
      !instance->runtime || !IsSafeName(surface_id) ||
      !IsSafeName(control_id) || value_json.size() > kSurfaceStateLimit ||
      instance->surfaces.count(surface_id) == 0) {
    return false;
  }
  const std::uint64_t generation = instance->executor.Generation();
  const auto posted = instance->executor.Post(
      generation,
      [instance, surface_id, control_id,
       value_json](std::uint64_t task_generation) {
        if (task_generation != instance->executor.Generation() ||
            !instance->enabled || instance->failed) {
          return;
        }
        std::lock_guard<std::mutex> lock(instance->runtime_mutex);
        if (task_generation != instance->executor.Generation() ||
            !instance->enabled || instance->failed || !instance->runtime) {
          return;
        }
        std::vector<char> state(kSurfaceStateLimit + 1, '\0');
        std::array<char, kErrorCapacity> error{};
        std::size_t state_length = 0;
        const int result = ocpn_portable_runtime_on_surface_event(
            instance->runtime, surface_id.data(), surface_id.size(),
            control_id.data(), control_id.size(), value_json.data(),
            value_json.size(), state.data(), kSurfaceStateLimit,
            &state_length, error.data(), error.size());
        std::string state_json;
        std::string diagnostic;
        if (result == 0 && state_length <= kSurfaceStateLimit) {
          state_json.assign(state.data(), state_length);
        } else {
          diagnostic =
              error[0] ? error.data() : "portable surface event failed";
        }
        if (!instance->owner->surface_response) return;
        const auto response = instance->owner->surface_response;
        const std::string id = instance->id;
        instance->owner->Publish(
            [response, id, surface_id, control_id,
             state_json = std::move(state_json),
             diagnostic = std::move(diagnostic)]() {
              response(id, surface_id, control_id, state_json, diagnostic);
            });
      });
  return posted == SerialExecutor::PostResult::kAccepted;
}

bool RuntimeEngine::Impl::WaitForIdle(const std::string& package_id,
                                      std::chrono::milliseconds timeout) {
  Instance* instance = Find(package_id);
  return instance && instance->executor.WaitIdle(timeout);
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
                             StateChanged state_changed,
                             UiDispatch ui_dispatch)
    : impl_(std::make_unique<Impl>(storage_root, std::move(register_action),
                                  std::move(remove_actions),
                                  std::move(state_changed),
                                  std::move(ui_dispatch))),
      storage_root_(std::move(storage_root)) {}

RuntimeEngine::~RuntimeEngine() = default;

bool RuntimeEngine::LoadInstalled(bool developer_mode) {
  return impl_->LoadInstalled(developer_mode);
}

void RuntimeEngine::SetSurfaceOpenedCallback(SurfaceOpened callback) {
  impl_->SetSurfaceOpenedCallback(std::move(callback));
}

void RuntimeEngine::SetSurfaceResponseCallback(SurfaceResponse callback) {
  impl_->SetSurfaceResponseCallback(std::move(callback));
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

bool RuntimeEngine::HandleSurfaceEvent(
    const std::string& package_id, const std::string& surface_id,
    const std::string& control_id, const std::string& value_json) {
  return impl_->HandleSurfaceEvent(package_id, surface_id, control_id,
                                   value_json);
}

bool RuntimeEngine::WaitForIdle(const std::string& package_id,
                                std::chrono::milliseconds timeout) {
  return impl_->WaitForIdle(package_id, timeout);
}

void RuntimeEngine::SetPositionFix(const PlugIn_Position_Fix_Ex& fix) {
  std::lock_guard<std::mutex> lock(impl_->position_mutex);
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
    std::string diagnostic;
    {
      std::lock_guard<std::mutex> lock(instance->state_mutex);
      diagnostic = instance->diagnostic;
    }
    result.push_back(
         {instance->id, instance->name, instance->version,
         instance->failed ? "Failed"
         : instance->enabled ? "Enabled"
         : instance->runtime ? "Disabled"
                             : "Unloaded",
         "Unknown",
         diagnostic,
         instance->executor.Generation(),
         instance->executor.Pending(),
         instance->enable_count.load(),
         instance->disable_count.load()});
    result.back().surface_count = instance->surfaces.size();
  }
  return result;
}

std::vector<OverlayScene> RuntimeEngine::Scenes() const {
  std::vector<OverlayScene> result;
  for (const auto& instance : impl_->instances) {
    if (!instance->enabled || instance->failed) continue;
    std::lock_guard<std::mutex> lock(instance->state_mutex);
    for (const auto& item : instance->scenes) result.push_back(item.second);
  }
  return result;
}

}  // namespace ppm
