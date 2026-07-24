#include "chart_safety_service.h"
#include "portable_polar.h"
#include "runtime_engine.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include <wx/init.h>

extern "C" bool PlugIn_GSHHS_CrossesLand(double, double, double, double) {
  // This probe validates the package/runtime/environment/routing chain. Chart
  // coverage is an OpenCPN UI-thread service and is tested in the live host.
  return false;
}

namespace {

bool Number(const char* value, double* result) {
  char* end = nullptr;
  *result = std::strtod(value, &end);
  return end && *end == '\0' && std::isfinite(*result);
}

bool UnixTime(const char* value, std::int64_t* result) {
  char* end = nullptr;
  *result = std::strtoll(value, &end, 10);
  return end && *end == '\0';
}

bool Enable(ppm::RuntimeEngine* engine, const std::string& package_id,
            std::string* diagnostic) {
  return engine->SetGrantedPermissions(package_id,
                                       engine->RequestedPermissions(package_id),
                                       diagnostic) &&
         engine->Enable(package_id, diagnostic);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 9) {
    std::cerr
        << "usage: ppm_routing_probe STORE_ROOT GRIB POLAR DEPARTURE_UNIX "
           "START_LAT START_LON DEST_LAT DEST_LON\n";
    return 2;
  }
  std::int64_t departure = 0;
  double start_latitude = 0.0;
  double start_longitude = 0.0;
  double destination_latitude = 0.0;
  double destination_longitude = 0.0;
  if (!UnixTime(argv[4], &departure) || !Number(argv[5], &start_latitude) ||
      !Number(argv[6], &start_longitude) ||
      !Number(argv[7], &destination_latitude) ||
      !Number(argv[8], &destination_longitude)) {
    std::cerr << "departure and route coordinates are invalid\n";
    return 2;
  }

  wxInitializer wx;
  if (!wx.IsOk()) {
    std::cerr << "wxWidgets could not be initialised\n";
    return 1;
  }
  if (const char* cm93_root = std::getenv("PPM_TEST_CM93_ROOT")) {
    ppm::ChartSafetyService::ConfigureChartRoots({cm93_root});
    if (!ppm::ChartSafetyService().AuthoritativeAvailable()) {
      std::cerr << "configured CM93 semantic chart root is unavailable\n";
      return 1;
    }
    std::cout << ppm::ChartSafetyService().Summary() << '\n';
  }
  std::uint32_t next_action = 1;
  ppm::RuntimeEngine engine(
      argv[1],
      [&](const ppm::RuntimeAction&, std::uint32_t* host_action_id) {
        *host_action_id = next_action++;
        return 0;
      },
      [](const std::string&) {}, []() {});
  std::string diagnostic;
  if (!engine.LoadInstalled(true)) {
    std::cerr << "one or more packages failed static loading\n";
    return 1;
  }
  if (!Enable(&engine, "org.opencpn.igrib", &diagnostic)) {
    std::cerr << "iGRIB enable failed: " << diagnostic << '\n';
    return 1;
  }
  if (!engine.SelectEnvironmentDataset("org.opencpn.igrib", {argv[2]}) ||
      !engine.WaitForIdle("org.opencpn.igrib", std::chrono::minutes(2))) {
    std::cerr << "iGRIB did not finish loading the selected dataset\n";
    return 1;
  }
  const std::string environment_summary =
      engine.EnvironmentSummary("org.opencpn.igrib");
  if (environment_summary.find("forecast times") == std::string::npos) {
    std::cerr << "iGRIB did not publish a usable forecast snapshot: "
              << environment_summary << '\n';
    return 1;
  }
  if (!Enable(&engine, "org.opencpn.iweather-routing", &diagnostic)) {
    std::cerr << "iWeatherRouting enable failed: " << diagnostic << '\n';
    return 1;
  }

  PortablePolarSet loaded;
  if (!LoadPortablePolarSet(argv[3], &loaded, &diagnostic)) {
    std::cerr << "polar load failed: " << diagnostic << '\n';
    return 1;
  }
  ppm::RoutingRequest request;
  request.parameters.start_latitude = start_latitude;
  request.parameters.start_longitude = start_longitude;
  request.parameters.destination_latitude = destination_latitude;
  request.parameters.destination_longitude = destination_longitude;
  request.parameters.departure_unix_time = departure;
  // Exercise the same staged policy as the GUI: broad coarse discovery,
  // reverse/graph recovery when needed, then automatic fine-corridor
  // refinement inside the portable routing component.
  request.parameters.time_step_seconds = 3600;
  request.parameters.heading_step_degrees = 15;
  request.parameters.refined_heading_step_degrees = 5;
  request.parameters.adaptive_headings = 1;
  request.parameters.spatial_cell_nautical_miles = 3.0;
  request.parameters.labels_per_cell = 2;
  request.parameters.max_hours = 120;
  request.parameters.max_states = 20'000;
  request.parameters.min_true_wind_angle_degrees = 40.0;
  request.parameters.max_true_wind_angle_degrees = 180.0;
  request.parameters.maximum_latitude_degrees = 89.0;
  request.parameters.upwind_efficiency = 1.0;
  request.parameters.downwind_efficiency = 1.0;
  request.parameters.maximum_search_angle_degrees = 120.0;
  request.parameters.destination_tolerance_nm = 1.0;
  request.parameters.avoid_unsafe_charts = 1;
  request.parameters.land_safety_margin_nautical_miles = 0.4;
  request.parameters.minimum_chart_depth_metres = 2.0;
  request.parameters.require_authoritative_chart_safety = 1;
  request.parameters.tack_penalty_seconds = 300;
  request.parameters.gybe_penalty_seconds = 300;
  request.parameters.use_currents = 1;
  request.parameters.require_current_data = 1;
  request.parameters.use_waves = 1;
  request.parameters.require_wave_data = 1;
  for (auto& polar : loaded.grids) {
    request.polars.push_back({std::move(polar.identity),
                              std::move(polar.true_wind_speeds_knots),
                              std::move(polar.true_wind_angles_degrees),
                              std::move(polar.boat_speeds_knots)});
  }

  engine.SetRoutingProgressCallback(
      [](const std::string&, std::uint8_t percent, const std::string& message) {
        std::cout << "progress=" << static_cast<unsigned>(percent) << '\t'
                  << message << '\n';
      });
  ppm::RoutingOutcome outcome;
  if (!engine.CalculateRouteBlocking("org.opencpn.iweather-routing",
                                     std::move(request), &outcome,
                                     &diagnostic)) {
    std::cerr << "route calculation failed: " << diagnostic << '\n';
    return 1;
  }
  if (outcome.points.size() < 2 ||
      outcome.route_environment.size() != outcome.points.size()) {
    std::cerr << "route result is incomplete\n";
    return 1;
  }
  std::cout << "route=complete"
            << " points=" << outcome.points.size()
            << " duration_seconds=" << outcome.duration_seconds
            << " distance_nm=" << outcome.distance_nautical_miles
            << " states=" << outcome.states_examined
            << " tacks=" << outcome.tacks
            << " diagnostic=" << outcome.diagnostic << '\n';
  engine.Shutdown();
  return 0;
}
