#include "ocpn_portable_runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
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
  std::atomic<bool> environment_failure{false};
  std::atomic<int64_t> environment_unavailable_after{
      std::numeric_limits<int64_t>::max()};
  std::atomic<double> environment_unavailable_north_of{91.0};
  std::atomic<bool> use_reported_irish_sea_weather{false};
  std::atomic<bool> routing_cancelled{false};
  std::atomic<unsigned> routing_progress_events{0};
  std::atomic<bool> saw_corridor_refinement{false};
  std::atomic<bool> saw_graph_fallback{false};
  std::atomic<bool> saw_passage_progress{false};
  std::atomic<unsigned> routing_stage{0};
  std::atomic<bool> reject_reverse_charts{false};
  std::atomic<size_t> chart_segments_queried{0};
  std::atomic<size_t> chart_query_calls{0};
  std::atomic<size_t> reject_chart_query_call{0};
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

int32_t EnvironmentSampleBatch(
    void* data, const ocpn_portable_environment_sample_request* requests,
    size_t count, ocpn_portable_environment_sample* results,
    size_t result_count, char* error, size_t error_capacity) {
  if (static_cast<HostState*>(data)->environment_failure.load()) {
    constexpr char kFailure[] = "decoder deliberately unavailable";
    if (error && error_capacity) {
      const size_t copied = std::min(error_capacity - 1, sizeof(kFailure) - 1);
      std::memcpy(error, kFailure, copied);
      error[copied] = '\0';
    }
    return -2;
  }
  if (count != result_count) return -1;
  if (static_cast<HostState*>(data)->use_reported_irish_sea_weather.load()) {
    // Hourly samples decoded from the reported immutable Irish Sea GRIB at
    // 53.32 N, 5.36 W, beginning 2026-07-23 08:00 UTC. Values are SI in the
    // file and converted here to the host ABI's knots. Linear interpolation
    // reproduces the wind shifts and reversing tide which exposed the former
    // start-of-leg versus midpoint-validation mismatch.
    static constexpr int64_t kEpoch = 1784793600;
    static constexpr double kKnotsPerMetreSecond = 1.9438444924406;
    static constexpr std::array<double, 23> kWindU = {
        0.783520,  0.763138,  1.431503,  1.950100,  0.898796,  0.471714,
        0.517817,  -0.692440, -1.552264, -1.476562, -1.724635, -2.077554,
        -1.289976, -0.817876, -1.119772, -1.519404, -1.683104, -1.081521,
        0.292825,  0.686308,  0.912189,  1.394554,  1.026602};
    static constexpr std::array<double, 23> kWindV = {
        -3.657707, -3.210291, -3.593356, -3.235740, -3.477012, -3.071839,
        -2.911352, -2.885527, -3.489727, -2.168231, -1.542068, -1.984826,
        -2.531719, -2.915737, -3.394135, -2.845692, -3.171215, -2.421723,
        -1.401065, -1.034437, -1.207748, -0.116391, 0.631812};
    static constexpr std::array<double, 23> kCurrentU = {
        0.072664,  0.118496,  0.045761,  -0.081518, -0.145313, -0.143101,
        -0.118057, -0.092275, -0.086963, -0.113185, -0.110138, -0.049844,
        0.044250,  0.126479,  0.132169,  0.033009,  -0.086397, -0.145368,
        -0.151331, -0.131736, -0.107508, -0.116650, -0.139099};
    static constexpr std::array<double, 23> kCurrentV = {
        -0.560477, -0.687923, -0.677485, -0.469053, -0.123266, 0.250970,
        0.555608,  0.712770,  0.653159,  0.449666,  0.213734,  -0.026554,
        -0.259014, -0.443620, -0.552010, -0.520051, -0.298285, 0.033814,
        0.363170,  0.595197,  0.635996,  0.488669,  0.274862};
    for (size_t i = 0; i < count; ++i) {
      const double hour = std::clamp(
          static_cast<double>(requests[i].unix_time - kEpoch) / 3600.0, 0.0,
          static_cast<double>(kWindU.size() - 1));
      const size_t lower = static_cast<size_t>(std::floor(hour));
      const size_t upper = std::min(lower + 1, kWindU.size() - 1);
      const double fraction = hour - static_cast<double>(lower);
      const auto interpolate = [&](const auto& values) {
        return (values[lower] + fraction * (values[upper] - values[lower])) *
               kKnotsPerMetreSecond;
      };
      results[i] = {interpolate(kWindU),
                    interpolate(kWindV),
                    interpolate(kCurrentU),
                    interpolate(kCurrentV),
                    0.6,
                    7};
    }
    return 0;
  }
  const int64_t unavailable_after =
      static_cast<HostState*>(data)->environment_unavailable_after.load();
  const double unavailable_north_of =
      static_cast<HostState*>(data)->environment_unavailable_north_of.load();
  for (size_t i = 0; i < count; ++i)
    results[i] =
        requests[i].unix_time >= unavailable_after ||
                requests[i].latitude >= unavailable_north_of
            ? ocpn_portable_environment_sample{0, 0, 0, 0, 0, 0}
            : ocpn_portable_environment_sample{8.0, 2.0, 0.4, 0.1, 1.2, 7};
  return 0;
}

