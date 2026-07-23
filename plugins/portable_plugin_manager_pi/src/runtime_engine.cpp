#include "runtime_engine.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
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

#include <openssl/rand.h>

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/log.h>
#include <wx/regex.h>
#include <wx/sstream.h>
#include <wx/thread.h>
#include <wx/wfstream.h>

#include "ocpn_plugin.h"
#include "ocpn_portable_runtime.h"
#include "declarative_ui.h"
#include "environment_provider.h"
#include "job_scheduler.h"
#include "permission_store.h"
#include "service_version.h"
#include "serial_executor.h"

namespace ppm {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kErrorCapacity = 4096;
constexpr std::size_t kSettingCapacity = 64 * 1024;
constexpr std::size_t kOverlayPointLimit = 1'000'000;
constexpr std::size_t kPrivateReadLimit = 8 * 1024 * 1024;
constexpr std::size_t kUserFileLimit = 8 * 1024 * 1024;
constexpr std::size_t kUserFileGrantLimit = 32;
constexpr std::size_t kSurfaceDocumentLimit = 1024 * 1024;
constexpr std::size_t kSurfaceStateLimit = 64 * 1024;
constexpr std::size_t kRoutePointLimit = 20'000;
constexpr std::size_t kRouteInspectionPointLimit = 200'000;
constexpr std::size_t kRouteInspectionLineLimit = 10'000;
constexpr std::size_t kRoutePolarLimit = 16;
constexpr std::size_t kRoutePolarAxisLimit = 512;
constexpr std::size_t kRoutePolarCellLimit = 256 * 1024;

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
  static const wxRegEx expression("^[a-z0-9]+([.-][a-z0-9]+)+$");
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

std::string ReadSmallFile(const fs::path& path, std::size_t limit, bool* okay) {
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
  struct UserFileGrant {
    fs::path path;
    bool writable = false;
    bool consumed = false;
    std::uint64_t generation = 0;
  };

  struct Instance {
    ~Instance() {
      routing_cancelled = true;
      if (routing_worker.joinable()) routing_worker.join();
    }

    Impl* owner = nullptr;
    std::string id;
    std::string name;
    std::string version;
    fs::path package_root;
    fs::path component_path;
    fs::path private_root;
    std::set<std::string> requested_permissions;
    std::set<std::string> permissions;
    std::map<std::string, std::string> provided_services;
    std::map<std::string, std::string> required_services;
    std::unique_ptr<EnvironmentProvider> environment_provider;
    ocpn_portable_runtime* runtime = nullptr;
    mutable std::mutex runtime_mutex;
    mutable std::mutex state_mutex;
    SerialExecutor executor;
    std::atomic_bool enabled{false};
    std::atomic_bool failed{false};
    std::atomic_uint64_t enable_count{0};
    std::atomic_uint64_t disable_count{0};
    std::atomic_bool routing_cancelled{false};
    std::atomic_bool routing_running{false};
    mutable std::mutex routing_mutex;
    std::condition_variable routing_changed;
    std::thread routing_worker;
    bool loadable = false;
    std::string diagnostic;
    std::vector<RuntimeAction> registered_actions;
    std::map<std::string, OverlayScene> scenes;
    std::map<std::string, DeclarativeSurface> surfaces;
    std::map<std::string, UserFileGrant> user_file_grants;
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
  void SetRoutingProgressCallback(RoutingProgress callback) {
    routing_progress = std::move(callback);
  }
  void SetRoutingCompletedCallback(RoutingCompleted callback) {
    routing_completed = std::move(callback);
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
  bool RegisterUserFileGrant(const std::string& package_id,
                             const std::string& path, bool writable,
                             std::string* token, std::string* diagnostic);
  bool SelectEnvironmentDataset(const std::string& package_id,
                                const std::vector<std::string>& selected_paths);
  std::string EnvironmentSummary(const std::string& package_id) const;
  bool StartRoute(const std::string& package_id, RoutingRequest request,
                  std::string* diagnostic);
  bool CancelRoute(const std::string& package_id);
  bool WaitForRoute(const std::string& package_id,
                    std::chrono::milliseconds timeout);
  bool WaitForIdle(const std::string& package_id,
                   std::chrono::milliseconds timeout);
  void DeliverNavigationSentence(const std::string& sentence);
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
  const Instance* CompatibleProvider(const Instance& consumer,
                                     const std::string& interface) const {
    const auto requirement = consumer.required_services.find(interface);
    if (requirement == consumer.required_services.end()) return nullptr;
    for (const auto& candidate : instances) {
      if (candidate.get() == &consumer || !candidate->enabled ||
          candidate->failed) {
        continue;
      }
      const auto provided = candidate->provided_services.find(interface);
      if (provided != candidate->provided_services.end() &&
          ServiceVersionSatisfies(provided->second, requirement->second)) {
        return candidate.get();
      }
    }
    return nullptr;
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
  bool RunUiService(std::function<void()> task,
                    std::chrono::milliseconds timeout) {
    if (!task) return false;
    if (!ui_dispatch || wxIsMainThread()) {
      task();
      return true;
    }
    struct Completion {
      std::mutex mutex;
      std::condition_variable changed;
      bool complete = false;
    };
    auto completion = std::make_shared<Completion>();
    ui_dispatch([completion, task = std::move(task)]() mutable {
      task();
      {
        std::lock_guard<std::mutex> lock(completion->mutex);
        completion->complete = true;
      }
      completion->changed.notify_all();
    });
    std::unique_lock<std::mutex> lock(completion->mutex);
    return completion->changed.wait_for(
        lock, timeout, [&completion]() { return completion->complete; });
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
  static std::int32_t GetVesselPosition(void* user_data, double* latitude,
                                        double* longitude, double* cog,
                                        std::uint8_t* has_cog, double* sog,
                                        std::uint8_t* has_sog);
  static std::int32_t SettingGet(void* user_data, const char* key,
                                 std::size_t key_length, char* value,
                                 std::size_t value_capacity,
                                 std::size_t* value_length,
                                 std::uint8_t* found);
  static std::int32_t SettingSet(void* user_data, const char* key,
                                 std::size_t key_length, const char* value,
                                 std::size_t value_length);
  static std::int32_t SubmitPolyline(void* user_data, const char* scene_id,
                                     std::size_t scene_id_length,
                                     const ocpn_portable_geo_point* points,
                                     std::size_t point_count,
                                     ocpn_portable_overlay_style style);
  static std::int32_t ClearScene(void* user_data, const char* scene_id,
                                 std::size_t scene_id_length);
  static std::int32_t Unsupported(void*) { return -1; }
  static std::int32_t StartJob(void* user_data, const char* job_id,
                               std::size_t job_id_length,
                               std::uint32_t work_units);
  static std::int32_t CancelJob(void* user_data, const char* job_id,
                                std::size_t job_id_length);
  static std::int32_t OpenEnvironmentalViewer(void* user_data) {
    return OpenNamedSurface(user_data, "environment.viewer");
  }
  static std::int32_t OpenWeatherRouting(void* user_data) {
    return OpenNamedSurface(user_data, "routing.workbench");
  }
  static std::int32_t OpenNamedSurface(void* user_data,
                                       const std::string& surface_id);
  static std::int32_t OpenSurface(void* user_data, const char* surface_id,
                                  std::size_t surface_id_length) {
    return OpenNamedSurface(user_data, Text(surface_id, surface_id_length));
  }
  static std::int32_t EnvironmentSampleBatch(
      void* user_data, const ocpn_portable_environment_sample_request* requests,
      std::size_t request_count, ocpn_portable_environment_sample* results,
      std::size_t result_count, char* error, std::size_t error_capacity);
  static void RoutingProgressCallback(void* user_data, std::uint8_t percent,
                                      const char* message,
                                      std::size_t message_length);
  static std::uint8_t RoutingCancelled(void* user_data);
  static std::int32_t ChartsQuerySegments(
      void* user_data, const ocpn_portable_geo_segment* segments,
      std::size_t segment_count, ocpn_portable_chart_segment_result* results,
      std::size_t result_count);
  static std::int32_t NetworkGetToPrivate(void*, const char*, std::size_t,
                                          const char*, std::size_t, const char*,
                                          std::size_t, std::uint64_t) {
    return -1;
  }
  static std::int32_t StoragePrivateRead(void* user_data,
                                         const char* private_name,
                                         std::size_t private_name_length,
                                         std::uint8_t* value,
                                         std::size_t value_capacity,
                                         std::size_t* value_length);
  static std::int32_t UserFileRead(void* user_data, const char* grant_token,
                                   std::size_t grant_token_length,
                                   std::uint8_t* value,
                                   std::size_t value_capacity,
                                   std::size_t* value_length);
  static std::int32_t UserFileWrite(void* user_data, const char* grant_token,
                                    std::size_t grant_token_length,
                                    const std::uint8_t* value,
                                    std::size_t value_length);
  void DeliverJobEvent(Instance* instance, const JobEvent& event);

  fs::path storage_root;
  RegisterAction register_action;
  RemoveActions remove_actions;
  StateChanged state_changed;
  UiDispatch ui_dispatch;
  SurfaceOpened surface_opened;
  SurfaceResponse surface_response;
  RoutingProgress routing_progress;
  RoutingCompleted routing_completed;
  JobScheduler jobs;
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

std::int32_t RuntimeEngine::Impl::StartJob(void* user_data, const char* job_id,
                                           std::size_t job_id_length,
                                           std::uint32_t work_units) {
  auto* instance = static_cast<Instance*>(user_data);
  const std::string id = Text(job_id, job_id_length);
  if (!instance || !instance->owner || !instance->enabled || instance->failed ||
      !IsSafeName(id) ||
      !instance->owner->Permitted(*instance, "jobs.compute")) {
    return -1;
  }
  const std::uint64_t generation = instance->executor.Generation();
  std::string diagnostic;
  return instance->owner->jobs.Start(
             instance->id, id, generation, work_units,
             [owner = instance->owner, instance](const JobEvent& event) {
               owner->DeliverJobEvent(instance, event);
             },
             &diagnostic)
             ? 0
             : -2;
}

std::int32_t RuntimeEngine::Impl::CancelJob(void* user_data, const char* job_id,
                                            std::size_t job_id_length) {
  auto* instance = static_cast<Instance*>(user_data);
  const std::string id = Text(job_id, job_id_length);
  if (!instance || !instance->owner || !IsSafeName(id) ||
      !instance->owner->Permitted(*instance, "jobs.compute")) {
    return -1;
  }
  return instance->owner->jobs.Cancel(instance->id, id,
                                      instance->executor.Generation())
             ? 0
             : -2;
}

void RuntimeEngine::Impl::DeliverJobEvent(Instance* instance,
                                          const JobEvent& event) {
  if (!instance || event.owner != instance->id ||
      event.generation != instance->executor.Generation()) {
    return;
  }
  instance->executor.Post(
      event.generation, [instance, event](std::uint64_t task_generation) {
        if (task_generation != instance->executor.Generation() ||
            !instance->enabled || instance->failed) {
          return;
        }
        std::lock_guard<std::mutex> lock(instance->runtime_mutex);
        if (task_generation != instance->executor.Generation() ||
            !instance->enabled || instance->failed || !instance->runtime) {
          return;
        }
        std::array<char, kErrorCapacity> error{};
        if (ocpn_portable_runtime_on_job_event(
                instance->runtime, event.id.data(), event.id.size(),
                static_cast<std::uint32_t>(event.kind), event.progress,
                event.message.data(), event.message.size(), error.data(),
                error.size()) != 0) {
          instance->owner->Fail(
              *instance, "job event " + event.id,
              error[0] ? error.data() : "portable job event failed");
        } else {
          instance->owner->PublishStateChanged();
        }
      });
}

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
  instance->owner->Publish(
      [opened, package_id, surface]() { opened(package_id, surface); });
  return 0;
}

bool RuntimeEngine::Impl::RegisterUserFileGrant(const std::string& package_id,
                                                const std::string& path_value,
                                                bool writable,
                                                std::string* token,
                                                std::string* diagnostic) {
  Instance* instance = Find(package_id);
  if (!instance || !instance->enabled || instance->failed ||
      !Permitted(*instance, "storage.user-selected")) {
    if (diagnostic)
      *diagnostic = "package is unavailable or lacks user-selected file access";
    return false;
  }
  fs::path path = fs::path(path_value).lexically_normal();
  std::error_code error;
  if (!path.is_absolute()) {
    if (diagnostic) *diagnostic = "selected path is not absolute";
    return false;
  }
  if (writable) {
    if (fs::is_directory(path, error) || fs::is_symlink(path, error)) {
      if (diagnostic)
        *diagnostic = "save target must not be a directory or link";
      return false;
    }
    error.clear();
    const fs::path parent = fs::canonical(path.parent_path(), error);
    if (error || !fs::is_directory(parent, error)) {
      if (diagnostic) *diagnostic = "save target directory is unavailable";
      return false;
    }
    path = parent / path.filename();
  } else {
    path = fs::canonical(path, error);
    if (error || !fs::is_regular_file(path, error) ||
        fs::file_size(path, error) > kUserFileLimit || error) {
      if (diagnostic)
        *diagnostic = "selected file is unavailable or exceeds the 8 MiB limit";
      return false;
    }
  }

  std::array<unsigned char, 16> random{};
  if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
    if (diagnostic) *diagnostic = "could not create a secure file grant";
    return false;
  }
  static constexpr char digits[] = "0123456789abcdef";
  std::string generated = "ufg-";
  generated.reserve(4 + random.size() * 2);
  for (const unsigned char byte : random) {
    generated.push_back(digits[byte >> 4]);
    generated.push_back(digits[byte & 0x0f]);
  }
  {
    std::lock_guard<std::mutex> lock(instance->state_mutex);
    if (instance->user_file_grants.size() >= kUserFileGrantLimit) {
      if (diagnostic) *diagnostic = "package has too many active file grants";
      return false;
    }
    instance->user_file_grants.emplace(
        generated,
        UserFileGrant{path, writable, false, instance->executor.Generation()});
  }
  if (token) *token = generated;
  return true;
}

bool RuntimeEngine::Impl::SelectEnvironmentDataset(
    const std::string& package_id,
    const std::vector<std::string>& selected_paths) {
  Instance* instance = Find(package_id);
  if (!instance || !instance->enabled || instance->failed ||
      !instance->environment_provider ||
      !Permitted(*instance, "environment.datasets") ||
      !Permitted(*instance, "helpers.environment.decode") ||
      selected_paths.empty() || selected_paths.size() > 16) {
    return false;
  }
  const std::uint64_t generation = instance->executor.Generation();
  const auto posted = instance->executor.Post(
      generation, [instance, selected_paths](std::uint64_t task_generation) {
        if (task_generation != instance->executor.Generation() ||
            !instance->enabled || instance->failed ||
            !instance->environment_provider) {
          return;
        }
        std::string diagnostic;
        const bool opened = instance->environment_provider->OpenDataset(
            selected_paths,
            [instance, task_generation]() {
              return task_generation != instance->executor.Generation() ||
                     !instance->enabled || instance->failed;
            },
            &diagnostic);
        if (!instance->owner->surface_response) return;
        std::string state;
        if (opened) {
          const std::string summary = instance->environment_provider->Summary();
          auto json_string = [](const std::string& value) {
            std::string result{"\""};
            for (const unsigned char character : value) {
              if (character == '"' || character == '\\') {
                result.push_back('\\');
                result.push_back(static_cast<char>(character));
              } else if (character >= 0x20) {
                result.push_back(static_cast<char>(character));
              }
            }
            result.push_back('"');
            return result;
          };
          state =
              "{\"status\":\"GRIB ready for weather routing\","
              "\"controls\":{\"file\":" +
              json_string(summary) + "}}";
        }
        const auto response = instance->owner->surface_response;
        const std::string id = instance->id;
        instance->owner->Publish([response, id, state = std::move(state),
                                  diagnostic = std::move(diagnostic)]() {
          response(id, "environment.viewer", "open", state, diagnostic);
        });
      });
  return posted == SerialExecutor::PostResult::kAccepted;
}

std::string RuntimeEngine::Impl::EnvironmentSummary(
    const std::string& package_id) const {
  const Instance* instance = Find(package_id);
  return instance && instance->environment_provider
             ? instance->environment_provider->Summary()
             : "No environmental provider is loaded";
}

bool RuntimeEngine::Impl::StartRoute(const std::string& package_id,
                                     RoutingRequest request,
                                     std::string* diagnostic) {
  Instance* instance = Find(package_id);
  if (!instance || !instance->enabled || instance->failed ||
      !instance->runtime) {
    if (diagnostic) *diagnostic = "routing package is not enabled";
    return false;
  }
  if (!Permitted(*instance, "weather-routing.compute")) {
    if (diagnostic)
      *diagnostic = "weather-routing.compute permission is not granted";
    return false;
  }
  if (request.polars.empty() || request.polars.size() > kRoutePolarLimit ||
      !std::isfinite(request.parameters.start_latitude) ||
      !std::isfinite(request.parameters.start_longitude) ||
      !std::isfinite(request.parameters.destination_latitude) ||
      !std::isfinite(request.parameters.destination_longitude) ||
      std::abs(request.parameters.start_latitude) > 90.0 ||
      std::abs(request.parameters.destination_latitude) > 90.0 ||
      std::abs(request.parameters.start_longitude) > 180.0 ||
      std::abs(request.parameters.destination_longitude) > 180.0) {
    if (diagnostic) *diagnostic = "invalid route endpoints or polar count";
    return false;
  }
  for (const auto& polar : request.polars) {
    const std::size_t wind_count = polar.true_wind_speeds_knots.size();
    const std::size_t angle_count = polar.true_wind_angles_degrees.size();
    if (polar.identity.empty() || polar.identity.size() > 1024 ||
        wind_count < 2 || angle_count < 2 ||
        wind_count > kRoutePolarAxisLimit ||
        angle_count > kRoutePolarAxisLimit ||
        wind_count > kRoutePolarCellLimit / angle_count ||
        polar.boat_speeds_knots.size() != wind_count * angle_count ||
        !std::all_of(polar.true_wind_speeds_knots.begin(),
                     polar.true_wind_speeds_knots.end(),
                     [](double value) {
                       return std::isfinite(value) && value >= 0.0 &&
                              value <= 250.0;
                     }) ||
        !std::all_of(polar.true_wind_angles_degrees.begin(),
                     polar.true_wind_angles_degrees.end(),
                     [](double value) {
                       return std::isfinite(value) && value >= 0.0 &&
                              value <= 180.0;
                     }) ||
        !std::all_of(polar.boat_speeds_knots.begin(),
                     polar.boat_speeds_knots.end(), [](double value) {
                       return std::isfinite(value) && value >= 0.0 &&
                              value <= 250.0;
                     }) ||
        std::adjacent_find(polar.true_wind_speeds_knots.begin(),
                           polar.true_wind_speeds_knots.end(),
                           [](double left, double right) {
                             return right <= left;
                           }) != polar.true_wind_speeds_knots.end() ||
        std::adjacent_find(polar.true_wind_angles_degrees.begin(),
                           polar.true_wind_angles_degrees.end(),
                           [](double left, double right) {
                             return right <= left;
                           }) != polar.true_wind_angles_degrees.end()) {
      if (diagnostic) *diagnostic = "invalid or unbounded polar grid";
      return false;
    }
  }

  std::thread previous_worker;
  {
    std::lock_guard<std::mutex> route_lock(instance->routing_mutex);
    if (instance->routing_running) {
      if (diagnostic) *diagnostic = "a route calculation is already running";
      return false;
    }
    if (instance->routing_worker.joinable())
      previous_worker = std::move(instance->routing_worker);
  }
  if (previous_worker.joinable()) previous_worker.join();

  std::array<char, kErrorCapacity> clone_error{};
  ocpn_portable_runtime* replica = nullptr;
  {
    std::lock_guard<std::mutex> runtime_lock(instance->runtime_mutex);
    if (instance->enabled && !instance->failed && instance->runtime) {
      replica = ocpn_portable_runtime_clone_compute(
          instance->runtime, clone_error.data(), clone_error.size());
    }
  }
  if (!replica) {
    if (diagnostic) {
      *diagnostic = clone_error[0] ? clone_error.data()
                                   : "could not create routing worker";
    }
    return false;
  }

  {
    std::lock_guard<std::mutex> route_lock(instance->routing_mutex);
    if (!instance->enabled || instance->failed || instance->routing_running) {
      ocpn_portable_runtime_destroy(replica);
      if (diagnostic) *diagnostic = "routing package changed state";
      return false;
    }
    instance->routing_cancelled = false;
    instance->routing_running = true;
    try {
      instance->routing_worker = std::thread([instance, replica,
                                              request = std::move(
                                                  request)]() mutable {
      std::unique_ptr<ocpn_portable_runtime,
                      decltype(&ocpn_portable_runtime_destroy)>
          worker_runtime(replica, ocpn_portable_runtime_destroy);
      RoutingOutcome outcome;
      std::string failure;
      bool success = false;
      try {
        std::vector<ocpn_portable_polar_grid> polar_views;
        polar_views.reserve(request.polars.size());
        for (const auto& polar : request.polars) {
          polar_views.push_back({polar.identity.data(), polar.identity.size(),
                                 polar.true_wind_speeds_knots.data(),
                                 polar.true_wind_speeds_knots.size(),
                                 polar.true_wind_angles_degrees.data(),
                                 polar.true_wind_angles_degrees.size(),
                                 polar.boat_speeds_knots.data(),
                                 polar.boat_speeds_knots.size()});
        }
        request.parameters.polars = polar_views.data();
        request.parameters.polar_count = polar_views.size();

        std::vector<ocpn_portable_route_point> points(kRoutePointLimit);
        std::vector<ocpn_portable_route_environment_point> environment(
            kRoutePointLimit);
        std::vector<ocpn_portable_route_point> isochrone_points(
            kRouteInspectionPointLimit);
        std::vector<ocpn_portable_route_line> isochrones(
            kRouteInspectionLineLimit);
        std::vector<ocpn_portable_route_point> trace_points(
            kRouteInspectionPointLimit);
        std::vector<ocpn_portable_route_line> traces(kRouteInspectionLineLimit);
        std::array<char, kErrorCapacity> error{};
        std::array<char, kErrorCapacity> result_diagnostic{};
        ocpn_portable_route_result result{};
        result.points = points.data();
        result.point_capacity = points.size();
        result.isochrone_points = isochrone_points.data();
        result.isochrone_point_capacity = isochrone_points.size();
        result.isochrones = isochrones.data();
        result.isochrone_capacity = isochrones.size();
        result.trace_points = trace_points.data();
        result.trace_point_capacity = trace_points.size();
        result.traces = traces.data();
        result.trace_capacity = traces.size();
        result.route_environment = environment.data();
        result.route_environment_capacity = environment.size();
        result.diagnostic = result_diagnostic.data();
        result.diagnostic_capacity = result_diagnostic.size();

        const int status = ocpn_portable_runtime_calculate_route(
            worker_runtime.get(), &request.parameters, &result, error.data(),
            error.size());
        worker_runtime.reset();

        const bool bounded =
            result.point_count <= points.size() &&
            result.route_environment_count <= environment.size() &&
            result.isochrone_point_count <= isochrone_points.size() &&
            result.isochrone_count <= isochrones.size() &&
            result.trace_point_count <= trace_points.size() &&
            result.trace_count <= traces.size() &&
            result.diagnostic_len < result_diagnostic.size();
        auto valid_lines = [](const auto& lines, std::size_t line_count,
                              std::size_t point_count) {
          for (std::size_t index = 0; index < line_count; ++index) {
            if (lines[index].point_offset > point_count ||
                lines[index].point_count >
                    point_count - lines[index].point_offset) {
              return false;
            }
          }
          return true;
        };
        const bool valid_spans =
            bounded &&
            valid_lines(isochrones, result.isochrone_count,
                        result.isochrone_point_count) &&
            valid_lines(traces, result.trace_count, result.trace_point_count);
        success = status == 0 && bounded && valid_spans;
        if (!bounded || !valid_spans) {
          failure =
              "routing component returned an invalid or oversized "
              "result";
        } else {
          outcome.points.assign(points.begin(),
                                points.begin() + result.point_count);
          outcome.route_environment.assign(
              environment.begin(),
              environment.begin() + result.route_environment_count);
          auto copy_lines = [](const auto& source_points,
                               const auto& source_lines, std::size_t line_count,
                               auto* destination) {
            destination->reserve(line_count);
            for (std::size_t index = 0; index < line_count; ++index) {
              const auto& line = source_lines[index];
              RoutingInspectionLine copied;
              copied.unix_time = line.unix_time;
              copied.points.assign(
                  source_points.begin() + line.point_offset,
                  source_points.begin() + line.point_offset + line.point_count);
              destination->push_back(std::move(copied));
            }
          };
          copy_lines(isochrone_points, isochrones, result.isochrone_count,
                     &outcome.isochrones);
          copy_lines(trace_points, traces, result.trace_count, &outcome.traces);
          outcome.diagnostic.assign(result_diagnostic.data(),
                                    result.diagnostic_len);
          outcome.distance_nautical_miles = result.distance_nautical_miles;
          outcome.duration_seconds = result.duration_seconds;
          outcome.states_examined = result.states_examined;
          outcome.average_speed_knots = result.average_speed_knots;
          outcome.maximum_speed_knots = result.maximum_speed_knots;
          outcome.average_sog_knots = result.average_sog_knots;
          outcome.maximum_sog_knots = result.maximum_sog_knots;
          outcome.average_wind_knots = result.average_wind_knots;
          outcome.maximum_wind_knots = result.maximum_wind_knots;
          outcome.average_current_knots = result.average_current_knots;
          outcome.maximum_current_knots = result.maximum_current_knots;
          outcome.tacks = result.tacks;
          outcome.motor_seconds = result.motor_seconds;
          outcome.estimated_fuel_litres = result.estimated_fuel_litres;
          outcome.propulsion_transitions = result.propulsion_transitions;
          outcome.comfort_level = result.comfort_level;
          outcome.metrics_available = result.metrics_available;
          if (!success) {
            failure = error[0] ? error.data() : outcome.diagnostic;
            if (failure.empty()) failure = "route calculation failed";
          }
        }
      } catch (const std::exception& exception) {
        failure =
            std::string("routing worker failed safely: ") + exception.what();
      } catch (...) {
        failure = "routing worker failed safely with an unknown error";
      }

      {
        std::lock_guard<std::mutex> route_lock(instance->routing_mutex);
        instance->routing_running = false;
      }
      instance->routing_changed.notify_all();
      const auto completed = instance->owner->routing_completed;
      if (completed) {
        const std::string id = instance->id;
        instance->owner->Publish([completed, id, success,
                                  outcome = std::move(outcome),
                                  failure = std::move(failure)]() mutable {
          completed(id, success, std::move(outcome), failure);
        });
      }
      });
    } catch (const std::exception& exception) {
      instance->routing_running = false;
      ocpn_portable_runtime_destroy(replica);
      if (diagnostic)
        *diagnostic =
            std::string("could not start routing worker: ") + exception.what();
      return false;
    }
  }
  return true;
}

bool RuntimeEngine::Impl::CancelRoute(const std::string& package_id) {
  Instance* instance = Find(package_id);
  if (!instance) return false;
  instance->routing_cancelled = true;
  return instance->routing_running;
}

bool RuntimeEngine::Impl::WaitForRoute(const std::string& package_id,
                                       std::chrono::milliseconds timeout) {
  Instance* instance = Find(package_id);
  if (!instance) return false;
  std::thread worker;
  {
    std::unique_lock<std::mutex> route_lock(instance->routing_mutex);
    if (!instance->routing_changed.wait_for(
            route_lock, timeout,
            [instance]() { return !instance->routing_running; })) {
      return false;
    }
    if (instance->routing_worker.joinable())
      worker = std::move(instance->routing_worker);
  }
  if (worker.joinable()) worker.join();
  return true;
}

std::int32_t RuntimeEngine::Impl::UserFileRead(void* user_data,
                                               const char* grant_token,
                                               std::size_t grant_token_length,
                                               std::uint8_t* value,
                                               std::size_t value_capacity,
                                               std::size_t* value_length) {
  auto* instance = static_cast<Instance*>(user_data);
  const std::string token = Text(grant_token, grant_token_length);
  if (!instance || !instance->owner || !value_length ||
      !instance->owner->Permitted(*instance, "storage.user-selected") ||
      !instance->enabled || instance->failed ||
      value_capacity > kUserFileLimit) {
    return -1;
  }
  fs::path path;
  {
    std::lock_guard<std::mutex> lock(instance->state_mutex);
    const auto item = instance->user_file_grants.find(token);
    if (item == instance->user_file_grants.end() || item->second.writable ||
        item->second.consumed ||
        item->second.generation != instance->executor.Generation()) {
      return -2;
    }
    path = item->second.path;
  }
  bool okay = false;
  const std::string contents = ReadSmallFile(path, kUserFileLimit, &okay);
  if (!okay || contents.size() > value_capacity ||
      (!value && !contents.empty())) {
    return -3;
  }
  if (!contents.empty()) std::memcpy(value, contents.data(), contents.size());
  *value_length = contents.size();
  return 0;
}

std::int32_t RuntimeEngine::Impl::UserFileWrite(void* user_data,
                                                const char* grant_token,
                                                std::size_t grant_token_length,
                                                const std::uint8_t* value,
                                                std::size_t value_length) {
  auto* instance = static_cast<Instance*>(user_data);
  const std::string token = Text(grant_token, grant_token_length);
  if (!instance || !instance->owner || value_length > kUserFileLimit ||
      (!value && value_length != 0) ||
      !instance->owner->Permitted(*instance, "storage.user-selected") ||
      !instance->enabled || instance->failed) {
    return -1;
  }
  fs::path path;
  {
    std::lock_guard<std::mutex> lock(instance->state_mutex);
    const auto item = instance->user_file_grants.find(token);
    if (item == instance->user_file_grants.end() || !item->second.writable ||
        item->second.consumed ||
        item->second.generation != instance->executor.Generation()) {
      return -2;
    }
    item->second.consumed = true;
    path = item->second.path;
  }
  std::array<unsigned char, 8> suffix{};
  if (RAND_bytes(suffix.data(), static_cast<int>(suffix.size())) != 1)
    return -3;
  std::string suffix_text;
  static constexpr char digits[] = "0123456789abcdef";
  for (const unsigned char byte : suffix) {
    suffix_text.push_back(digits[byte >> 4]);
    suffix_text.push_back(digits[byte & 0x0f]);
  }
  const fs::path temporary =
      path.parent_path() / (path.filename().string() + ".ppm-" + suffix_text);
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return -4;
    output.write(reinterpret_cast<const char*>(value),
                 static_cast<std::streamsize>(value_length));
    output.flush();
    if (!output) {
      std::error_code ignored;
      fs::remove(temporary, ignored);
      return -4;
    }
  }
  std::error_code error;
  fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace, error);
  error.clear();
  fs::rename(temporary, path, error);
  if (error) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return -5;
  }
  return 0;
}

void RuntimeEngine::Impl::Log(void* user_data, std::uint32_t level,
                              const char* message, std::size_t message_length) {
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
    action.icon_path =
        (instance->package_root / fs::path(resource.ToStdString()))
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

std::int32_t RuntimeEngine::Impl::SettingGet(void* user_data, const char* key,
                                             std::size_t key_length,
                                             char* value,
                                             std::size_t value_capacity,
                                             std::size_t* value_length,
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

std::int32_t RuntimeEngine::Impl::SettingSet(void* user_data, const char* key,
                                             std::size_t key_length,
                                             const char* value,
                                             std::size_t value_length) {
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
        points[index].longitude < -180.0 || points[index].longitude > 180.0) {
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

std::int32_t RuntimeEngine::Impl::EnvironmentSampleBatch(
    void* user_data, const ocpn_portable_environment_sample_request* requests,
    std::size_t request_count, ocpn_portable_environment_sample* results,
    std::size_t result_count, char* error, std::size_t error_capacity) {
  auto* consumer = static_cast<Instance*>(user_data);
  if (!consumer || !consumer->owner || request_count != result_count ||
      (!requests && request_count != 0) || (!results && result_count != 0) ||
      request_count > 100'000 ||
      !consumer->owner->Permitted(*consumer, "environment.consume")) {
    CopyError("invalid or unauthorized environmental sample batch", error,
              error_capacity);
    return -1;
  }
  const Instance* provider = consumer->owner->CompatibleProvider(
      *consumer, "org.opencpn.environment.provider");
  if (!provider || !provider->environment_provider) {
    CopyError("no enabled compatible environmental provider is available",
              error, error_capacity);
    return -2;
  }
  std::vector<EnvironmentRequest> input;
  input.reserve(request_count);
  for (std::size_t index = 0; index < request_count; ++index) {
    input.push_back({requests[index].latitude, requests[index].longitude,
                     requests[index].unix_time});
  }
  std::vector<EnvironmentSample> sampled;
  std::string diagnostic;
  if (!provider->environment_provider->SampleBatch(
          input, &sampled,
          [consumer]() {
            return !consumer->enabled || consumer->failed ||
                   consumer->routing_cancelled || !consumer->owner ||
                   consumer->owner->stopped;
          },
          &diagnostic) ||
      sampled.size() != request_count) {
    CopyError(diagnostic.empty()
                  ? "environmental provider returned an invalid batch"
                  : diagnostic,
              error, error_capacity);
    return -3;
  }
  for (std::size_t index = 0; index < result_count; ++index) {
    results[index].wind_u_knots = sampled[index].wind_u_knots;
    results[index].wind_v_knots = sampled[index].wind_v_knots;
    results[index].current_u_knots = sampled[index].current_u_knots;
    results[index].current_v_knots = sampled[index].current_v_knots;
    results[index].wave_height_metres = sampled[index].wave_height_metres;
    results[index].available =
        static_cast<std::uint8_t>(sampled[index].available);
  }
  return 0;
}

void RuntimeEngine::Impl::RoutingProgressCallback(
    void* user_data, std::uint8_t percent, const char* message,
    std::size_t message_length) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner || !instance->owner->routing_progress)
    return;
  const auto callback = instance->owner->routing_progress;
  const std::string id = instance->id;
  const std::string text = Text(message, message_length);
  instance->owner->Publish(
      [callback, id, percent, text]() { callback(id, percent, text); });
}

std::uint8_t RuntimeEngine::Impl::RoutingCancelled(void* user_data) {
  auto* instance = static_cast<Instance*>(user_data);
  return instance &&
                 (instance->routing_cancelled || !instance->enabled ||
                  instance->failed || !instance->owner ||
                  instance->owner->stopped)
             ? 1U
             : 0U;
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
  const std::vector<ocpn_portable_geo_segment> input(segments,
                                                     segments + segment_count);
  auto output =
      std::make_shared<std::vector<ocpn_portable_chart_segment_result>>(
          segment_count);
  if (!instance->owner->RunUiService(
          [input, output]() {
            for (std::size_t index = 0; index < input.size(); ++index) {
              const auto& segment = input[index];
              (*output)[index] = {
                  PlugIn_GSHHS_CrossesLand(
                      segment.start.latitude, segment.start.longitude,
                      segment.end.latitude, segment.end.longitude)
                      ? 1U
                      : 0U,
                  1U};
            }
          },
          std::chrono::seconds(5))) {
    wxLogWarning("PPM chart service timed out waiting for the UI thread");
    return -2;
  }
  std::copy(output->begin(), output->end(), results);
  return 0;
}

std::int32_t RuntimeEngine::Impl::StoragePrivateRead(
    void* user_data, const char* private_name, std::size_t private_name_length,
    std::uint8_t* value, std::size_t value_capacity,
    std::size_t* value_length) {
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
      std::find_if(instances.begin(), instances.end(),
                   [&](const auto& value) { return value->id == package_id; });
  return item == instances.end() ? nullptr : item->get();
}

const RuntimeEngine::Impl::Instance* RuntimeEngine::Impl::Find(
    const std::string& package_id) const {
  const auto item =
      std::find_if(instances.begin(), instances.end(),
                   [&](const auto& value) { return value->id == package_id; });
  return item == instances.end() ? nullptr : item->get();
}

bool RuntimeEngine::Impl::Start(Instance& instance, std::string* diagnostic) {
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
  for (const auto& [interface, range] : instance.required_services) {
    if (!CompatibleProvider(instance, interface)) {
      instance.diagnostic = "no enabled compatible provider for service " +
                            interface + " (" + range + ")";
      if (diagnostic) *diagnostic = instance.diagnostic;
      return false;
    }
  }
  if (instance.environment_provider) instance.environment_provider->Resume();
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
    callbacks.open_surface = OpenSurface;
    callbacks.open_environmental_viewer = OpenEnvironmentalViewer;
    callbacks.open_weather_routing = OpenWeatherRouting;
    callbacks.environment_sample_batch = EnvironmentSampleBatch;
    callbacks.routing_progress = RoutingProgressCallback;
    callbacks.routing_cancelled = RoutingCancelled;
    callbacks.charts_query_segments = ChartsQuerySegments;
    callbacks.network_get_to_private = NetworkGetToPrivate;
    callbacks.storage_private_read = StoragePrivateRead;
    callbacks.user_file_read = UserFileRead;
    callbacks.user_file_write = UserFileWrite;
    instance.runtime =
        ocpn_portable_runtime_create(instance.component_path.c_str(),
                                     &callbacks, error.data(), error.size());
    if (!instance.runtime ||
        ocpn_portable_runtime_initialize(
            instance.runtime, instance.id.data(), instance.id.size(),
            instance.name.data(), instance.name.size(), instance.version.data(),
            instance.version.size(), error.data(), error.size()) != 0) {
      remove_actions(instance.id);
      if (instance.runtime) ocpn_portable_runtime_destroy(instance.runtime);
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
    instance.diagnostic = error[0] ? error.data() : "runtime enable failed";
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
  if (!stopped && instance.enabled) {
    for (const auto& [interface, version] : instance.provided_services) {
      for (const auto& candidate : instances) {
        if (candidate.get() == &instance || !candidate->enabled ||
            candidate->failed) {
          continue;
        }
        const auto requirement = candidate->required_services.find(interface);
        if (requirement != candidate->required_services.end() &&
            ServiceVersionSatisfies(version, requirement->second)) {
          const std::string message =
              "service " + interface + " is in use by enabled package " +
              candidate->id + "; disable the dependent package first";
          {
            std::lock_guard<std::mutex> state_lock(instance.state_mutex);
            instance.diagnostic = message;
          }
          if (diagnostic) *diagnostic = message;
          return false;
        }
      }
    }
  }
  if (instance.environment_provider) {
    instance.environment_provider->RequestStop();
  }
  instance.routing_cancelled = true;
  const bool was_enabled = instance.enabled.exchange(false);
  instance.executor.AdvanceGeneration();
  jobs.CancelOwner(instance.id);
  if (!WaitForRoute(instance.id, std::chrono::seconds(10))) {
    const std::string message =
        "weather routing did not reach its ten-second cancellation barrier";
    {
      std::lock_guard<std::mutex> state_lock(instance.state_mutex);
      instance.diagnostic = message;
    }
    remove_actions(instance.id);
    if (diagnostic) *diagnostic = message;
    return false;
  }
  if (!jobs.WaitOwnerIdle(instance.id, std::chrono::seconds(2))) {
    const std::string message =
        "portable jobs did not reach their two-second cancellation barrier";
    {
      std::lock_guard<std::mutex> state_lock(instance.state_mutex);
      instance.diagnostic = message;
    }
    remove_actions(instance.id);
    if (diagnostic) *diagnostic = message;
    return false;
  }
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
      instance.diagnostic = error[0] ? error.data() : "runtime disable failed";
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
    instance.user_file_grants.clear();
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
                                   bool activate, std::string* diagnostic) {
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
      manifest["permissions"].IsArray() && manifest["development"].IsBool();
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
  const std::set<std::string> known_services = {
      "org.opencpn.environment.provider"};
  auto read_services = [&](const char* member, const char* version_member,
                           std::map<std::string, std::string>* services) {
    if (!manifest.HasMember(member)) return true;
    const wxJSONValue declared = manifest[member];
    if (!declared.IsArray() || declared.Size() > 32) return false;
    for (int index = 0; index < declared.Size(); ++index) {
      const wxJSONValue service = declared.ItemAt(index);
      const wxJSONValue interface_value = service.ItemAt("interface");
      const wxJSONValue version_value = service.ItemAt(version_member);
      if (!service.IsObject() || !interface_value.IsString() ||
          !version_value.IsString()) {
        return false;
      }
      const std::string interface = interface_value.AsString().ToStdString();
      const std::string version = version_value.AsString().ToStdString();
      ServiceVersion parsed;
      const bool valid_version = std::string(version_member) == "version"
                                     ? ParseServiceVersion(version, &parsed)
                                     : IsServiceVersionRange(version);
      if (known_services.count(interface) == 0 || !valid_version ||
          !services->emplace(interface, version).second) {
        return false;
      }
    }
    return true;
  };
  if (!instance->failed &&
      (!read_services("provides", "version", &instance->provided_services) ||
       !read_services("requires", "range", &instance->required_services))) {
    instance->failed = true;
    instance->diagnostic =
        "manifest typed service metadata is invalid or unsupported";
  }
  if (!instance->failed && manifest.HasMember("surfaces")) {
    const wxJSONValue declared_surfaces = manifest["surfaces"];
    if (!declared_surfaces.IsObject() || declared_surfaces.Size() == 0 ||
        declared_surfaces.Size() > 16) {
      instance->failed = true;
      instance->diagnostic = "manifest surfaces are invalid or exceed policy";
    } else {
      const wxArrayString names = declared_surfaces.GetMemberNames();
      for (const auto& name : names) {
        const std::string surface_id = name.ToStdString();
        const wxJSONValue resource_value = declared_surfaces.ItemAt(name);
        if (!IsSafeName(surface_id) || !resource_value.IsString() ||
            !SafeRelativePath(resource_value.AsString())) {
          instance->failed = true;
          instance->diagnostic = "manifest surface identity/path is invalid";
          break;
        }
        const fs::path surface_path =
            (root / resource_value.AsString().ToStdString()).lexically_normal();
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
  if (!instance->failed && instance->provided_services.count(
                               "org.opencpn.environment.provider") != 0) {
    instance->environment_provider = std::make_unique<EnvironmentProvider>(
        instance->package_root.string(), instance->private_root.string());
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
  const bool result = LoadRoot(root, developer_mode, false, diagnostic);
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
        *diagnostic =
            "grant contains an unknown, unrequested or duplicate "
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

void RuntimeEngine::Impl::Fail(Instance& instance, const std::string& operation,
                               const std::string& diagnostic) {
  instance.failed = true;
  instance.enabled = false;
  instance.routing_cancelled = true;
  instance.executor.AdvanceGeneration();
  jobs.CancelOwner(instance.id);
  WaitForRoute(instance.id, std::chrono::seconds(10));
  {
    std::lock_guard<std::mutex> state_lock(instance.state_mutex);
    instance.diagnostic = operation + ": " + diagnostic;
    instance.scenes.clear();
    instance.user_file_grants.clear();
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
      instances.begin(), instances.end(),
      [&](const auto& candidate) { return candidate->id == package_id; });
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
        if (ocpn_portable_runtime_on_action(instance.runtime, action_id.data(),
                                            action_id.size(), error.data(),
                                            error.size()) != 0) {
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

bool RuntimeEngine::Impl::HandleSurfaceEvent(const std::string& package_id,
                                             const std::string& surface_id,
                                             const std::string& control_id,
                                             const std::string& value_json) {
  Instance* instance = Find(package_id);
  if (!instance || !instance->enabled || instance->failed ||
      !instance->runtime || !IsSafeName(surface_id) ||
      !IsSafeName(control_id) || value_json.size() > kSurfaceStateLimit ||
      instance->surfaces.count(surface_id) == 0) {
    return false;
  }
  const std::uint64_t generation = instance->executor.Generation();
  const auto posted = instance->executor.Post(
      generation, [instance, surface_id, control_id,
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
            value_json.size(), state.data(), kSurfaceStateLimit, &state_length,
            error.data(), error.size());
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
        instance->owner->Publish([response, id, surface_id, control_id,
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
  if (!instance) return false;
  const auto started = std::chrono::steady_clock::now();
  if (!jobs.WaitOwnerIdle(package_id, timeout)) return false;
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  if (!instance->executor.WaitIdle(
          elapsed >= timeout ? std::chrono::milliseconds(0)
                             : timeout - elapsed)) {
    return false;
  }
  elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  return WaitForRoute(package_id, elapsed >= timeout
                                      ? std::chrono::milliseconds(0)
                                      : timeout - elapsed);
}

void RuntimeEngine::Impl::DeliverNavigationSentence(
    const std::string& sentence) {
  if (sentence.empty() || sentence.size() > 1024 ||
      (sentence.front() != '$' && sentence.front() != '!') ||
      !std::all_of(sentence.begin(), sentence.end(), [](unsigned char value) {
        return value == '\r' || value == '\n' ||
               (value >= 0x20 && value <= 0x7e);
      })) {
    return;
  }
  for (auto& item : instances) {
    Instance* instance = item.get();
    if (!instance->enabled || instance->failed || !instance->runtime ||
        !Permitted(*instance, "navigation.nmea.read")) {
      continue;
    }
    const std::uint64_t generation = instance->executor.Generation();
    instance->executor.Post(
        generation, [instance, sentence](std::uint64_t task_generation) {
          if (task_generation != instance->executor.Generation() ||
              !instance->enabled || instance->failed) {
            return;
          }
          std::lock_guard<std::mutex> lock(instance->runtime_mutex);
          if (!instance->runtime ||
              task_generation != instance->executor.Generation()) {
            return;
          }
          std::array<char, kErrorCapacity> error{};
          if (ocpn_portable_runtime_on_navigation_sentence(
                  instance->runtime, sentence.data(), sentence.size(),
                  error.data(), error.size()) != 0) {
            instance->owner->Fail(
                *instance, "navigation sentence",
                error[0] ? error.data()
                         : "portable navigation sentence handler failed");
          }
        });
  }
}

void RuntimeEngine::Impl::Shutdown() {
  if (stopped) return;
  stopped = true;
  for (auto& instance : instances) {
    std::string ignored;
    Stop(*instance, true, &ignored);
  }
  jobs.Shutdown();
  state_changed();
}

RuntimeEngine::RuntimeEngine(std::string storage_root,
                             RegisterAction register_action,
                             RemoveActions remove_actions,
                             StateChanged state_changed, UiDispatch ui_dispatch)
    : impl_(std::make_unique<Impl>(
          storage_root, std::move(register_action), std::move(remove_actions),
          std::move(state_changed), std::move(ui_dispatch))),
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

void RuntimeEngine::SetRoutingProgressCallback(RoutingProgress callback) {
  impl_->SetRoutingProgressCallback(std::move(callback));
}

void RuntimeEngine::SetRoutingCompletedCallback(RoutingCompleted callback) {
  impl_->SetRoutingCompletedCallback(std::move(callback));
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

bool RuntimeEngine::HandleSurfaceEvent(const std::string& package_id,
                                       const std::string& surface_id,
                                       const std::string& control_id,
                                       const std::string& value_json) {
  return impl_->HandleSurfaceEvent(package_id, surface_id, control_id,
                                   value_json);
}

bool RuntimeEngine::RegisterUserFileGrant(const std::string& package_id,
                                          const std::string& path,
                                          bool writable, std::string* token,
                                          std::string* diagnostic) {
  return impl_->RegisterUserFileGrant(package_id, path, writable, token,
                                      diagnostic);
}

bool RuntimeEngine::SelectEnvironmentDataset(
    const std::string& package_id,
    const std::vector<std::string>& selected_paths) {
  return impl_->SelectEnvironmentDataset(package_id, selected_paths);
}

std::string RuntimeEngine::EnvironmentSummary(
    const std::string& package_id) const {
  return impl_->EnvironmentSummary(package_id);
}

bool RuntimeEngine::StartRoute(const std::string& package_id,
                               RoutingRequest request,
                               std::string* diagnostic) {
  return impl_->StartRoute(package_id, std::move(request), diagnostic);
}

bool RuntimeEngine::CancelRoute(const std::string& package_id) {
  return impl_->CancelRoute(package_id);
}

bool RuntimeEngine::WaitForRoute(const std::string& package_id,
                                 std::chrono::milliseconds timeout) {
  return impl_->WaitForRoute(package_id, timeout);
}

bool RuntimeEngine::WaitForIdle(const std::string& package_id,
                                std::chrono::milliseconds timeout) {
  return impl_->WaitForIdle(package_id, timeout);
}

void RuntimeEngine::SetPositionFix(const PlugIn_Position_Fix_Ex& fix) {
  std::lock_guard<std::mutex> lock(impl_->position_mutex);
  impl_->position_valid = std::isfinite(fix.Lat) && std::isfinite(fix.Lon) &&
                          fix.Lat >= -90.0 && fix.Lat <= 90.0 &&
                          fix.Lon >= -180.0 && fix.Lon <= 180.0;
  impl_->latitude = fix.Lat;
  impl_->longitude = fix.Lon;
  impl_->has_cog = std::isfinite(fix.Cog);
  impl_->has_sog = std::isfinite(fix.Sog);
  impl_->cog = impl_->has_cog ? fix.Cog : 0.0;
  impl_->sog = impl_->has_sog ? fix.Sog : 0.0;
}

void RuntimeEngine::DeliverNavigationSentence(const std::string& sentence) {
  impl_->DeliverNavigationSentence(sentence);
}

std::vector<PackageSnapshot> RuntimeEngine::Packages() const {
  std::vector<PackageSnapshot> result;
  result.reserve(impl_->instances.size());
  const auto jobs = impl_->jobs.Snapshots();
  for (const auto& instance : impl_->instances) {
    std::string diagnostic;
    {
      std::lock_guard<std::mutex> lock(instance->state_mutex);
      diagnostic = instance->diagnostic;
    }
    result.push_back({instance->id, instance->name, instance->version,
                      instance->failed    ? "Failed"
                      : instance->enabled ? "Enabled"
                      : instance->runtime ? "Disabled"
                                          : "Unloaded",
                      "Unknown", diagnostic, instance->executor.Generation(),
                      instance->executor.Pending(),
                      instance->enable_count.load(),
                      instance->disable_count.load()});
    result.back().surface_count = instance->surfaces.size();
    result.back().provided_service_count = instance->provided_services.size();
    result.back().required_service_count = instance->required_services.size();
    result.back().job_count = static_cast<std::size_t>(std::count_if(
        jobs.begin(), jobs.end(),
        [&](const auto& job) { return job.owner == instance->id; }));
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
