#include "ocpn_portable_runtime.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

struct HostState {
  std::vector<std::string> actions;
  std::vector<std::string> action_icons;
  std::vector<std::string> logs;
  std::map<std::string, std::string> settings;
  std::string scene_id;
  std::vector<ocpn_portable_geo_point> points;
  std::string job_id;
  uint32_t work_units = 0;
  bool scene_cleared = false;
  bool job_cancelled = false;
  bool environmental_viewer_opened = false;
  size_t chart_segments_queried = 0;
  std::string network_request_id;
  std::string network_url;
  std::string network_private_name;
  uint64_t network_max_bytes = 0;
};

std::string Text(const char* value, size_t length) {
  return value ? std::string(value, length) : std::string();
}

void Log(void* data, uint32_t, const char* message, size_t length) {
  static_cast<HostState*>(data)->logs.push_back(Text(message, length));
}

int32_t RegisterAction(void* data, const char* action_id, size_t action_id_len,
                       const char*, size_t, const char*, size_t,
                       const char* icon_resource, size_t icon_resource_len,
                       uint32_t* host_action_id) {
  auto& state = *static_cast<HostState*>(data);
  state.actions.push_back(Text(action_id, action_id_len));
  state.action_icons.push_back(Text(icon_resource, icon_resource_len));
  *host_action_id = static_cast<uint32_t>(1000 + state.actions.size());
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
                   size_t value_capacity, size_t* value_len, uint8_t* found) {
  auto& settings = static_cast<HostState*>(data)->settings;
  const auto item = settings.find(Text(key, key_len));
  if (item == settings.end()) {
    *found = 0;
    *value_len = 0;
    return 0;
  }
  *found = 1;
  *value_len = item->second.size();
  if (*value_len > value_capacity) return -2;
  std::memcpy(value, item->second.data(), *value_len);
  return 0;
}

int32_t SettingSet(void* data, const char* key, size_t key_len,
                   const char* value, size_t value_len) {
  static_cast<HostState*>(data)->settings[Text(key, key_len)] =
      Text(value, value_len);
  return 0;
}

int32_t SubmitPolyline(void* data, const char* scene_id, size_t scene_id_len,
                       const ocpn_portable_geo_point* points,
                       size_t point_count, ocpn_portable_overlay_style) {
  auto& state = *static_cast<HostState*>(data);
  state.scene_id = Text(scene_id, scene_id_len);
  state.points.assign(points, points + point_count);
  return 0;
}

int32_t ClearScene(void* data, const char* scene_id, size_t scene_id_len) {
  auto& state = *static_cast<HostState*>(data);
  state.scene_cleared = Text(scene_id, scene_id_len) == state.scene_id;
  return 0;
}

int32_t StartJob(void* data, const char* job_id, size_t job_id_len,
                 uint32_t work_units) {
  auto& state = *static_cast<HostState*>(data);
  state.job_id = Text(job_id, job_id_len);
  state.work_units = work_units;
  return 0;
}

int32_t CancelJob(void* data, const char* job_id, size_t job_id_len) {
  auto& state = *static_cast<HostState*>(data);
  state.job_cancelled = Text(job_id, job_id_len) == state.job_id;
  return 0;
}

int32_t OpenEnvironmentalViewer(void* data) {
  static_cast<HostState*>(data)->environmental_viewer_opened = true;
  return 0;
}

int32_t ChartsQuerySegments(void* data, const ocpn_portable_geo_segment*,
                            size_t segment_count,
                            ocpn_portable_chart_segment_result* results,
                            size_t result_count) {
  if (segment_count != result_count) return -1;
  static_cast<HostState*>(data)->chart_segments_queried = segment_count;
  for (size_t i = 0; i < result_count; ++i) results[i] = {0, 3};
  return 0;
}

int32_t NetworkGetToPrivate(void* data, const char* request_id,
                            size_t request_id_len, const char* url,
                            size_t url_len, const char* private_name,
                            size_t private_name_len, uint64_t max_bytes) {
  auto& state = *static_cast<HostState*>(data);
  state.network_request_id = Text(request_id, request_id_len);
  state.network_url = Text(url, url_len);
  state.network_private_name = Text(private_name, private_name_len);
  state.network_max_bytes = max_bytes;
  return 0;
}

int32_t StoragePrivateRead(void*, const char*, size_t, uint8_t* value,
                           size_t value_capacity, size_t* value_len) {
  constexpr char kPayload[] = "portable-host-http-ok";
  *value_len = sizeof(kPayload) - 1;
  if (value_capacity < *value_len) return -2;
  std::memcpy(value, kPayload, *value_len);
  return 0;
}

ocpn_portable_host_callbacks Callbacks(HostState* state) {
  return {OCPN_PORTABLE_HOST_ABI_VERSION,
          state,
          Log,
          RegisterAction,
          GetVesselPosition,
          SettingGet,
          SettingSet,
          SubmitPolyline,
          ClearScene,
          StartJob,
          CancelJob,
          OpenEnvironmentalViewer,
          ChartsQuerySegments,
          NetworkGetToPrivate,
          StoragePrivateRead};
}

bool CallSucceeded(int32_t result, const char* operation, const char* error) {
  if (result == 0) return true;
  std::cerr << operation << " failed: " << error << '\n';
  return false;
}

int32_t Initialize(ocpn_portable_runtime* runtime, char* error,
                   size_t error_capacity) {
  const std::string id = "org.opencpn.igrib";
  const std::string name = "iGRIB";
  const std::string version = "0.1.0";
  return ocpn_portable_runtime_initialize(
      runtime, id.data(), id.size(), name.data(), name.size(), version.data(),
      version.size(), error, error_capacity);
}