void RoutingProgress(void* data, uint8_t, const char* message, size_t length) {
  auto& state = *static_cast<HostState*>(data);
  ++state.routing_progress_events;
  const auto text = Text(message, length);
  if (text.find("Corridor refinement") != std::string::npos)
    state.saw_corridor_refinement = true;
  if (text.find("Departure +0:00") != std::string::npos &&
      text.find("passage leg 2 of 2") != std::string::npos)
    state.saw_passage_progress = true;
  if (text.find("Reverse-isocrone recovery") != std::string::npos)
    state.routing_stage = 1;
  else if (text.find("Time-dependent graph fallback") != std::string::npos) {
    state.routing_stage = 2;
    state.saw_graph_fallback = true;
  }
}
uint8_t RoutingCancelled(void* data) {
  return static_cast<HostState*>(data)->routing_cancelled.load() ? 1 : 0;
}

int32_t ChartsQuerySegments(void* data, const ocpn_portable_geo_segment*,
                            size_t segment_count,
                            ocpn_portable_chart_segment_result* results,
                            size_t result_count) {
  if (segment_count != result_count) return -1;
  auto& state = *static_cast<HostState*>(data);
  state.chart_segments_queried = segment_count;
  const size_t call = ++state.chart_query_calls;
  const bool reject =
      state.reject_chart_query_call.load() == call ||
      (state.reject_reverse_charts.load() && state.routing_stage.load() == 1);
  for (size_t i = 0; i < result_count; ++i)
    results[i] = reject ? ocpn_portable_chart_segment_result{1, 3, 1}
                        : ocpn_portable_chart_segment_result{0, 3, 0};
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

int32_t OpenSurface(void* data, const char* surface_id, size_t surface_id_len) {
  auto& state = *static_cast<HostState*>(data);
  const std::string id = Text(surface_id, surface_id_len);
  state.environmental_viewer_opened = id == "environment.viewer";
  state.weather_routing_opened = id == "routing.workbench";
  return 0;
}

int32_t UserFileRead(void*, const char*, size_t, uint8_t*, size_t,
                     size_t* value_len) {
  *value_len = 0;
  return 0;
}

int32_t UserFileWrite(void*, const char*, size_t, const uint8_t*, size_t) {
  return 0;
}

int32_t SendPluginMessage(void*, const char*, size_t, const char*, size_t) {
  return 0;
}

int32_t ChartsQueryFinalSafety(
    void* data, const ocpn_portable_geo_segment* segments, size_t segment_count,
    const ocpn_portable_final_chart_safety_options* options,
    ocpn_portable_chart_segment_result* results, size_t result_count) {
  if (!options || !std::isfinite(options->safety_margin_nautical_miles) ||
      !std::isfinite(options->minimum_depth_metres)) {
    return -1;
  }
  if (!segments || segment_count != result_count) return -1;
  auto& state = *static_cast<HostState*>(data);
  state.chart_segments_queried = segment_count;
  const size_t call = ++state.chart_query_calls;
  const bool reject = state.reject_chart_query_call.load() == call;
  for (size_t i = 0; i < result_count; ++i)
    results[i] = reject ? ocpn_portable_chart_segment_result{1, 3, 1}
                        : ocpn_portable_chart_segment_result{0, 3, 0};
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
          UserFileWrite,
          SendPluginMessage,
          ChartsQueryFinalSafety};
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
  auto* runtime = ocpn_portable_runtime_create(
      component_path, &callbacks, OCPN_PORTABLE_API_V02,
      OCPN_PORTABLE_WORLD_PLUGIN, error, sizeof(error));
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

  ok = ok && state.actions.size() == 1;
  ok = ok && state.actions[0] == "igrib.toggle";
  ok = ok && state.action_icons.size() == 1;
  ok = ok && state.action_icons[0] == "resources/igrib.svg";
  ok = ok && state.settings["activation-count"] == "1";
  ok = ok && state.scene_id == "igrib.weather-window";
  ok = ok && state.points.size() == 5;
  ok = ok && state.chart_segments_queried == 4;
  ok = ok && std::abs(state.points[0].latitude - 50.35) < 0.000001;
  ok = ok && std::abs(state.points[0].longitude + 4.55) < 0.000001;
  ok = ok && state.job_id == "igrib.prepare-weather";
  ok = ok && state.work_units == 40;
  ok = ok && state.environmental_viewer_opened;

  const std::string surface = "environment.viewer";
  const std::string control = "display-settings";
  const std::string surface_value = "{\"wind\":true}";
  std::array<char, 256> surface_state{};
  size_t surface_state_length = 0;
  ok = ok && CallSucceeded(
                 ocpn_portable_runtime_on_surface_event(
                     runtime, surface.data(), surface.size(), control.data(),
                     control.size(), surface_value.data(), surface_value.size(),
                     surface_state.data(), surface_state.size(),
                     &surface_state_length, error, sizeof(error)),
                 "surface-event", error);
  ok = ok &&
       std::string(surface_state.data(), surface_state_length) ==
           surface_value &&
       state.settings["surface.display-settings"] == surface_value;
  const std::string restore_value = "{\"request\":\"restore\"}";
  surface_state_length = 0;
  ok = ok && CallSucceeded(
                 ocpn_portable_runtime_on_surface_event(
                     runtime, surface.data(), surface.size(), control.data(),
                     control.size(), restore_value.data(), restore_value.size(),
                     surface_state.data(), surface_state.size(),
                     &surface_state_length, error, sizeof(error)),
                 "surface-state-restore", error);
  ok = ok &&
       std::string(surface_state.data(), surface_state_length) == surface_value;

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
  auto* runtime = ocpn_portable_runtime_create(
      component_path, &callbacks, OCPN_PORTABLE_API_V01,
      OCPN_PORTABLE_WORLD_PLUGIN, error, sizeof(error));
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
  auto* runtime = ocpn_portable_runtime_create(
      component_path, &callbacks, OCPN_PORTABLE_API_V02,
      OCPN_PORTABLE_WORLD_PLUGIN, error, sizeof(error));
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
  auto* runtime = ocpn_portable_runtime_create(
      component_path, &callbacks, OCPN_PORTABLE_API_V02,
      OCPN_PORTABLE_WORLD_PASSAGE_ROUTING, error, sizeof(error));
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
  request.refined_heading_step_degrees = 5;
  request.adaptive_headings = 1;
  request.spatial_cell_nautical_miles = 3.0;
  request.labels_per_cell = 2;
  request.max_hours = 24;
  request.max_states = 10000;
  request.avoid_unsafe_charts = 1;
  request.min_true_wind_angle_degrees = 40.0;
  request.max_true_wind_angle_degrees = 160.0;
  request.max_wind_knots = 35.0;
  request.max_apparent_wind_knots = 50.0;
  request.max_wave_metres = 4.0;
  request.max_opposing_wind_current_knots_squared = 30.0;
  request.land_safety_margin_nautical_miles = 0.4;
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
  request.limits_available = 15;
  std::vector<ocpn_portable_route_point> points(1000);
  std::vector<ocpn_portable_route_point> isochrone_points(20000);
  std::vector<ocpn_portable_route_line> isochrone_lines(2000);
  std::vector<ocpn_portable_route_point> trace_points(20000);
  std::vector<ocpn_portable_route_line> trace_lines(2000);
  std::vector<ocpn_portable_route_environment_point> route_environment(1000);
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
  result.route_environment = route_environment.data();
  result.route_environment_capacity = route_environment.size();
  result.diagnostic = diagnostic;
  result.diagnostic_capacity = sizeof(diagnostic);
  ok =
      ok && CallSucceeded(ocpn_portable_runtime_calculate_route(
                              runtime, &request, &result, error, sizeof(error)),
                          "calculate route", error);
  ok = ok && result.point_count >= 2 && result.states_examined > 0 &&
       result.isochrone_count == 0 && result.trace_count == 0 &&
       result.route_environment_count == result.point_count &&
       result.duration_seconds > 0 && result.diagnostic_len > 0 &&
       state.routing_progress_events > 0 && result.average_speed_knots > 0.0 &&
       state.saw_corridor_refinement.load() && result.average_sog_knots > 0.0 &&
       result.maximum_sog_knots > 0.0 && result.average_wind_knots > 8.0 &&
       result.maximum_wind_knots > 8.0 && (result.metrics_available & 1) != 0 &&
       result.average_current_knots > 0.0 && result.comfort_level >= 1 &&
       result.comfort_level <= 3;
  for (size_t index = 0; ok && index < result.route_environment_count;
       ++index) {
    ok = std::isfinite(route_environment[index].wind_u_knots) &&
         std::isfinite(route_environment[index].wind_v_knots) &&
         route_environment[index].unix_time == points[index].unix_time;
  }
  if (!ok) {
    std::cerr << "initial portable route assertions failed\n";
    ocpn_portable_runtime_destroy(runtime);
    return false;
  }
  const std::array<ocpn_portable_passage_gate, 3> gates = {
      ocpn_portable_passage_gate{"start", 5, "Start", 5, 50.0, -4.0},
      ocpn_portable_passage_gate{"gate", 4, "Intermediate gate", 17, 50.025,
                                 -3.975},
      ocpn_portable_passage_gate{"finish", 6, "Finish", 6, 50.05, -3.95}};
  ocpn_portable_passage_request passage_request{};
  passage_request.route = request;
  passage_request.gates = gates.data();
  passage_request.gate_count = gates.size();
  std::array<ocpn_portable_passage_leg, 2> passage_legs{};
  std::array<char, 16384> passage_diagnostic{};
  ocpn_portable_passage_result passage_result{};
  passage_result.route = result;
  passage_result.route.point_count = 0;
  passage_result.route.isochrone_point_count = 0;
  passage_result.route.isochrone_count = 0;
  passage_result.route.trace_point_count = 0;
  passage_result.route.trace_count = 0;
  passage_result.route.route_environment_count = 0;
  passage_result.route.diagnostic = passage_diagnostic.data();
  passage_result.route.diagnostic_capacity = passage_diagnostic.size();
  passage_result.route.diagnostic_len = 0;
  passage_result.legs = passage_legs.data();
  passage_result.leg_capacity = passage_legs.size();
  state.chart_query_calls = 0;
  ok = ok && CallSucceeded(ocpn_portable_runtime_calculate_passage(
                               runtime, &passage_request, &passage_result,
                               error, sizeof(error)),
                           "calculate continuous passage", error);
  ok = ok && passage_result.leg_count == 2 &&
       passage_result.route.point_count >= 3 &&
       passage_result.route.route_environment_count ==
           passage_result.route.point_count &&
       passage_result.validation_samples > 0 &&
       state.saw_passage_progress.load() &&
       passage_legs[0].start_gate_index == 0 &&
       passage_legs[0].end_gate_index == 1 &&
       passage_legs[1].start_gate_index == 1 &&
       passage_legs[1].end_gate_index == 2 &&
       passage_legs[0].point_offset == 0 && passage_legs[0].point_count >= 2 &&
       passage_legs[1].point_offset + 1 == passage_legs[0].point_count &&
       passage_legs[1].point_count >= 2 &&
       passage_legs[1].departure_unix_time ==
           passage_legs[0].arrival_unix_time &&
       passage_legs[1].point_offset + passage_legs[1].point_count ==
           passage_result.route.point_count &&
       std::strstr(passage_diagnostic.data(), "complete 2-leg passage") !=
           nullptr;
  if (!ok) {
    std::cerr << "continuous passage assertions failed: " << error
              << " legs=" << passage_result.leg_count
              << " points=" << passage_result.route.point_count
              << " environment=" << passage_result.route.route_environment_count
              << " validation=" << passage_result.validation_samples
              << " first-span=" << passage_legs[0].point_offset << "+"
              << passage_legs[0].point_count
              << " second-span=" << passage_legs[1].point_offset << "+"
              << passage_legs[1].point_count
              << " seam-times=" << passage_legs[0].arrival_unix_time << "/"
              << passage_legs[1].departure_unix_time
              << " diagnostic=" << passage_diagnostic.data() << '\n';
    ocpn_portable_runtime_destroy(runtime);
    return false;
  }
  const size_t baseline_passage_chart_calls = state.chart_query_calls.load();
  const std::vector<ocpn_portable_route_point> serial_passage_points(
      points.begin(), points.begin() + passage_result.route.point_count);
  state.environment_unavailable_north_of = 50.03;
  passage_result.route.point_count = 0;
  passage_result.route.diagnostic_len = 0;
  passage_result.leg_count = 0;
  std::memset(error, 0, sizeof(error));
  const int32_t missing_later_weather = ocpn_portable_runtime_calculate_passage(
      runtime, &passage_request, &passage_result, error, sizeof(error));
  ok = ok && missing_later_weather != 0 &&
       std::strstr(error, "leg 2 of 2") != nullptr;
  state.environment_unavailable_north_of = 91.0;
  if (!ok) {
    std::cerr << "later-gate weather coverage was not rejected: " << error
              << '\n';
    ocpn_portable_runtime_destroy(runtime);
    return false;
  }
  state.chart_query_calls = 0;
  state.reject_chart_query_call = baseline_passage_chart_calls;
  passage_result.route.point_count = 0;
  passage_result.route.diagnostic_len = 0;
  passage_result.leg_count = 0;
  std::memset(error, 0, sizeof(error));
  const int32_t unsafe_whole_passage = ocpn_portable_runtime_calculate_passage(
      runtime, &passage_request, &passage_result, error, sizeof(error));
  ok = ok && baseline_passage_chart_calls > 0 && unsafe_whole_passage != 0 &&
       std::strstr(error, "whole-passage independent validation failed") !=
           nullptr;
  state.reject_chart_query_call = 0;
  if (!ok) {
    std::cerr << "unsafe whole passage was not rejected independently: "
              << error << '\n';
    ocpn_portable_runtime_destroy(runtime);
    return false;
  }

  // Search decisions are provisional.  Re-run the deterministic request and
  // make only the final chart-service call unsafe: the component must reject
  // the exact delivered geometry at its independent acceptance boundary.
  // Use the already-fine resolution here so the deliberately rejected route
  // cannot correctly fall back to a separately validated coarse incumbent.
  auto validation_request = request;
  validation_request.time_step_seconds = 1800;
  validation_request.heading_step_degrees = 5;
  validation_request.refined_heading_step_degrees = 5;
  validation_request.spatial_cell_nautical_miles = 1.5;
  state.chart_query_calls = 0;
  result.point_count = 0;
  result.diagnostic_len = 0;
  std::memset(error, 0, sizeof(error));
  ok = ok && CallSucceeded(ocpn_portable_runtime_calculate_route(
                               runtime, &validation_request, &result, error,
                               sizeof(error)),
                           "calculate fine route for replay rejection", error);
  const size_t baseline_chart_calls = state.chart_query_calls.load();
  state.chart_query_calls = 0;
  state.reject_chart_query_call = baseline_chart_calls;
  result.point_count = 0;
  result.diagnostic_len = 0;
  std::memset(error, 0, sizeof(error));
  const int32_t independently_rejected = ocpn_portable_runtime_calculate_route(
      runtime, &validation_request, &result, error, sizeof(error));
  ok = ok && baseline_chart_calls > 0 && independently_rejected != 0 &&
       (std::strstr(error, "independent validation rejected") != nullptr ||
        std::strstr(error, "independent final safety validation rejected") !=
            nullptr ||
        std::strstr(error,
                    "independent final chart-safety validation rejected") !=
            nullptr);
  if (!ok) {
    std::cerr << "independent replay rejection assertion failed: " << error
              << '\n';
    ocpn_portable_runtime_destroy(runtime);
    return false;
  }
  state.reject_chart_query_call = 0;
  state.chart_query_calls = 0;

  // A route requiring several forecast steps must expose bounded retained
  // isochrones and predecessor traces for host-side inspection rendering.
  auto inspection_request = request;
  inspection_request.destination_latitude = 50.2;
  inspection_request.destination_longitude = -3.8;
  inspection_request.inspection_interval_seconds = 3600;
  inspection_request.include_traces = 1;
  // This phase verifies API 0.2 inspection scheduling and result marshalling;
  // synthetic chart rejection is covered independently above.
  inspection_request.avoid_unsafe_charts = 0;
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
  if (!ok) {
    std::cerr << "inspection geometry assertions failed: " << error << '\n';
    ocpn_portable_runtime_destroy(runtime);
    return false;
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

  // Regression for the real Holyhead offshore -> Dun Laoghaire offshore
  // failure reported by the beta profile.  This upwind Irish Sea passage is
  // long enough to require several retained isochrones.  Raw heading
  // candidates must not consume the complete 80,000-state budget before the
  // boat has had time to reach the destination.
  auto irish_sea_request = request;
  irish_sea_request.start_latitude = 53.336985;
  irish_sea_request.start_longitude = -4.607470;
  irish_sea_request.destination_latitude = 53.308408;
  irish_sea_request.destination_longitude = -6.119702;
  irish_sea_request.max_hours = 120;
  irish_sea_request.max_states = 80000;
  result.point_count = 0;
  result.isochrone_point_count = 0;
  result.isochrone_count = 0;
  result.trace_point_count = 0;
  result.trace_count = 0;
  result.route_environment_count = 0;
  result.diagnostic_len = 0;
  std::memset(error, 0, sizeof(error));
  ok = ok && CallSucceeded(ocpn_portable_runtime_calculate_route(
                               runtime, &irish_sea_request, &result, error,
                               sizeof(error)),
                           "calculate Holyhead to Dun Laoghaire route", error);
  ok = ok && result.point_count >= 3 && result.duration_seconds < 72 * 3600 &&
       result.tacks > 0 &&
       result.states_examined < irish_sea_request.max_states &&
       std::hypot((points[result.point_count - 1].latitude -
                   irish_sea_request.destination_latitude) *
                      60.0,
                  (points[result.point_count - 1].longitude -
                   irish_sea_request.destination_longitude) *
                      60.0 *
                      std::cos(irish_sea_request.destination_latitude *
                               3.14159265358979323846 / 180.0)) <=
           irish_sea_request.destination_tolerance_nm;
  if (!ok) {
    std::cerr << "Irish Sea regression assertions failed: " << error << '\n';
    ocpn_portable_runtime_destroy(runtime);
    return false;
  }

  // Re-run the reported departure against the decoded hourly wind/current
  // sequence from the user's GRIB. This specifically guards the former leg-9
  // TWA 165.5-degree validator rejection.
  auto reported_weather_request = irish_sea_request;
  reported_weather_request.departure_unix_time = 1784793600;
  state.use_reported_irish_sea_weather = true;
  result.point_count = 0;
  result.diagnostic_len = 0;
  std::memset(error, 0, sizeof(error));
  ok = ok && CallSucceeded(ocpn_portable_runtime_calculate_route(
                               runtime, &reported_weather_request, &result,
                               error, sizeof(error)),
                           "calculate reported Irish Sea GRIB route", error);
  ok = ok && result.point_count >= 3 &&
       std::string(diagnostic, result.diagnostic_len)
               .find("independent chronological replay") != std::string::npos;
  state.use_reported_irish_sea_weather = false;
  if (!ok) {
    std::cerr << "reported-weather regression assertions failed: " << error
              << '\n';
    ocpn_portable_runtime_destroy(runtime);
    return false;
  }

  // Constrain the forward-stage share so the same upwind passage exercises
  // automatic destination-side reverse-isocrone recovery. The stage must be
  // visible through progress and recorded in the result diagnostic.
  auto reverse_request = irish_sea_request;
  reverse_request.max_states = 4000;
  state.routing_stage = 0;
  result.point_count = 0;
  result.diagnostic_len = 0;
  std::memset(error, 0, sizeof(error));
  ok = ok && CallSucceeded(
                 ocpn_portable_runtime_calculate_route(
                     runtime, &reverse_request, &result, error, sizeof(error)),
                 "calculate route using reverse-isocrone recovery", error);
  ok = ok && state.routing_stage.load() >= 1 &&
       std::string(diagnostic, result.diagnostic_len)
               .find("reverse-isocrone recovery") != std::string::npos;
  if (!ok) {
    std::cerr << "reverse recovery assertions failed: " << error
              << " stage=" << state.routing_stage.load() << " diagnostic="
              << std::string(diagnostic, result.diagnostic_len) << '\n';
    ocpn_portable_runtime_destroy(runtime);
    return false;
  }

  // Reject chart corridors only while reverse recovery is active. The
  // component must visibly escalate and complete through its bounded
  // time-dependent graph labels rather than reporting a frozen/failed job.
  state.routing_stage = 0;
  state.saw_graph_fallback = false;
  state.reject_reverse_charts = true;
  result.point_count = 0;
  result.diagnostic_len = 0;
  std::memset(error, 0, sizeof(error));
  ok = ok && CallSucceeded(
                 ocpn_portable_runtime_calculate_route(
                     runtime, &reverse_request, &result, error, sizeof(error)),
                 "calculate route using graph fallback", error);
  ok = ok && state.saw_graph_fallback.load() &&
       std::string(diagnostic, result.diagnostic_len)
               .find("time-dependent graph fallback") != std::string::npos;
  state.reject_reverse_charts = false;
  state.routing_stage = 0;
  if (!ok) {
    std::cerr << "graph fallback assertions failed (stage "
              << state.routing_stage.load() << ", diagnostic "
              << std::string(diagnostic, result.diagnostic_len)
              << "): " << error << '\n';
    ocpn_portable_runtime_destroy(runtime);
    return false;
  }

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

  // Propulsion is an explicit policy, disabled by default.  The same route
  // which cannot sail inside this deliberately narrow TWA sector must become
  // feasible when motor-only operation is granted, and must report auditable
  // time/fuel metrics.  A real motor-time budget must then reject it.
  auto motor_request = rejected_request;
  motor_request.allow_motor = 1;
  motor_request.motor_below_sailing_speed_knots = 30.0;
  motor_request.motor_speed_knots = 5.5;
  motor_request.motor_crossover_hysteresis_knots = 0.2;
  motor_request.minimum_motor_run_seconds = 0;
  motor_request.mode_change_penalty_seconds = 120;
  motor_request.maximum_motor_seconds = 24 * 3600;
  motor_request.fuel_consumption_litres_per_hour = 2.5;
  motor_request.maximum_fuel_litres = 100.0;
  motor_request.limits_available |= 16 | 32 | 64;
  result.point_count = 0;
  result.diagnostic_len = 0;
  std::memset(error, 0, sizeof(error));
  ok = ok && CallSucceeded(
                 ocpn_portable_runtime_calculate_route(
                     runtime, &motor_request, &result, error, sizeof(error)),
                 "calculate motor-only route", error);
  ok = ok && result.point_count >= 2 && result.motor_seconds > 0 &&
       result.estimated_fuel_litres > 0.0 &&
       result.propulsion_transitions > 0 && (result.metrics_available & 2) != 0;
  motor_request.maximum_motor_seconds = 1;
  result.point_count = 0;
  result.diagnostic_len = 0;
  std::memset(error, 0, sizeof(error));
  const int32_t motor_budget_rejected = ocpn_portable_runtime_calculate_route(
      runtime, &motor_request, &result, error, sizeof(error));
  ok = ok && motor_budget_rejected != 0 &&
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
      std::vector<ocpn_portable_route_environment_point>
          local_route_environment(1000);
      char local_diagnostic[16384] = {};
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
      local_result.route_environment = local_route_environment.data();
      local_result.route_environment_capacity = local_route_environment.size();
      local_result.diagnostic = local_diagnostic;
      local_result.diagnostic_capacity = sizeof(local_diagnostic);
      std::array<ocpn_portable_passage_leg, 2> local_legs{};
      ocpn_portable_passage_result local_passage{};
      local_passage.route = local_result;
      local_passage.legs = local_legs.data();
      local_passage.leg_capacity = local_legs.size();
      replica_results[index] =
          ocpn_portable_runtime_calculate_passage(
              replicas[index], &passage_request, &local_passage, local_error,
              sizeof(local_error)) == 0 &&
          local_passage.leg_count == 2 &&
          local_passage.route.point_count == serial_passage_points.size();
      for (size_t point_index = 0;
           replica_results[index] && point_index < serial_passage_points.size();
           ++point_index) {
        replica_results[index] =
            local_points[point_index].latitude ==
                serial_passage_points[point_index].latitude &&
            local_points[point_index].longitude ==
                serial_passage_points[point_index].longitude &&
            local_points[point_index].unix_time ==
                serial_passage_points[point_index].unix_time;
      }
    });
  }
  for (auto& thread : threads) thread.join();
  for (auto* compute_runtime : replicas)
    ocpn_portable_runtime_destroy(compute_runtime);
  ok = ok &&
       std::all_of(replica_results.begin(), replica_results.end(),
                   [](bool result) { return result; }) &&
       state.actions.size() == 1;
  // Provider diagnostics must cross the C bridge instead of being reduced to
  // an opaque numeric host-service error.
  state.environment_failure = true;
  std::memset(error, 0, sizeof(error));
  const int32_t provider_failure = ocpn_portable_runtime_calculate_route(
      runtime, &request, &result, error, sizeof(error));
  ok = ok && provider_failure != 0 &&
       std::strstr(error, "decoder deliberately unavailable") != nullptr;
  state.environment_failure = false;
  state.routing_cancelled = true;
  std::memset(error, 0, sizeof(error));
  passage_result.route.point_count = 0;
  passage_result.route.diagnostic_len = 0;
  passage_result.leg_count = 0;
  const int32_t cancelled = ocpn_portable_runtime_calculate_passage(
      runtime, &passage_request, &passage_result, error, sizeof(error));
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
  if (argc != 4) {
    std::cerr << "usage: portable_runtime_smoke_test <igrib.wasm> "
                 "<iweather-routing.wasm> <legacy-v01.wasm>\n";
    return 2;
  }
  if (!NormalLifecycle(argv[1]) || !TrapIsContained(argv[3]) ||
      !IdentityMismatchIsRejected(argv[1]) || !RoutingLifecycle(argv[2]))
    return 1;
  std::cout << "portable runtime smoke test passed\n";
  return 0;
}
