#ifndef PORTABLE_PLUGIN_MANAGER_ROUTING_SERVICE_H
#define PORTABLE_PLUGIN_MANAGER_ROUTING_SERVICE_H

#include <cstdint>
#include <string>
#include <vector>

#include "ocpn_portable_runtime.h"

namespace ppm {

struct RoutingPolar {
  std::string identity;
  std::vector<double> true_wind_speeds_knots;
  std::vector<double> true_wind_angles_degrees;
  std::vector<double> boat_speeds_knots;
};

struct RoutingRequest {
  ocpn_portable_route_request parameters{};
  std::vector<RoutingPolar> polars;
};

struct RoutingInspectionLine {
  std::int64_t unix_time = 0;
  std::vector<ocpn_portable_route_point> points;
};

struct RoutingOutcome {
  std::vector<ocpn_portable_route_point> points;
  std::vector<ocpn_portable_route_environment_point> route_environment;
  std::vector<RoutingInspectionLine> isochrones;
  std::vector<RoutingInspectionLine> traces;
  std::string diagnostic;
  double distance_nautical_miles = 0.0;
  std::uint64_t duration_seconds = 0;
  std::uint32_t states_examined = 0;
  double average_speed_knots = 0.0;
  double maximum_speed_knots = 0.0;
  double average_sog_knots = 0.0;
  double maximum_sog_knots = 0.0;
  double average_wind_knots = 0.0;
  double maximum_wind_knots = 0.0;
  double average_current_knots = 0.0;
  double maximum_current_knots = 0.0;
  std::uint32_t tacks = 0;
  std::uint64_t motor_seconds = 0;
  double estimated_fuel_litres = 0.0;
  std::uint32_t propulsion_transitions = 0;
  std::uint8_t comfort_level = 0;
  std::uint8_t metrics_available = 0;
};

}  // namespace ppm

#endif