bool NormalLifecycle(const char* component_path) {
  HostState state;
  auto callbacks = Callbacks(&state);
  char error[4096] = {};
  auto* runtime = ocpn_portable_runtime_create(component_path, &callbacks,
                                               error, sizeof(error));
  if (!runtime) {
    std::cerr << "create failed: " << error << '\n';
    return false;
  }

  bool ok =
      CallSucceeded(Initialize(runtime, error, sizeof(error)), "initialize",
                    error) &&
      CallSucceeded(ocpn_portable_runtime_enable(runtime, error, sizeof(error)),
                    "enable", error);
  const std::string action = "igrib.toggle";
  ok = ok && CallSucceeded(ocpn_portable_runtime_on_action(
                               runtime, action.data(), action.size(), error,
                               sizeof(error)),
                           "on-action", error);

  ok = ok && state.actions.size() == 3;
  ok = ok && state.actions[0] == "igrib.toggle";
  ok = ok && state.actions[1] == "igrib.failure-test";
  ok = ok && state.actions[2] == "igrib.http-test";
  ok = ok && state.action_icons.size() == 3;
  ok = ok && state.action_icons[0] == "resources/grib.svg";
  ok = ok && state.action_icons[1] == "resources/fault-test.svg";
  ok = ok && state.action_icons[2] == "resources/http-download.svg";
  ok = ok && state.settings["activation-count"] == "1";
  ok = ok && state.scene_id == "igrib.weather-window";
  ok = ok && state.points.size() == 5;
  ok = ok && state.chart_segments_queried == 4;
  ok = ok && std::abs(state.points[0].latitude - 50.35) < 0.000001;
  ok = ok && std::abs(state.points[0].longitude + 4.55) < 0.000001;
  ok = ok && state.job_id == "igrib.prepare-weather";
  ok = ok && state.work_units == 40;
  ok = ok && state.environmental_viewer_opened;

  const char* empty = "";
  const std::string http_action = "igrib.http-test";
  ok = ok && CallSucceeded(ocpn_portable_runtime_on_action(
                               runtime, http_action.data(), http_action.size(),
                               error, sizeof(error)),
                           "http-action", error);
  ok = ok && state.network_request_id == "igrib.http-probe";
  ok = ok && state.network_url == "https://opencpn.org/";
  ok = ok && state.network_private_name == "http-probe.html";
  ok = ok && state.network_max_bytes == 1024 * 1024;
  ok = ok && CallSucceeded(ocpn_portable_runtime_on_job_event(
                               runtime, state.network_request_id.data(),
                               state.network_request_id.size(), 1, 100, empty,
                               0, error, sizeof(error)),
                           "http-completed", error);
  ok =
      ok && CallSucceeded(ocpn_portable_runtime_on_job_event(
                              runtime, state.job_id.data(), state.job_id.size(),
                              0, 50, empty, 0, error, sizeof(error)),
                          "job-progress", error);
  ok =
      ok && CallSucceeded(ocpn_portable_runtime_on_job_event(
                              runtime, state.job_id.data(), state.job_id.size(),
                              1, 100, empty, 0, error, sizeof(error)),
                          "job-completed", error);
  ok = ok && CallSucceeded(
                 ocpn_portable_runtime_disable(runtime, error, sizeof(error)),
                 "disable", error);
  ok = ok && state.scene_cleared && state.job_cancelled;
  ocpn_portable_runtime_destroy(runtime);

  if (!ok) std::cerr << "host callback state did not match expectations\n";
  return ok;
}

bool TrapIsContained(const char* component_path) {
  HostState state;
  auto callbacks = Callbacks(&state);
  char error[4096] = {};
  auto* runtime = ocpn_portable_runtime_create(component_path, &callbacks,
                                               error, sizeof(error));
  if (!runtime) {
    std::cerr << "trap-test create failed: " << error << '\n';
    return false;
  }
  bool ok = CallSucceeded(Initialize(runtime, error, sizeof(error)),
                          "trap-test initialize", error);
  const int32_t result =
      ocpn_portable_runtime_test_trap(runtime, error, sizeof(error));
  ok = ok && result != 0 && std::strlen(error) != 0;
  ocpn_portable_runtime_destroy(runtime);
  if (!ok) std::cerr << "component trap escaped or was not reported\n";
  return ok;
}

bool IdentityMismatchIsRejected(const char* component_path) {
  HostState state;
  auto callbacks = Callbacks(&state);
  char error[4096] = {};
  auto* runtime = ocpn_portable_runtime_create(component_path, &callbacks,
                                               error, sizeof(error));
  if (!runtime) {
    std::cerr << "identity-test create failed: " << error << '\n';
    return false;
  }
  const std::string wrong_id = "org.opencpn.not-igrib";
  const std::string name = "iGRIB";
  const std::string version = "0.1.0";
  const int32_t result = ocpn_portable_runtime_initialize(
      runtime, wrong_id.data(), wrong_id.size(), name.data(), name.size(),
      version.data(), version.size(), error, sizeof(error));
  const bool ok =
      result != 0 && std::strstr(error, "identity mismatch") != nullptr;
  ocpn_portable_runtime_destroy(runtime);
  if (!ok) std::cerr << "component/manifest identity mismatch was accepted\n";
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: portable_runtime_smoke_test <component.wasm>\n";
    return 2;
  }
  if (!NormalLifecycle(argv[1]) || !TrapIsContained(argv[1]) ||
      !IdentityMismatchIsRejected(argv[1]))
    return 1;
  std::cout << "portable runtime smoke test passed\n";
  return 0;
}
