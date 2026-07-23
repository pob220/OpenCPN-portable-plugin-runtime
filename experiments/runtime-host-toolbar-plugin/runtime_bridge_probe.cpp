#include "runtime_bridge_probe.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>

#include <wx/log.h>

namespace {
std::string Text(const char* value, size_t length) {
  return value ? std::string(value, length) : std::string();
}

void Log(void*, uint32_t level, const char* message, size_t length) {
  wxLogMessage("RUNTIME_HOST_WASMTIME event=guest-log level=%u message=%s",
               level, Text(message, length));
}

int32_t RegisterAction(void* data, const char* action_id, size_t action_id_len,
                       const char*, size_t, const char*, size_t,
                       const char*, size_t, uint32_t* host_action_id) {
  auto& probe = *static_cast<RuntimeBridgeProbe*>(data);
  ++probe.action_count;
  *host_action_id = 2000 + probe.action_count;
  wxLogMessage("RUNTIME_HOST_WASMTIME event=guest-action id=%s host-id=%u",
               Text(action_id, action_id_len), *host_action_id);
  return 0;
}

int32_t GetVesselPosition(void*, double* latitude, double* longitude,
                          double* cog, uint8_t* has_cog, double* sog,
                          uint8_t* has_sog) {
  *latitude = 50.0;
  *longitude = -4.0;
  *cog = 92.0;
  *has_cog = 1;
  *sog = 6.5;
  *has_sog = 1;
  return 0;
}

int32_t SettingGet(void* data, const char* key, size_t key_len, char* value,
                   size_t capacity, size_t* value_len, uint8_t* found) {
  auto& settings = static_cast<RuntimeBridgeProbe*>(data)->settings;
  auto it = settings.find(Text(key, key_len));
  if (it == settings.end()) {
    *found = 0;
    *value_len = 0;
    return 0;
  }
  *found = 1;
  *value_len = it->second.size();
  if (*value_len > capacity) return -2;
  std::memcpy(value, it->second.data(), *value_len);
  return 0;
}

int32_t SettingSet(void* data, const char* key, size_t key_len,
                   const char* value, size_t value_len) {
  static_cast<RuntimeBridgeProbe*>(data)->settings[Text(key, key_len)] =
      Text(value, value_len);
  return 0;
}

int32_t SubmitPolyline(void* data, const char*, size_t,
                       const ocpn_portable_geo_point* points, size_t count,
                       ocpn_portable_overlay_style) {
  auto& probe = *static_cast<RuntimeBridgeProbe*>(data);
  ++probe.scene_count;
  probe.points.assign(points, points + count);
  return 0;
}

int32_t ClearScene(void*, const char*, size_t) { return 0; }

int32_t StartJob(void* data, const char*, size_t, uint32_t) {
  auto* probe = static_cast<RuntimeBridgeProbe*>(data);
  ++probe->job_count;
  probe->StartBoundedJob();
  return 0;
}

int32_t CancelJob(void*, const char*, size_t) { return 0; }
int32_t OpenEnvironmentalViewer(void*) { return 0; }
int32_t OpenWeatherRouting(void*) { return 0; }

int32_t EnvironmentSampleBatch(void*,
                               const ocpn_portable_environment_sample_request*,
                               size_t count,
                               ocpn_portable_environment_sample* results,
                               size_t result_count, char*, size_t) {
  if (count != result_count) return -1;
  for (size_t i = 0; i < count; ++i) results[i] = {8.0, 2.0, 0.4, 0.1, 1.2, 7};
  return 0;
}

void RoutingProgress(void*, uint8_t, const char*, size_t) {}
uint8_t RoutingCancelled(void*) { return 0; }

int32_t ChartsQuerySegments(void*, const ocpn_portable_geo_segment*,
                            size_t count,
                            ocpn_portable_chart_segment_result* results,
                            size_t result_count) {
  if (count != result_count) return -1;
  for (size_t i = 0; i < count; ++i) results[i] = {0, 3};
  return 0;
}

int32_t NetworkGetToPrivate(void*, const char*, size_t, const char*, size_t,
                            const char*, size_t, uint64_t) {
  return 0;
}

int32_t StoragePrivateRead(void*, const char*, size_t, uint8_t*, size_t,
                           size_t* value_len) {
  *value_len = 0;
  return 0;
}

int32_t OpenSurface(void*, const char*, size_t) { return 0; }
int32_t UserFileRead(void*, const char*, size_t, uint8_t*, size_t,
                     size_t* value_len) {
  *value_len = 0;
  return 0;
}
int32_t UserFileWrite(void*, const char*, size_t, const uint8_t*, size_t) {
  return 0;
}

ocpn_portable_host_callbacks Callbacks(RuntimeBridgeProbe* probe) {
  return {OCPN_PORTABLE_HOST_ABI_VERSION,
          probe,
          Log,
          RegisterAction,
          GetVesselPosition,
          SettingGet,
          SettingSet,
          SubmitPolyline,
          ClearScene,
          StartJob,
          CancelJob,
          OpenSurface,
          OpenEnvironmentalViewer,
          OpenWeatherRouting,
          EnvironmentSampleBatch,
          RoutingProgress,
          RoutingCancelled,
          ChartsQuerySegments,
          NetworkGetToPrivate,
          StoragePrivateRead,
          UserFileRead,
          UserFileWrite};
}
}  // namespace

