#include "ocpn_portable_runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <thread>
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
  bool weather_routing_opened = false;
  std::atomic<bool> routing_cancelled{false};
  std::atomic<unsigned> routing_progress_events{0};
  std::atomic<size_t> chart_segments_queried{0};
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

int32_t OpenWeatherRouting(void* data) {
  static_cast<HostState*>(data)->weather_routing_opened = true;
  return 0;
}

int32_t EnvironmentSampleBatch(void*,
                               const ocpn_portable_environment_sample_request*,
                               size_t count,
                               ocpn_portable_environment_sample* results,
                               size_t result_count) {
  if (count != result_count) return -1;
  for (size_t i = 0; i < count; ++i) results[i] = {8.0, 2.0, 0.4, 0.1, 1.2, 7};
  return 0;
}

void RoutingProgress(void* data, uint8_t, const char*, size_t) {
  ++static_cast<HostState*>(data)->routing_progress_events;
}
uint8_t RoutingCancelled(void* data) {
  return static_cast<HostState*>(data)->routing_cancelled.load() ? 1 : 0;
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
          OpenWeatherRouting,
          EnvironmentSampleBatch,
          RoutingProgress,
          RoutingCancelled,
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
  ok = ok && state.action_icons[0] == "resources/igrib.svg";
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

bool RoutingLifecycle(const char* component_path) {
  HostState state;
  auto callbacks = Callbacks(&state);
  char error[4096] = {};
  auto* runtime = ocpn_portable_runtime_create(component_path, &callbacks,
                                               error, sizeof(error));
  if (!runtime) return false;
  const std::string id = "org.opencpn.iweather-routing";
  const std::string name = "iWeatherRouting";
  const std::string version = "0.1.0";
  bool ok =
      CallSucceeded(ocpn_portable_runtime_initialize(
                        runtime, id.data(), id.size(), name.data(), name.size(),
                        version.data(), version.size(), error, sizeof(error)),
                    "routing initialize", error);
  ok = ok && CallSucceeded(
                 ocpn_portable_runtime_enable(runtime, error, sizeof(error)),
                 "routing enable", error);
  const std::string action = "iweather-routing.open";
  ok = ok && CallSucceeded(ocpn_portable_runtime_on_action(
                               runtime, action.data(), action.size(), error,
                               sizeof(error)),
                           "routing action", error);
  ok = ok && state.actions.size() == 1 &&
       state.actions[0] == "iweather-routing.open" &&
       state.action_icons[0] == "resources/iweather-routing.svg" &&
       state.weather_routing_opened;
  ocpn_portable_route_request request{};
  const std::string polar_identity = "Nicholson 35 Mk1 test subset";
  const std::array<double, 4> polar_winds = {0.0, 10.0, 20.0, 40.0};
  const std::array<double, 5> polar_angles = {0.0, 40.0, 90.0, 160.0, 180.0};
  const std::array<double, 20> polar_speeds = {
      0.0, 0.0,  0.0,  0.0,  0.0,  0.0, 3.86, 5.47, 3.92, 3.76,
      0.0, 3.98, 6.18, 5.30, 4.90, 0.0, 2.33, 3.99, 3.60, 3.33};
  ocpn_portable_polar_grid polar{polar_identity.data(), polar_identity.size(),
                                 polar_winds.data(),    polar_winds.size(),
                                 polar_angles.data(),   polar_angles.size(),
                                 polar_speeds.data(),   polar_speeds.size()};
  request.start_latitude = 50.0;
  request.start_longitude = -4.0;
  request.destination_latitude = 50.05;
  request.destination_longitude = -3.95;
  request.departure_unix_time = 1780000000;
  request.polars = &polar;
  request.polar_count = 1;
  request.time_step_seconds = 3600;
  request.heading_step_degrees = 15;
  request.max_hours = 24;
  request.max_states = 10000;
  request.avoid_unsafe_charts = 1;
  request.min_true_wind_angle_degrees = 40.0;
  request.max_true_wind_angle_degrees = 160.0;
  request.max_wind_knots = 35.0;
  request.max_apparent_wind_knots = 50.0;
  request.max_wave_metres = 4.0;
  request.maximum_latitude_degrees = 89.0;
  request.upwind_efficiency = 1.0;
  request.downwind_efficiency = 1.0;
  request.maximum_search_angle_degrees = 120.0;
  request.destination_tolerance_nm = 1.0;
  request.tack_penalty_seconds = 60;
  request.gybe_penalty_seconds = 60;
  request.use_currents = 1;
  request.require_current_data = 1;
  request.use_waves = 1;
  request.require_wave_data = 1;
  request.limits_available = 7;
  std::vector<ocpn_portable_route_point> points(1000);
  std::vector<ocpn_portable_route_point> isochrone_points(20000);
  std::vector<ocpn_portable_route_line> isochrone_lines(2000);
  std::vector<ocpn_portable_route_point> trace_points(20000);
  std::vector<ocpn_portable_route_line> trace_lines(2000);
  char diagnostic[4096] = {};
  ocpn_portable_route_result result{};
  result.points = points.data();
  result.point_capacity = points.size();
  result.isochrone_points = isochrone_points.data();
  result.isochrone_point_capacity = isochrone_points.size();
  result.isochrones = isochrone_lines.data();
  result.isochrone_capacity = isochrone_lines.size();
  result.trace_points = trace_points.data();
  result.trace_point_capacity = trace_points.size();
  result.traces = trace_lines.data();
  result.trace_capacity = trace_lines.size();
  result.diagnostic = diagnostic;
  result.diagnostic_capacity = sizeof(diagnostic);
  ok =
      ok && CallSucceeded(ocpn_portable_runtime_calculate_route(
                              runtime, &request, &result, error, sizeof(error)),
                          "calculate route", error);
  ok = ok && result.point_count >= 2 && result.states_examined > 0 &&
       result.duration_seconds > 0 && result.diagnostic_len > 0 &&
       state.routing_progress_events > 0 && result.average_speed_knots > 0.0 &&
       result.average_sog_knots > 0.0 && result.maximum_sog_knots > 0.0 &&
       result.average_wind_knots > 8.0 && result.maximum_wind_knots > 8.0 &&
       (result.metrics_available & 1) != 0 &&
       result.average_current_knots > 0.0 && result.comfort_level >= 1 &&
       result.comfort_level <= 3;

  // A route requiring several forecast steps must expose bounded retained
  // isochrones and predecessor traces for host-side inspection rendering.
  auto inspection_request = request;
  inspection_request.destination_latitude = 50.2;
  inspection_request.destination_longitude = -3.8;
  result.point_count = 0;
  result.isochrone_point_count = 0;
  result.isochrone_count = 0;
  result.trace_point_count = 0;
  result.trace_count = 0;
  result.diagnostic_len = 0;
  ok = ok && CallSucceeded(ocpn_portable_runtime_calculate_route(
                               runtime, &inspection_request, &result, error,
                               sizeof(error)),
                           "calculate route with inspection geometry", error);
  ok = ok && result.isochrone_count > 0 && result.trace_count > 0 &&
       result.isochrone_point_count <= isochrone_points.size() &&
       result.trace_point_count <= trace_points.size();
  for (size_t index = 0; ok && index < result.isochrone_count; ++index) {
    const auto& line = isochrone_lines[index];
    ok = line.point_count >= 2 &&
         line.point_offset + line.point_count <= result.isochrone_point_count;
  }
  for (size_t index = 0; ok && index < result.trace_count; ++index) {
    const auto& line = trace_lines[index];
    ok = line.point_count >= 2 &&
         line.point_offset + line.point_count <= result.trace_point_count;
  }

  result.point_count = 0;
  result.isochrone_point_count = 0;
  result.isochrone_count = 0;
  result.trace_point_count = 0;
  result.trace_count = 0;
  result.diagnostic_len = 0;
  ok =
      ok && CallSucceeded(ocpn_portable_runtime_calculate_route(
                              runtime, &request, &result, error, sizeof(error)),
                          "restore baseline route", error);

  // The guest must use the supplied polar rather than a synthetic reference
  // speed. A conservative factor must produce a later arrival for the same
  // route and environmental samples.
  const uint64_t normal_duration = result.duration_seconds;
  auto conservative_speeds = polar_speeds;
  for (double& speed : conservative_speeds) speed *= 0.5;
  polar.boat_speeds_knots = conservative_speeds.data();
  result.point_count = 0;
  result.diagnostic_len = 0;
  ok =
      ok && CallSucceeded(ocpn_portable_runtime_calculate_route(
                              runtime, &request, &result, error, sizeof(error)),
                          "calculate route with conservative polar", error);
  ok = ok && result.duration_seconds > normal_duration;
  polar.boat_speeds_knots = polar_speeds.data();

  // True-wind-angle bounds apply to candidate headings, not wind speed. This
  // deliberately narrow sector excludes every heading in the search and must
  // fail in the guest without trapping or affecting the host process.
  auto rejected_request = request;
  rejected_request.min_true_wind_angle_degrees = 0.0;
  rejected_request.max_true_wind_angle_degrees = 5.0;
  std::memset(error, 0, sizeof(error));
  const int32_t rejected = ocpn_portable_runtime_calculate_route(
      runtime, &rejected_request, &result, error, sizeof(error));
  ok = ok && rejected != 0 &&
       std::strstr(error, "environmental limits") != nullptr;

  auto invalid_angles = request;
  invalid_angles.min_true_wind_angle_degrees = 170.0;
  invalid_angles.max_true_wind_angle_degrees = 40.0;
  std::memset(error, 0, sizeof(error));
  const int32_t invalid = ocpn_portable_runtime_calculate_route(
      runtime, &invalid_angles, &result, error, sizeof(error));
  ok = ok && invalid != 0 && std::strstr(error, "true-wind-angle") != nullptr;

  // Compute replicas share the compiled component and explicit host
  // capabilities, but own an independent Wasmtime Store. They do not repeat
  // lifecycle registration and can be used concurrently by the host.
  std::memset(error, 0, sizeof(error));
  auto* replica =
      ocpn_portable_runtime_clone_compute(runtime, error, sizeof(error));
  ok = ok && replica != nullptr;
  if (replica) {
    result.point_count = 0;
    result.diagnostic_len = 0;
    ok = ok &&
         CallSucceeded(ocpn_portable_runtime_calculate_route(
                           replica, &request, &result, error, sizeof(error)),
                       "calculate route in replica", error);
    ok = ok && result.point_count >= 2 && state.actions.size() == 1;
    ocpn_portable_runtime_destroy(replica);
  }

  std::array<ocpn_portable_runtime*, 4> replicas{};
  std::array<bool, 4> replica_results{};
  for (auto& compute_runtime : replicas) {
    std::memset(error, 0, sizeof(error));
    compute_runtime =
        ocpn_portable_runtime_clone_compute(runtime, error, sizeof(error));
    ok = ok && compute_runtime != nullptr;
  }
  std::array<std::thread, 4> threads;
  for (size_t index = 0; index < replicas.size(); ++index) {
    threads[index] = std::thread([&, index] {
      if (!replicas[index]) return;
      std::vector<ocpn_portable_route_point> local_points(1000);
      std::vector<ocpn_portable_route_point> local_isochrone_points(20000);
      std::vector<ocpn_portable_route_line> local_isochrone_lines(2000);
      std::vector<ocpn_portable_route_point> local_trace_points(20000);
      std::vector<ocpn_portable_route_line> local_trace_lines(2000);
      char local_diagnostic[4096] = {};
      char local_error[4096] = {};
      ocpn_portable_route_result local_result{};
      local_result.points = local_points.data();
      local_result.point_capacity = local_points.size();
      local_result.isochrone_points = local_isochrone_points.data();
      local_result.isochrone_point_capacity = local_isochrone_points.size();
      local_result.isochrones = local_isochrone_lines.data();
      local_result.isochrone_capacity = local_isochrone_lines.size();
      local_result.trace_points = local_trace_points.data();
      local_result.trace_point_capacity = local_trace_points.size();
      local_result.traces = local_trace_lines.data();
      local_result.trace_capacity = local_trace_lines.size();
      local_result.diagnostic = local_diagnostic;
      local_result.diagnostic_capacity = sizeof(local_diagnostic);
      replica_results[index] = ocpn_portable_runtime_calculate_route(
                                   replicas[index], &request, &local_result,
                                   local_error, sizeof(local_error)) == 0 &&
                               local_result.point_count >= 2;
    });
  }
  for (auto& thread : threads) thread.join();
  for (auto* compute_runtime : replicas)
    ocpn_portable_runtime_destroy(compute_runtime);
  ok = ok &&
       std::all_of(replica_results.begin(), replica_results.end(),
                   [](bool result) { return result; }) &&
       state.actions.size() == 1;
  state.routing_cancelled = true;
  std::memset(error, 0, sizeof(error));
  const int32_t cancelled = ocpn_portable_runtime_calculate_route(
      runtime, &request, &result, error, sizeof(error));
  ok = ok && cancelled != 0 && std::strstr(error, "cancelled") != nullptr;
  ok = ok && CallSucceeded(
                 ocpn_portable_runtime_disable(runtime, error, sizeof(error)),
                 "routing disable", error);
  ocpn_portable_runtime_destroy(runtime);
  if (!ok) std::cerr << "portable routing lifecycle failed: " << error << '\n';
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: portable_runtime_smoke_test <igrib.wasm> "
                 "<iweather-routing.wasm>\n";
    return 2;
  }
  if (!NormalLifecycle(argv[1]) || !TrapIsContained(argv[1]) ||
      !IdentityMismatchIsRejected(argv[1]) || !RoutingLifecycle(argv[2]))
    return 1;
  std::cout << "portable runtime smoke test passed\n";
  return 0;
}