RuntimeBridgeProbe::~RuntimeBridgeProbe() { Stop(); }

void RuntimeBridgeProbe::StartBoundedJob() {
  StopBoundedJob();
  cancel_job_ = false;
  job_thread_ = std::thread([this] {
    wxLogMessage("RUNTIME_HOST_JOB event=worker-start ui-thread=0");
    for (unsigned progress = 0; progress != 600; ++progress) {
      if (cancel_job_.load()) {
        wxLogMessage("RUNTIME_HOST_JOB event=cancelled progress=%u", progress);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    wxLogMessage("RUNTIME_HOST_JOB event=complete");
  });
}

void RuntimeBridgeProbe::StopBoundedJob() {
  if (!job_thread_.joinable()) return;
  const auto started = std::chrono::steady_clock::now();
  cancel_job_ = true;
  job_thread_.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  wxLogMessage("RUNTIME_HOST_JOB event=joined elapsed-ms=%lld",
               static_cast<long long>(elapsed.count()));
}

bool RuntimeBridgeProbe::Start(const std::string& component_path) {
  Stop();
  std::array<char, 4096> error{};
  auto callbacks = Callbacks(this);
  runtime_ = ocpn_portable_runtime_create(component_path.c_str(), &callbacks,
                                           error.data(), error.size());
  if (!runtime_) {
    wxLogMessage("RUNTIME_HOST_WASMTIME event=create-failed error=%s",
                 error.data());
    return false;
  }

  const std::string id = "org.opencpn.igrib";
  const std::string name = "iGRIB";
  const std::string version = "0.1.0";
  int32_t result = ocpn_portable_runtime_initialize(
      runtime_, id.data(), id.size(), name.data(), name.size(), version.data(),
      version.size(), error.data(), error.size());
  if (result == 0)
    result = ocpn_portable_runtime_enable(runtime_, error.data(), error.size());
  const std::string action = "igrib.toggle";
  if (result == 0)
    result = ocpn_portable_runtime_on_action(
        runtime_, action.data(), action.size(), error.data(), error.size());
  if (result != 0) {
    wxLogMessage("RUNTIME_HOST_WASMTIME event=lifecycle-failed error=%s",
                 error.data());
    Stop();
    return false;
  }
  wxLogMessage(
      "RUNTIME_HOST_WASMTIME event=enabled actions=%u scenes=%u jobs=%u settings=%zu",
      action_count, scene_count, job_count, settings.size());
  return action_count == 3 && scene_count >= 1 && job_count >= 1;
}

void RuntimeBridgeProbe::Stop() {
  StopBoundedJob();
  if (!runtime_) return;
  std::array<char, 4096> error{};
  const int32_t result =
      ocpn_portable_runtime_disable(runtime_, error.data(), error.size());
  wxLogMessage("RUNTIME_HOST_WASMTIME event=disabled result=%d error=%s",
               result, error.data());
  ocpn_portable_runtime_destroy(runtime_);
  runtime_ = nullptr;
  wxLogMessage("RUNTIME_HOST_WASMTIME event=destroyed");
}
