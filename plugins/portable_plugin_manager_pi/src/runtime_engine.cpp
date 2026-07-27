#include "runtime_engine.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <openssl/rand.h>

#include <wx/dir.h>
#include <wx/base64.h>
#include <wx/filename.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/jsonwriter.h>
#include <wx/log.h>
#include <wx/regex.h>
#include <wx/sstream.h>
#include <wx/thread.h>
#include <wx/wfstream.h>

#include "ocpn_plugin.h"
#include "ocpn_portable_runtime.h"
#include "capability_event_broker.h"
#include "chart_safety_service.h"
#include "declarative_ui.h"
#include "environment_provider.h"
#include "job_scheduler.h"
#include "https_service.h"
#include "permission_store.h"
#include "service_version.h"
#include "serial_executor.h"

namespace ppm {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kErrorCapacity = 4096;
constexpr std::size_t kPassageDiagnosticCapacity = 256 * 1024;
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
constexpr std::size_t kAuthorRequestLimit = 16 * 1024 * 1024;
constexpr std::size_t kAuthorResponseLimit = 16 * 1024 * 1024;
constexpr std::size_t kSceneLayerLimit = 128;
constexpr std::size_t kScenePrimitiveLimit = 4096;
constexpr std::size_t kScenePointLimit = 250'000;
constexpr std::size_t kTimerLimitPerPackage = 32;
constexpr std::uint32_t kMinimumTimerMilliseconds = 50;
constexpr std::uint32_t kMaximumTimerMilliseconds = 24U * 60U * 60U * 1000U;
constexpr std::size_t kRpcServiceLimitPerPackage = 64;
constexpr std::size_t kRpcOutstandingLimitPerPackage = 128;
constexpr std::size_t kRpcPayloadLimit = 1024 * 1024;
constexpr std::uint32_t kRpcMinimumTimeoutMilliseconds = 100;
constexpr std::uint32_t kRpcMaximumTimeoutMilliseconds = 60'000;

std::string Text(const char* value, std::size_t length) {
  return value && length ? std::string(value, length) : std::string();
}

void CopyError(const std::string& value, char* output, std::size_t capacity) {
  if (!output || capacity == 0) return;
  const std::size_t copied = std::min(value.size(), capacity - 1);
  if (copied != 0) std::memcpy(output, value.data(), copied);
  output[copied] = '\0';
}

std::string JsonText(const wxJSONValue& value) {
  wxString encoded;
  wxJSONWriter writer;
  writer.Write(value, encoded);
  return encoded.ToStdString();
}

wxJSONValue AuthorError(const std::string& code, const std::string& message,
                        bool retryable = false) {
  wxJSONValue response;
  response["error"]["code"] = code;
  response["error"]["message"] = message;
  response["error"]["retryable"] = retryable;
  return response;
}

bool ParseAuthorRequest(const std::string& encoded, wxJSONValue* value) {
  if (!value || encoded.size() > kAuthorRequestLimit) return false;
  wxJSONReader reader;
  return reader.Parse(wxString::FromUTF8(encoded), value) == 0 &&
         value->IsObject();
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

bool ParseOverlayPoint(wxJSONValue value, OverlayPoint* point) {
  if (!point || !value.IsObject() || !value["latitude"].IsDouble() ||
      !value["longitude"].IsDouble()) {
    return false;
  }
  point->latitude = value["latitude"].AsDouble();
  point->longitude = value["longitude"].AsDouble();
  return std::isfinite(point->latitude) && std::isfinite(point->longitude) &&
         std::abs(point->latitude) <= 90.0 &&
         std::abs(point->longitude) <= 180.0;
}

bool ParseOverlayColor(wxJSONValue value, OverlayColor* color) {
  if (!color || !value.IsObject()) return false;
  const long red = value["red"].AsLong();
  const long green = value["green"].AsLong();
  const long blue = value["blue"].AsLong();
  const long alpha = value["alpha"].AsLong();
  if (red < 0 || red > 255 || green < 0 || green > 255 || blue < 0 ||
      blue > 255 || alpha < 0 || alpha > 255) {
    return false;
  }
  *color = {static_cast<unsigned char>(red), static_cast<unsigned char>(green),
            static_cast<unsigned char>(blue),
            static_cast<unsigned char>(alpha)};
  return true;
}

bool ParseOverlayStyle(wxJSONValue value, OverlayStyle* style) {
  if (!style || !value.IsObject()) return false;
  style->has_stroke = !value["stroke"].IsNull();
  style->has_fill = !value["fill"].IsNull();
  if (style->has_stroke &&
      !ParseOverlayColor(value["stroke"], &style->stroke)) {
    return false;
  }
  if (style->has_fill && !ParseOverlayColor(value["fill"], &style->fill))
    return false;
  style->width_pixels = static_cast<float>(value["width_pixels"].AsDouble());
  style->dash_pattern.clear();
  if (value.HasMember("dash_pattern")) {
    if (!value["dash_pattern"].IsArray() || value["dash_pattern"].Size() > 16)
      return false;
    for (int index = 0; index < value["dash_pattern"].Size(); ++index) {
      const float length =
          static_cast<float>(value["dash_pattern"][index].AsDouble());
      if (!std::isfinite(length) || length < 0.5F || length > 512.0F)
        return false;
      style->dash_pattern.push_back(length);
    }
    if (style->dash_pattern.size() % 2 != 0) return false;
  }
  return std::isfinite(style->width_pixels) && style->width_pixels >= 0.5F &&
         style->width_pixels <= 64.0F;
}

std::int64_t UnixMillisecondsNow() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool ValidateRoutingRequest(const RoutingRequest& request,
                            std::string* diagnostic) {
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
  std::size_t total_cells = 0;
  for (const auto& polar : request.polars) {
    const std::size_t wind_count = polar.true_wind_speeds_knots.size();
    const std::size_t angle_count = polar.true_wind_angles_degrees.size();
    if (polar.identity.empty() || polar.identity.size() > 1024 ||
        wind_count < 2 || angle_count < 2 ||
        wind_count > kRoutePolarAxisLimit ||
        angle_count > kRoutePolarAxisLimit ||
        wind_count > kRoutePolarCellLimit / angle_count ||
        polar.boat_speeds_knots.size() != wind_count * angle_count ||
        total_cells > kRoutePolarCellLimit - wind_count * angle_count ||
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
        !std::all_of(
            polar.boat_speeds_knots.begin(), polar.boat_speeds_knots.end(),
            [](double value) {
              return std::isfinite(value) && value >= 0.0 && value <= 250.0;
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
    total_cells += wind_count * angle_count;
  }
  return true;
}

bool ValidatePassageRequest(const RoutingPassageRequest& request,
                            std::string* diagnostic) {
  if (request.gates.size() < 2 || request.gates.size() > 64) {
    if (diagnostic) *diagnostic = "passage routing requires 2-64 gates";
    return false;
  }
  for (std::size_t index = 0; index < request.gates.size(); ++index) {
    const auto& gate = request.gates[index];
    if (gate.id.empty() || gate.id.size() > 1024 || gate.name.empty() ||
        gate.name.size() > 1024 || !std::isfinite(gate.latitude) ||
        !std::isfinite(gate.longitude) || std::abs(gate.latitude) > 90.0 ||
        std::abs(gate.longitude) > 180.0 ||
        (index > 0 &&
         std::abs(gate.latitude - request.gates[index - 1].latitude) < 1e-9 &&
         std::abs(gate.longitude - request.gates[index - 1].longitude) <
             1e-9)) {
      if (diagnostic)
        *diagnostic = "passage contains an invalid or duplicate gate";
      return false;
    }
  }
  return ValidateRoutingRequest(request.route, diagnostic);
}

bool ValidRoutePoint(const ocpn_portable_route_point& point) {
  return std::isfinite(point.latitude) && std::isfinite(point.longitude) &&
         std::abs(point.latitude) <= 90.0 && std::abs(point.longitude) <= 180.0;
}

struct RoutingExecution {
  bool success = false;
  RoutingOutcome outcome;
  std::string failure;
};

RoutingExecution ExecuteRoute(ocpn_portable_runtime* replica,
                              RoutingRequest request) noexcept {
  RoutingExecution execution;
  std::unique_ptr<ocpn_portable_runtime,
                  decltype(&ocpn_portable_runtime_destroy)>
      worker_runtime(replica, ocpn_portable_runtime_destroy);
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
    std::vector<ocpn_portable_route_line> isochrones(kRouteInspectionLineLimit);
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
    const bool valid_points =
        bounded &&
        std::all_of(points.begin(), points.begin() + result.point_count,
                    ValidRoutePoint) &&
        std::all_of(isochrone_points.begin(),
                    isochrone_points.begin() + result.isochrone_point_count,
                    ValidRoutePoint) &&
        std::all_of(trace_points.begin(),
                    trace_points.begin() + result.trace_point_count,
                    ValidRoutePoint);
    bool chronological = true;
    for (std::size_t index = 1; index < result.point_count; ++index) {
      if (points[index].unix_time <= points[index - 1].unix_time) {
        chronological = false;
        break;
      }
    }
    const bool valid_environment =
        bounded && result.route_environment_count == result.point_count &&
        std::all_of(environment.begin(),
                    environment.begin() + result.route_environment_count,
                    [](const auto& point) {
                      return std::isfinite(point.latitude) &&
                             std::isfinite(point.longitude) &&
                             std::abs(point.latitude) <= 90.0 &&
                             std::abs(point.longitude) <= 180.0 &&
                             std::isfinite(point.wind_u_knots) &&
                             std::isfinite(point.wind_v_knots) &&
                             std::isfinite(point.current_u_knots) &&
                             std::isfinite(point.current_v_knots) &&
                             std::isfinite(point.wave_height_metres) &&
                             (point.available & ~std::uint8_t{3}) == 0;
                    });
    bool environment_matches_route = valid_environment;
    for (std::size_t index = 0;
         environment_matches_route && index < result.point_count; ++index) {
      environment_matches_route =
          environment[index].latitude == points[index].latitude &&
          environment[index].longitude == points[index].longitude &&
          environment[index].unix_time == points[index].unix_time;
    }
    const std::array<double, 10> metrics = {
        result.distance_nautical_miles, result.average_speed_knots,
        result.maximum_speed_knots,     result.average_sog_knots,
        result.maximum_sog_knots,       result.average_wind_knots,
        result.maximum_wind_knots,      result.average_current_knots,
        result.maximum_current_knots,   result.estimated_fuel_litres};
    const bool valid_metrics =
        std::all_of(metrics.begin(), metrics.end(),
                    [](double value) {
                      return std::isfinite(value) && value >= 0.0;
                    }) &&
        result.comfort_level >= 1 && result.comfort_level <= 3 &&
        (result.metrics_available & ~std::uint8_t{3}) == 0;
    const bool valid_success =
        status != 0 || (result.point_count >= 2 && chronological &&
                        environment_matches_route && valid_metrics);
    execution.success =
        status == 0 && bounded && valid_spans && valid_points && valid_success;
    if (!bounded || !valid_spans || !valid_points || !valid_success) {
      execution.failure =
          "routing component returned an invalid or oversized result";
      return execution;
    }

    auto& outcome = execution.outcome;
    outcome.points.assign(points.begin(), points.begin() + result.point_count);
    outcome.route_environment.assign(
        environment.begin(),
        environment.begin() + result.route_environment_count);
    auto copy_lines = [](const auto& source_points, const auto& source_lines,
                         std::size_t line_count, auto* destination) {
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
    outcome.diagnostic.assign(result_diagnostic.data(), result.diagnostic_len);
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
    if (!execution.success) {
      execution.failure =
          error[0] ? error.data() : execution.outcome.diagnostic;
      if (execution.failure.empty())
        execution.failure = "route calculation failed";
    }
  } catch (const std::exception& exception) {
    execution.failure =
        std::string("routing worker failed safely: ") + exception.what();
  } catch (...) {
    execution.failure = "routing worker failed safely with an unknown error";
  }
  return execution;
}

RoutingExecution ExecutePassage(ocpn_portable_runtime* replica,
                                RoutingPassageRequest request) noexcept {
  RoutingExecution execution;
  std::unique_ptr<ocpn_portable_runtime,
                  decltype(&ocpn_portable_runtime_destroy)>
      worker_runtime(replica, ocpn_portable_runtime_destroy);
  try {
    std::vector<ocpn_portable_polar_grid> polar_views;
    polar_views.reserve(request.route.polars.size());
    for (const auto& polar : request.route.polars) {
      polar_views.push_back({polar.identity.data(), polar.identity.size(),
                             polar.true_wind_speeds_knots.data(),
                             polar.true_wind_speeds_knots.size(),
                             polar.true_wind_angles_degrees.data(),
                             polar.true_wind_angles_degrees.size(),
                             polar.boat_speeds_knots.data(),
                             polar.boat_speeds_knots.size()});
    }
    request.route.parameters.polars = polar_views.data();
    request.route.parameters.polar_count = polar_views.size();
    std::vector<ocpn_portable_passage_gate> gate_views;
    gate_views.reserve(request.gates.size());
    for (const auto& gate : request.gates) {
      gate_views.push_back({gate.id.data(), gate.id.size(), gate.name.data(),
                            gate.name.size(), gate.latitude, gate.longitude});
    }

    std::vector<ocpn_portable_route_point> points(kRoutePointLimit);
    std::vector<ocpn_portable_route_environment_point> environment(
        kRoutePointLimit);
    std::vector<ocpn_portable_route_point> isochrone_points(
        kRouteInspectionPointLimit);
    std::vector<ocpn_portable_route_line> isochrones(kRouteInspectionLineLimit);
    std::vector<ocpn_portable_route_point> trace_points(
        kRouteInspectionPointLimit);
    std::vector<ocpn_portable_route_line> traces(kRouteInspectionLineLimit);
    std::vector<ocpn_portable_passage_leg> legs(request.gates.size() - 1);
    std::array<char, kErrorCapacity> error{};
    std::vector<char> result_diagnostic(kPassageDiagnosticCapacity);
    ocpn_portable_passage_request passage_request{};
    passage_request.route = request.route.parameters;
    passage_request.gates = gate_views.data();
    passage_request.gate_count = gate_views.size();
    passage_request.departure_offset_seconds = request.departure_offset_seconds;
    ocpn_portable_passage_result result{};
    result.route.points = points.data();
    result.route.point_capacity = points.size();
    result.route.isochrone_points = isochrone_points.data();
    result.route.isochrone_point_capacity = isochrone_points.size();
    result.route.isochrones = isochrones.data();
    result.route.isochrone_capacity = isochrones.size();
    result.route.trace_points = trace_points.data();
    result.route.trace_point_capacity = trace_points.size();
    result.route.traces = traces.data();
    result.route.trace_capacity = traces.size();
    result.route.route_environment = environment.data();
    result.route.route_environment_capacity = environment.size();
    result.route.diagnostic = result_diagnostic.data();
    result.route.diagnostic_capacity = result_diagnostic.size();
    result.legs = legs.data();
    result.leg_capacity = legs.size();

    const int status = ocpn_portable_runtime_calculate_passage(
        worker_runtime.get(), &passage_request, &result, error.data(),
        error.size());
    worker_runtime.reset();

    const auto& route = result.route;
    const bool bounded =
        route.point_count <= points.size() &&
        route.route_environment_count <= environment.size() &&
        route.isochrone_point_count <= isochrone_points.size() &&
        route.isochrone_count <= isochrones.size() &&
        route.trace_point_count <= trace_points.size() &&
        route.trace_count <= traces.size() &&
        route.diagnostic_len < result_diagnostic.size() &&
        result.leg_count <= legs.size();
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
        valid_lines(isochrones, route.isochrone_count,
                    route.isochrone_point_count) &&
        valid_lines(traces, route.trace_count, route.trace_point_count);
    const bool valid_points =
        bounded &&
        std::all_of(points.begin(), points.begin() + route.point_count,
                    ValidRoutePoint) &&
        std::all_of(isochrone_points.begin(),
                    isochrone_points.begin() + route.isochrone_point_count,
                    ValidRoutePoint) &&
        std::all_of(trace_points.begin(),
                    trace_points.begin() + route.trace_point_count,
                    ValidRoutePoint);
    bool chronological = true;
    for (std::size_t index = 1; index < route.point_count; ++index) {
      if (points[index].unix_time <= points[index - 1].unix_time) {
        chronological = false;
        break;
      }
    }
    const bool valid_environment =
        bounded && route.route_environment_count == route.point_count &&
        std::all_of(environment.begin(),
                    environment.begin() + route.route_environment_count,
                    [](const auto& point) {
                      return std::isfinite(point.latitude) &&
                             std::isfinite(point.longitude) &&
                             std::isfinite(point.wind_u_knots) &&
                             std::isfinite(point.wind_v_knots) &&
                             std::isfinite(point.current_u_knots) &&
                             std::isfinite(point.current_v_knots) &&
                             std::isfinite(point.wave_height_metres) &&
                             (point.available & ~std::uint8_t{3}) == 0;
                    });
    bool environment_matches_route = valid_environment;
    for (std::size_t index = 0;
         environment_matches_route && index < route.point_count; ++index) {
      environment_matches_route =
          environment[index].latitude == points[index].latitude &&
          environment[index].longitude == points[index].longitude &&
          environment[index].unix_time == points[index].unix_time;
    }
    bool valid_legs = bounded && result.leg_count == request.gates.size() - 1;
    for (std::size_t index = 0; valid_legs && index < result.leg_count;
         ++index) {
      const auto& leg = legs[index];
      valid_legs =
          leg.start_gate_index == index && leg.end_gate_index == index + 1 &&
          leg.point_count >= 2 && leg.point_offset <= route.point_count &&
          leg.point_count <= route.point_count - leg.point_offset &&
          leg.arrival_unix_time > leg.departure_unix_time &&
          points[leg.point_offset].unix_time == leg.departure_unix_time &&
          points[leg.point_offset + leg.point_count - 1].unix_time ==
              leg.arrival_unix_time &&
          std::isfinite(leg.distance_nautical_miles) &&
          leg.distance_nautical_miles >= 0.0;
    }
    const std::array<double, 10> metrics = {
        route.distance_nautical_miles, route.average_speed_knots,
        route.maximum_speed_knots,     route.average_sog_knots,
        route.maximum_sog_knots,       route.average_wind_knots,
        route.maximum_wind_knots,      route.average_current_knots,
        route.maximum_current_knots,   route.estimated_fuel_litres};
    const bool valid_metrics =
        std::all_of(metrics.begin(), metrics.end(),
                    [](double value) {
                      return std::isfinite(value) && value >= 0.0;
                    }) &&
        route.comfort_level >= 1 && route.comfort_level <= 3 &&
        (route.metrics_available & ~std::uint8_t{3}) == 0;
    const bool valid_success =
        status != 0 ||
        (route.point_count >= 2 && chronological && environment_matches_route &&
         valid_legs && valid_metrics);
    execution.success =
        status == 0 && bounded && valid_spans && valid_points && valid_success;
    if (!bounded || !valid_spans || !valid_points || !valid_success) {
      execution.failure =
          "passage-routing component returned an invalid or oversized result";
      return execution;
    }

    auto& outcome = execution.outcome;
    outcome.points.assign(points.begin(), points.begin() + route.point_count);
    outcome.route_environment.assign(
        environment.begin(),
        environment.begin() + route.route_environment_count);
    auto copy_lines = [](const auto& source_points, const auto& source_lines,
                         std::size_t line_count, auto* destination) {
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
    copy_lines(isochrone_points, isochrones, route.isochrone_count,
               &outcome.isochrones);
    copy_lines(trace_points, traces, route.trace_count, &outcome.traces);
    outcome.diagnostic.assign(result_diagnostic.data(), route.diagnostic_len);
    outcome.distance_nautical_miles = route.distance_nautical_miles;
    outcome.duration_seconds = route.duration_seconds;
    outcome.states_examined = route.states_examined;
    outcome.average_speed_knots = route.average_speed_knots;
    outcome.maximum_speed_knots = route.maximum_speed_knots;
    outcome.average_sog_knots = route.average_sog_knots;
    outcome.maximum_sog_knots = route.maximum_sog_knots;
    outcome.average_wind_knots = route.average_wind_knots;
    outcome.maximum_wind_knots = route.maximum_wind_knots;
    outcome.average_current_knots = route.average_current_knots;
    outcome.maximum_current_knots = route.maximum_current_knots;
    outcome.tacks = route.tacks;
    outcome.motor_seconds = route.motor_seconds;
    outcome.estimated_fuel_litres = route.estimated_fuel_litres;
    outcome.propulsion_transitions = route.propulsion_transitions;
    outcome.comfort_level = route.comfort_level;
    outcome.metrics_available = route.metrics_available;
    outcome.validation_samples = result.validation_samples;
    outcome.passage_legs.reserve(result.leg_count);
    for (std::size_t index = 0; index < result.leg_count; ++index) {
      const auto& leg = legs[index];
      outcome.passage_legs.push_back(
          {leg.start_gate_index, leg.end_gate_index, leg.point_offset,
           leg.point_count, leg.departure_unix_time, leg.arrival_unix_time,
           leg.distance_nautical_miles, leg.states_examined});
    }
    if (!execution.success) {
      execution.failure =
          error[0] ? error.data() : execution.outcome.diagnostic;
      if (execution.failure.empty())
        execution.failure = "passage calculation failed";
    }
  } catch (const std::exception& exception) {
    execution.failure =
        std::string("passage worker failed safely: ") + exception.what();
  } catch (...) {
    execution.failure = "passage worker failed safely with an unknown error";
  }
  return execution;
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
    std::uint32_t portable_api = OCPN_PORTABLE_API_V01;
    std::uint32_t portable_world = OCPN_PORTABLE_WORLD_PLUGIN;
    std::set<std::string> requested_permissions;
    std::set<std::string> permissions;
    std::set<std::string> https_domains;
    std::deque<std::chrono::steady_clock::time_point> https_history;
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
    std::atomic_bool event_pump_scheduled{false};
    std::size_t routing_call_count = 0;
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

  struct TimerKey {
    std::string package_id;
    std::string timer_id;

    bool operator<(const TimerKey& other) const {
      return std::tie(package_id, timer_id) <
             std::tie(other.package_id, other.timer_id);
    }
  };

  struct TimerRegistration {
    Instance* instance = nullptr;
    std::uint64_t generation = 0;
    std::chrono::steady_clock::time_point due;
    std::int64_t scheduled_unix_milliseconds = 0;
    std::uint32_t repeat_milliseconds = 0;
  };

  struct RpcCallKey {
    std::string source_package;
    std::string correlation_id;

    bool operator<(const RpcCallKey& other) const {
      return std::tie(source_package, correlation_id) <
             std::tie(other.source_package, other.correlation_id);
    }
  };

  struct RpcCall {
    Instance* source = nullptr;
    std::uint64_t source_generation = 0;
    std::string target_package;
    std::string service;
    std::chrono::steady_clock::time_point expires;
  };

  Impl(std::string storage_root, RegisterAction register_action,
       RemoveActions remove_actions, StateChanged state_changed,
       UiDispatch ui_dispatch)
      : storage_root(std::move(storage_root)),
        register_action(std::move(register_action)),
        remove_actions(std::move(remove_actions)),
        state_changed(std::move(state_changed)),
        ui_dispatch(std::move(ui_dispatch)),
        timer_thread([this]() { RunTimers(); }) {}

  ~Impl() {
    Shutdown();
    {
      std::lock_guard<std::mutex> lock(timer_mutex);
      timer_stopped = true;
      timers.clear();
    }
    timer_changed.notify_all();
    if (timer_thread.joinable()) timer_thread.join();
  }

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
  void SetPluginMessageSender(PluginMessageSender callback) {
    plugin_message_sender = std::move(callback);
  }
  void SetAuthorUiRequestCallback(AuthorUiRequest callback) {
    author_ui_request = std::move(callback);
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
  bool HandleAction(const std::string& package_id, const std::string& action_id,
                    RuntimeActionContext context = {});
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
  bool CalculateRouteBlocking(const std::string& package_id,
                              RoutingRequest request, RoutingOutcome* outcome,
                              std::string* diagnostic);
  bool CalculatePassageBlocking(const std::string& package_id,
                                RoutingPassageRequest request,
                                RoutingOutcome* outcome,
                                std::string* diagnostic);
  bool BeginRouteAttempt(const std::string& package_id,
                         std::string* diagnostic);
  bool PreflightEnvironment(const std::string& package_id, double latitude,
                            double longitude,
                            const std::vector<std::int64_t>& unix_times,
                            std::vector<std::uint8_t>* availability,
                            std::string* diagnostic);
  bool CancelRoute(const std::string& package_id);
  bool WaitForRoute(const std::string& package_id,
                    std::chrono::milliseconds timeout);
  bool WaitForIdle(const std::string& package_id,
                   std::chrono::milliseconds timeout);
  void DeliverNavigationSentence(const std::string& sentence);
  void PublishCapabilityEvent(CapabilityEvent event) {
    std::string diagnostic;
    if (!events.Publish(std::move(event), &diagnostic)) {
      if (!diagnostic.empty())
        wxLogWarning("PPM capability-event-rejected diagnostic=%s", diagnostic);
      return;
    }
    for (auto& item : instances) ScheduleEvents(*item);
  }
  void DeliverPluginMessage(const std::string& message_id,
                            const std::string& message_body);
  void ScheduleEvents(Instance& instance);
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
  bool QueryChartSafety(const std::vector<ocpn_portable_geo_segment>& input,
                        const ChartSafetyServiceOptions& options,
                        std::chrono::milliseconds advisory_timeout,
                        std::vector<ChartSafetyServiceResult>* output) {
    if (!output) return false;

    // The manager-owned semantic decoder and immutable cell cache are
    // thread-safe. Keep this potentially substantial work on the routing
    // worker instead of posting it to the GUI thread.
    *output = chart_safety.QuerySemantic(input, options);
    if (output->size() != input.size() || options.require_authoritative)
      return output->size() == input.size();

    std::vector<ocpn_portable_geo_segment> fallback_segments;
    std::vector<std::size_t> fallback_indices;
    fallback_segments.reserve(output->size());
    fallback_indices.reserve(output->size());
    for (std::size_t index = 0; index < output->size(); ++index) {
      if ((*output)[index].state != 2U && (*output)[index].state != 3U)
        continue;
      fallback_segments.push_back(input[index]);
      fallback_indices.push_back(index);
    }
    if (fallback_segments.empty()) return true;

    // GSHHS is deliberately only an advisory fallback. Its public OpenCPN
    // wrapper requires main-thread initialisation, so dispatch only these
    // unresolved segments rather than the semantic CM93 batch.
    auto fallback = std::make_shared<std::vector<ChartSafetyServiceResult>>();
    if (!RunUiService(
            [this, fallback_segments = std::move(fallback_segments),
             fallback]() {
              *fallback =
                  chart_safety.QueryAdvisoryCoastline(fallback_segments);
            },
            advisory_timeout)) {
      return false;
    }
    if (fallback->size() != fallback_indices.size()) return false;
    for (std::size_t index = 0; index < fallback->size(); ++index)
      (*output)[fallback_indices[index]] = (*fallback)[index];
    return true;
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
                                       const std::string& surface_id,
                                       const std::string& role = {});
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
  static std::int32_t ChartsQueryFinalSafety(
      void* user_data, const ocpn_portable_geo_segment* segments,
      std::size_t segment_count,
      const ocpn_portable_final_chart_safety_options* options,
      ocpn_portable_chart_segment_result* results, std::size_t result_count);
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
  static std::int32_t SendPluginMessage(void* user_data, const char* message_id,
                                        std::size_t message_id_length,
                                        const char* message_body,
                                        std::size_t message_body_length);
  static std::int32_t AuthorServiceCall(void* user_data, const char* operation,
                                        std::size_t operation_length,
                                        const char* request_json,
                                        std::size_t request_json_length,
                                        char* response_json,
                                        std::size_t response_capacity,
                                        std::size_t* response_length);
  void DeliverJobEvent(Instance* instance, const JobEvent& event);
  void RunTimers();
  void CancelPackageTimers(const std::string& package_id);
  void ClearPackageRpc(const std::string& package_id);

  fs::path storage_root;
  RegisterAction register_action;
  RemoveActions remove_actions;
  StateChanged state_changed;
  UiDispatch ui_dispatch;
  SurfaceOpened surface_opened;
  SurfaceResponse surface_response;
  RoutingProgress routing_progress;
  RoutingCompleted routing_completed;
  PluginMessageSender plugin_message_sender;
  AuthorUiRequest author_ui_request;
  JobScheduler jobs;
  CapabilityEventBroker events;
  ChartSafetyService chart_safety;
  std::vector<std::unique_ptr<Instance>> instances;
  bool stopped = false;
  std::mutex timer_mutex;
  std::condition_variable timer_changed;
  std::map<TimerKey, TimerRegistration> timers;
  bool timer_stopped = false;
  std::thread timer_thread;
  std::mutex rpc_mutex;
  std::map<std::string, std::string> rpc_services;
  std::map<RpcCallKey, RpcCall> rpc_calls;
  mutable std::mutex position_mutex;
  bool position_valid = false;
  double latitude = 0.0;
  double longitude = 0.0;
  double cog = 0.0;
  double sog = 0.0;
  bool has_cog = false;
  bool has_sog = false;
  double heading_true = 0.0;
  double heading_magnetic = 0.0;
  double magnetic_variation = 0.0;
  bool has_heading_true = false;
  bool has_heading_magnetic = false;
  bool has_magnetic_variation = false;
  std::int64_t fix_unix_time = 0;
  std::uint16_t satellites = 0;
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

void RuntimeEngine::Impl::CancelPackageTimers(const std::string& package_id) {
  {
    std::lock_guard<std::mutex> lock(timer_mutex);
    for (auto item = timers.begin(); item != timers.end();) {
      if (item->first.package_id == package_id)
        item = timers.erase(item);
      else
        ++item;
    }
  }
  timer_changed.notify_all();
}

void RuntimeEngine::Impl::ClearPackageRpc(const std::string& package_id) {
  std::lock_guard<std::mutex> lock(rpc_mutex);
  for (auto item = rpc_services.begin(); item != rpc_services.end();) {
    if (item->second == package_id)
      item = rpc_services.erase(item);
    else
      ++item;
  }
  for (auto item = rpc_calls.begin(); item != rpc_calls.end();) {
    if (item->first.source_package == package_id ||
        item->second.target_package == package_id)
      item = rpc_calls.erase(item);
    else
      ++item;
  }
}

void RuntimeEngine::Impl::RunTimers() {
  while (true) {
    std::vector<std::pair<TimerKey, TimerRegistration>> due;
    {
      std::unique_lock<std::mutex> lock(timer_mutex);
      if (timer_stopped) return;
      auto wake =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
      for (const auto& [ignored, timer] : timers)
        wake = std::min(wake, timer.due);
      timer_changed.wait_until(lock, wake);
      if (timer_stopped) return;
      const auto now = std::chrono::steady_clock::now();
      for (auto item = timers.begin(); item != timers.end();) {
        if (item->second.due > now) {
          ++item;
          continue;
        }
        due.push_back(*item);
        if (item->second.repeat_milliseconds == 0) {
          item = timers.erase(item);
          continue;
        }
        const auto interval =
            std::chrono::milliseconds(item->second.repeat_milliseconds);
        do {
          item->second.due += interval;
          item->second.scheduled_unix_milliseconds +=
              item->second.repeat_milliseconds;
        } while (item->second.due <= now);
        ++item;
      }
    }

    for (const auto& [key, timer] : due) {
      Instance* instance = timer.instance;
      if (!instance) continue;
      const std::int64_t fired = UnixMillisecondsNow();
      instance->executor.Post(
          timer.generation,
          [instance, key, timer, fired](std::uint64_t task_generation) {
            if (task_generation != instance->executor.Generation() ||
                !instance->enabled || instance->failed)
              return;
            std::lock_guard<std::mutex> lock(instance->runtime_mutex);
            if (!instance->runtime) return;
            std::array<char, kErrorCapacity> error{};
            if (ocpn_portable_runtime_on_timer(
                    instance->runtime, key.timer_id.data(), key.timer_id.size(),
                    timer.scheduled_unix_milliseconds, fired, error.data(),
                    error.size()) != 0) {
              instance->owner->Fail(
                  *instance, "timer " + key.timer_id,
                  error[0] ? error.data() : "portable timer handler failed");
            }
          });
    }

    std::vector<std::pair<RpcCallKey, RpcCall>> expired;
    {
      std::lock_guard<std::mutex> lock(rpc_mutex);
      const auto now = std::chrono::steady_clock::now();
      for (auto item = rpc_calls.begin(); item != rpc_calls.end();) {
        if (item->second.expires > now) {
          ++item;
          continue;
        }
        expired.push_back(*item);
        item = rpc_calls.erase(item);
      }
    }
    for (const auto& [key, call] : expired) {
      Instance* source = call.source;
      if (!source) continue;
      wxJSONValue response;
      response["correlation_id"] = key.correlation_id;
      response["status"] = 504;
      response["content_type"] = "text/plain";
      response["payload_base64"] = "";
      response["diagnostic"] = "portable RPC request timed out";
      const std::string encoded = JsonText(response);
      source->executor.Post(
          call.source_generation, [source, provider = call.target_package,
                                   encoded](std::uint64_t task_generation) {
            if (task_generation != source->executor.Generation() ||
                !source->enabled || source->failed)
              return;
            std::lock_guard<std::mutex> lock(source->runtime_mutex);
            if (!source->runtime) return;
            std::array<char, kErrorCapacity> error{};
            if (ocpn_portable_runtime_on_rpc_response(
                    source->runtime, provider.data(), provider.size(),
                    encoded.data(), encoded.size(), error.data(),
                    error.size()) != 0) {
              source->owner->Fail(*source, "RPC timeout response",
                                  error[0]
                                      ? error.data()
                                      : "portable RPC response handler failed");
            }
          });
    }
  }
}

std::int32_t RuntimeEngine::Impl::OpenNamedSurface(
    void* user_data, const std::string& surface_id, const std::string& role) {
  auto* instance = static_cast<Instance*>(user_data);
  const bool has_permission =
      instance && instance->owner &&
      (instance->owner->Permitted(*instance, "ui.commands") ||
       (instance->portable_api == OCPN_PORTABLE_API_V04 &&
        instance->owner->Permitted(*instance, "ui.surfaces")));
  if (!instance || !instance->owner || !has_permission || !instance->enabled ||
      instance->failed) {
    return -1;
  }
  const auto item = instance->surfaces.find(surface_id);
  if (item == instance->surfaces.end() || !instance->owner->surface_opened)
    return -2;
  const std::string package_id = instance->id;
  DeclarativeSurface surface = item->second;
  if (!role.empty()) surface.role = role;
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
  if (!ValidateRoutingRequest(request, diagnostic)) return false;

  std::thread previous_worker;
  {
    std::lock_guard<std::mutex> route_lock(instance->routing_mutex);
    if (instance->routing_running || instance->routing_call_count != 0) {
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
    if (!instance->enabled || instance->failed || instance->routing_running ||
        instance->routing_call_count != 0) {
      ocpn_portable_runtime_destroy(replica);
      if (diagnostic) *diagnostic = "routing package changed state";
      return false;
    }
    instance->routing_cancelled = false;
    instance->routing_running = true;
    try {
      instance->routing_worker = std::thread(
          [instance, replica, request = std::move(request)]() mutable {
            RoutingExecution execution =
                ExecuteRoute(replica, std::move(request));
            {
              std::lock_guard<std::mutex> route_lock(instance->routing_mutex);
              instance->routing_running = false;
            }
            instance->routing_changed.notify_all();
            const auto completed = instance->owner->routing_completed;
            if (completed) {
              const std::string id = instance->id;
              instance->owner->Publish(
                  [completed, id, execution = std::move(execution)]() mutable {
                    completed(id, execution.success,
                              std::move(execution.outcome), execution.failure);
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

bool RuntimeEngine::Impl::CalculateRouteBlocking(const std::string& package_id,
                                                 RoutingRequest request,
                                                 RoutingOutcome* outcome,
                                                 std::string* diagnostic) {
  Instance* instance = Find(package_id);
  if (!outcome || !instance || !instance->enabled || instance->failed ||
      !instance->runtime || !Permitted(*instance, "weather-routing.compute")) {
    if (diagnostic)
      *diagnostic =
          !outcome ? "routing output is required"
                   : "routing package is unavailable or request is invalid";
    return false;
  }
  if (!ValidateRoutingRequest(request, diagnostic)) return false;

  {
    std::lock_guard<std::mutex> route_lock(instance->routing_mutex);
    if (instance->routing_running) {
      if (diagnostic) *diagnostic = "a route calculation is already running";
      return false;
    }
    if (instance->routing_call_count == 0) instance->routing_cancelled = false;
    if (instance->routing_cancelled || !instance->enabled || instance->failed) {
      if (diagnostic) *diagnostic = "route calculation was cancelled";
      return false;
    }
    ++instance->routing_call_count;
  }
  auto finish_call = [instance]() {
    {
      std::lock_guard<std::mutex> route_lock(instance->routing_mutex);
      if (instance->routing_call_count != 0) --instance->routing_call_count;
    }
    instance->routing_changed.notify_all();
  };

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
    finish_call();
    if (diagnostic)
      *diagnostic = clone_error[0] ? clone_error.data()
                                   : "could not create routing worker";
    return false;
  }

  RoutingExecution execution = ExecuteRoute(replica, std::move(request));
  finish_call();
  *outcome = std::move(execution.outcome);
  if (diagnostic) *diagnostic = execution.failure;
  return execution.success;
}

bool RuntimeEngine::Impl::CalculatePassageBlocking(
    const std::string& package_id, RoutingPassageRequest request,
    RoutingOutcome* outcome, std::string* diagnostic) {
  Instance* instance = Find(package_id);
  if (!outcome || !instance || !instance->enabled || instance->failed ||
      !instance->runtime || !Permitted(*instance, "weather-routing.compute")) {
    if (diagnostic)
      *diagnostic = !outcome ? "passage output is required"
                             : "passage-routing package is unavailable";
    return false;
  }
  if (!ValidatePassageRequest(request, diagnostic)) return false;

  {
    std::lock_guard<std::mutex> route_lock(instance->routing_mutex);
    if (instance->routing_running) {
      if (diagnostic) *diagnostic = "a route calculation is already running";
      return false;
    }
    if (instance->routing_call_count == 0) instance->routing_cancelled = false;
    if (instance->routing_cancelled || !instance->enabled || instance->failed) {
      if (diagnostic) *diagnostic = "passage calculation was cancelled";
      return false;
    }
    ++instance->routing_call_count;
  }
  auto finish_call = [instance]() {
    {
      std::lock_guard<std::mutex> route_lock(instance->routing_mutex);
      if (instance->routing_call_count != 0) --instance->routing_call_count;
    }
    instance->routing_changed.notify_all();
  };

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
    finish_call();
    if (diagnostic)
      *diagnostic = clone_error[0] ? clone_error.data()
                                   : "could not create passage worker";
    return false;
  }

  RoutingExecution execution = ExecutePassage(replica, std::move(request));
  finish_call();
  *outcome = std::move(execution.outcome);
  if (diagnostic) *diagnostic = execution.failure;
  return execution.success;
}

bool RuntimeEngine::Impl::BeginRouteAttempt(const std::string& package_id,
                                            std::string* diagnostic) {
  Instance* instance = Find(package_id);
  if (!instance || !instance->enabled || instance->failed ||
      !instance->runtime || !Permitted(*instance, "weather-routing.compute")) {
    if (diagnostic) *diagnostic = "routing package is unavailable";
    return false;
  }
  std::lock_guard<std::mutex> route_lock(instance->routing_mutex);
  if (instance->routing_running || instance->routing_call_count != 0) {
    if (diagnostic) *diagnostic = "a route calculation is already running";
    return false;
  }
  instance->routing_cancelled = false;
  if (diagnostic) diagnostic->clear();
  return true;
}

bool RuntimeEngine::Impl::PreflightEnvironment(
    const std::string& package_id, double latitude, double longitude,
    const std::vector<std::int64_t>& unix_times,
    std::vector<std::uint8_t>* availability, std::string* diagnostic) {
  Instance* consumer = Find(package_id);
  if (!availability || !consumer || !consumer->enabled || consumer->failed ||
      !Permitted(*consumer, "environment.consume") ||
      !std::isfinite(latitude) || !std::isfinite(longitude) ||
      std::abs(latitude) > 90.0 || std::abs(longitude) > 180.0 ||
      unix_times.empty() || unix_times.size() > 256) {
    if (diagnostic) *diagnostic = "invalid environmental preflight request";
    return false;
  }
  const Instance* provider =
      CompatibleProvider(*consumer, "org.opencpn.environment.provider");
  if (!provider || !provider->environment_provider) {
    if (diagnostic)
      *diagnostic = "no compatible environmental provider is enabled";
    return false;
  }
  std::vector<EnvironmentRequest> requests;
  requests.reserve(unix_times.size());
  for (const std::int64_t unix_time : unix_times)
    requests.push_back({latitude, longitude, unix_time});
  std::vector<EnvironmentSample> samples;
  if (!provider->environment_provider->SampleBatch(
          requests, &samples,
          [consumer]() {
            return !consumer->enabled || consumer->failed ||
                   consumer->routing_cancelled || !consumer->owner ||
                   consumer->owner->stopped;
          },
          diagnostic) ||
      samples.size() != unix_times.size()) {
    if (diagnostic && diagnostic->empty())
      *diagnostic = "environmental provider returned an invalid batch";
    return false;
  }
  availability->clear();
  availability->reserve(samples.size());
  for (const auto& sample : samples)
    availability->push_back(static_cast<std::uint8_t>(sample.available));
  return true;
}

bool RuntimeEngine::Impl::CancelRoute(const std::string& package_id) {
  Instance* instance = Find(package_id);
  if (!instance) return false;
  instance->routing_cancelled = true;
  std::lock_guard<std::mutex> route_lock(instance->routing_mutex);
  return instance->routing_running || instance->routing_call_count != 0;
}

bool RuntimeEngine::Impl::WaitForRoute(const std::string& package_id,
                                       std::chrono::milliseconds timeout) {
  Instance* instance = Find(package_id);
  if (!instance) return false;
  std::thread worker;
  {
    std::unique_lock<std::mutex> route_lock(instance->routing_mutex);
    if (!instance->routing_changed.wait_for(route_lock, timeout, [instance]() {
          return !instance->routing_running &&
                 instance->routing_call_count == 0;
        })) {
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

std::int32_t RuntimeEngine::Impl::SendPluginMessage(
    void* user_data, const char* message_id, std::size_t message_id_length,
    const char* message_body, std::size_t message_body_length) {
  auto* instance = static_cast<Instance*>(user_data);
  const std::string id = Text(message_id, message_id_length);
  const std::string body = Text(message_body, message_body_length);
  if (!instance || !instance->owner || !instance->enabled || instance->failed ||
      id.empty() || id.size() > CapabilityEventBroker::kMaximumTopicBytes ||
      body.size() > CapabilityEventBroker::kMaximumPayloadBytes ||
      !instance->owner->Permitted(*instance, "plugin.messages.send") ||
      !instance->owner->plugin_message_sender ||
      !std::all_of(id.begin(), id.end(), [](unsigned char character) {
        return character >= 0x20 && character != 0x7f;
      })) {
    return -1;
  }
  const auto sender = instance->owner->plugin_message_sender;
  return instance->owner->RunUiService(
             [sender, id, body]() { sender(id, body); },
             std::chrono::seconds(2))
             ? 0
             : -2;
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
  action.locations = {"toolbar"};
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

void RuntimeEngine::Impl::RoutingProgressCallback(void* user_data,
                                                  std::uint8_t percent,
                                                  const char* message,
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
  return instance && (instance->routing_cancelled || !instance->enabled ||
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
  std::vector<ChartSafetyServiceResult> assessments;
  if (!instance->owner->QueryChartSafety(
          input, ChartSafetyServiceOptions{0.0, 0.0, false},
          std::chrono::seconds(15), &assessments)) {
    wxLogWarning(
        "PPM advisory chart fallback timed out waiting for the UI thread");
    return -2;
  }
  if (assessments.size() != input.size()) return -3;
  for (std::size_t index = 0; index < input.size(); ++index) {
    results[index] = {assessments[index].state,
                      assessments[index].charts_considered,
                      assessments[index].reason};
  }
  return 0;
}

std::int32_t RuntimeEngine::Impl::ChartsQueryFinalSafety(
    void* user_data, const ocpn_portable_geo_segment* segments,
    std::size_t segment_count,
    const ocpn_portable_final_chart_safety_options* options,
    ocpn_portable_chart_segment_result* results, std::size_t result_count) {
  auto* instance = static_cast<Instance*>(user_data);
  if (!instance || !instance->owner || !segments || !options || !results ||
      result_count != segment_count || segment_count > 10'000 ||
      !instance->owner->Permitted(*instance, "charts.segment-safety")) {
    return -1;
  }
  const std::vector<ocpn_portable_geo_segment> input(segments,
                                                     segments + segment_count);
  const ChartSafetyServiceOptions service_options{
      options->safety_margin_nautical_miles, options->minimum_depth_metres,
      options->require_authoritative != 0};
  std::vector<ChartSafetyServiceResult> output;
  if (!instance->owner->QueryChartSafety(input, service_options,
                                         std::chrono::seconds(15), &output)) {
    wxLogWarning(
        "PPM final advisory chart fallback timed out waiting for the UI "
        "thread");
    return -2;
  }
  if (output.size() != segment_count) return -3;
  for (std::size_t index = 0; index < segment_count; ++index) {
    results[index] = {output[index].state, output[index].charts_considered,
                      output[index].reason};
  }
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

std::int32_t RuntimeEngine::Impl::AuthorServiceCall(
    void* user_data, const char* operation, std::size_t operation_length,
    const char* request_json, std::size_t request_json_length,
    char* response_json, std::size_t response_capacity,
    std::size_t* response_length) {
  auto* instance = static_cast<Instance*>(user_data);
  const std::string operation_name = Text(operation, operation_length);
  const std::string request_text = Text(request_json, request_json_length);
  if (!instance || !instance->owner || !response_length ||
      (instance->portable_api != OCPN_PORTABLE_API_V03 &&
       instance->portable_api != OCPN_PORTABLE_API_V04) ||
      operation_name.empty() || operation_name.size() > 128 ||
      request_text.size() > kAuthorRequestLimit) {
    return -1;
  }

  wxJSONValue request;
  if (!ParseAuthorRequest(request_text, &request)) return -2;

  auto finish = [&](std::int32_t status, const wxJSONValue& value) {
    const std::string encoded = JsonText(value);
    *response_length = encoded.size();
    if (encoded.size() > kAuthorResponseLimit ||
        encoded.size() > response_capacity ||
        (!encoded.empty() && !response_json)) {
      return std::int32_t{-3};
    }
    if (!encoded.empty())
      std::memcpy(response_json, encoded.data(), encoded.size());
    return status;
  };
  auto fail = [&](std::int32_t status, const std::string& code,
                  const std::string& message, bool retryable = false) {
    return finish(status, AuthorError(code, message, retryable));
  };

  if (operation_name == "navigation.get-vessel-position") {
    if (!instance->owner->Permitted(*instance, "navigation.position.read"))
      return fail(-4, "permission-denied",
                  "vessel position permission was not granted");
    wxJSONValue response;
    {
      std::lock_guard<std::mutex> lock(instance->owner->position_mutex);
      if (!instance->owner->position_valid)
        return fail(-6, "position-unavailable",
                    "OpenCPN has no valid vessel position", true);
      const auto& owner = *instance->owner;
      response["latitude"] = owner.latitude;
      response["longitude"] = owner.longitude;
      response["course_over_ground"] =
          owner.has_cog ? wxJSONValue(owner.cog) : wxJSONValue(wxJSONTYPE_NULL);
      response["speed_over_ground"] =
          owner.has_sog ? wxJSONValue(owner.sog) : wxJSONValue(wxJSONTYPE_NULL);
      response["heading_true"] = owner.has_heading_true
                                     ? wxJSONValue(owner.heading_true)
                                     : wxJSONValue(wxJSONTYPE_NULL);
      response["heading_magnetic"] = owner.has_heading_magnetic
                                         ? wxJSONValue(owner.heading_magnetic)
                                         : wxJSONValue(wxJSONTYPE_NULL);
      response["magnetic_variation"] =
          owner.has_magnetic_variation ? wxJSONValue(owner.magnetic_variation)
                                       : wxJSONValue(wxJSONTYPE_NULL);
      response["fix_unix_time"] =
          static_cast<wxLongLong_t>(owner.fix_unix_time);
      response["satellites"] = owner.satellites;
    }
    return finish(0, response);
  }

  if (operation_name == "actions.register") {
    if (!instance->owner->Permitted(*instance, "ui.commands"))
      return fail(-4, "permission-denied",
                  "command registration permission was not granted");
    RuntimeAction action;
    action.package_id = instance->id;
    action.action_id = request["action_id"].AsString().ToStdString();
    action.label = request["label"].AsString().ToStdString();
    action.tooltip = request["tooltip"].AsString().ToStdString();
    action.toolbar = false;
    action.context_menu = false;
    action.locations.clear();
    if (!IsSafeName(action.action_id) || action.label.empty() ||
        action.label.size() > 256 || action.tooltip.size() > 1024 ||
        !request["locations"].IsArray()) {
      return fail(-5, "invalid-action", "command definition is invalid");
    }
    for (int index = 0; index < request["locations"].Size(); ++index) {
      const wxString location = request["locations"][index].AsString();
      if (location == "toolbar")
        action.toolbar = true;
      else if (location == "chart-context-menu") {
        action.context_menu = true;
      } else if (instance->portable_api == OCPN_PORTABLE_API_V04 &&
                 (location == "ais-context-menu" ||
                  location == "route-context-menu" ||
                  location == "waypoint-context-menu" ||
                  location == "track-context-menu")) {
        action.context_menu = true;
      } else {
        return fail(-5, "invalid-action", "command location is invalid");
      }
      action.locations.push_back(location.ToStdString());
    }
    if (!action.toolbar && !action.context_menu)
      return fail(-5, "invalid-action",
                  "command must have at least one location");
    if (request["icon_resource"].IsString()) {
      const wxString resource = request["icon_resource"].AsString();
      if (!SafeRelativePath(resource))
        return fail(-5, "invalid-action", "command icon path is invalid");
      action.icon_path =
          (instance->package_root / fs::path(resource.ToStdString()))
              .lexically_normal()
              .string();
      std::error_code error;
      if (!fs::is_regular_file(action.icon_path, error))
        return fail(-6, "not-found", "command icon resource was not found");
    }
    auto status = std::make_shared<std::int32_t>(-10);
    auto host_id = std::make_shared<std::uint32_t>(0);
    const bool completed = instance->owner->RunUiService(
        [owner = instance->owner, action, status, host_id]() {
          *status = owner->register_action(action, host_id.get());
        },
        std::chrono::seconds(5));
    if (!completed || *status != 0)
      return fail(completed ? *status : -11, "registration-failed",
                  completed ? "OpenCPN rejected the command"
                            : "OpenCPN command registration timed out",
                  !completed);
    instance->registered_actions.push_back(action);
    wxJSONValue response;
    response["host_action_id"] = static_cast<wxLongLong_t>(*host_id);
    return finish(0, response);
  }

  if (operation_name == "scenes.clear") {
    if (!instance->owner->Permitted(*instance, "overlay.submit"))
      return fail(-4, "permission-denied",
                  "scene submission permission was not granted");
    const std::string scene_id = request["scene_id"].AsString().ToStdString();
    if (!IsSafeName(scene_id))
      return fail(-5, "invalid-scene", "scene identifier is invalid");
    {
      std::lock_guard<std::mutex> lock(instance->state_mutex);
      instance->scenes.erase(scene_id);
    }
    instance->owner->PublishStateChanged();
    return finish(0, wxJSONValue(wxJSONTYPE_OBJECT));
  }

  if (operation_name == "scenes.submit") {
    if (!instance->owner->Permitted(*instance, "overlay.submit"))
      return fail(-4, "permission-denied",
                  "scene submission permission was not granted");
    OverlayScene update;
    update.package_id = instance->id;
    update.scene_id = request["scene_id"].AsString().ToStdString();
    update.revision = static_cast<std::uint64_t>(request["revision"].AsLong());
    update.canvas_target =
        request.HasMember("canvas_target")
            ? request["canvas_target"].AsString().ToStdString()
            : "all";
    update.render_phase = request.HasMember("render_phase")
                              ? request["render_phase"].AsString().ToStdString()
                              : "below-vessels";
    if (request.HasMember("selected_canvases")) {
      if (!request["selected_canvases"].IsArray() ||
          request["selected_canvases"].Size() > 16)
        return fail(-5, "invalid-scene", "canvas selection is invalid");
      std::set<std::uint32_t> unique_canvases;
      for (int index = 0; index < request["selected_canvases"].Size();
           ++index) {
        const long canvas = request["selected_canvases"][index].AsLong();
        if (canvas < 0 || canvas > 15 ||
            !unique_canvases.insert(static_cast<std::uint32_t>(canvas)).second)
          return fail(-5, "invalid-scene", "canvas selection is invalid");
        update.selected_canvases.push_back(static_cast<std::uint32_t>(canvas));
      }
    }
    if (!IsSafeName(update.scene_id) || update.revision == 0 ||
        (update.canvas_target != "all" && update.canvas_target != "primary" &&
         update.canvas_target != "selected") ||
        (update.render_phase != "below-vessels" &&
         update.render_phase != "above-vessels" &&
         update.render_phase != "above-ui") ||
        (update.canvas_target == "selected" &&
         update.selected_canvases.empty()) ||
        !request["layers"].IsArray() ||
        static_cast<std::size_t>(request["layers"].Size()) > kSceneLayerLimit) {
      return fail(-5, "invalid-scene", "scene update is invalid");
    }

    std::size_t primitive_count = 0;
    std::size_t point_count = 0;
    std::set<std::string> layer_ids;
    std::set<std::string> primitive_ids;
    for (int layer_index = 0; layer_index < request["layers"].Size();
         ++layer_index) {
      wxJSONValue layer_value = request["layers"][layer_index];
      OverlayLayer layer;
      layer.layer_id = layer_value["layer_id"].AsString().ToStdString();
      layer.z_index = static_cast<int>(layer_value["z_index"].AsLong());
      layer.visible = layer_value["visible"].AsBool();
      if (!IsSafeName(layer.layer_id) ||
          !layer_ids.insert(layer.layer_id).second ||
          !layer_value["primitives"].IsArray()) {
        return fail(-5, "invalid-layer", "scene layer definition is invalid");
      }
      primitive_count +=
          static_cast<std::size_t>(layer_value["primitives"].Size());
      if (primitive_count > kScenePrimitiveLimit)
        return fail(-7, "scene-too-large", "scene exceeds the primitive limit");

      for (int primitive_index = 0;
           primitive_index < layer_value["primitives"].Size();
           ++primitive_index) {
        wxJSONValue primitive_value =
            layer_value["primitives"][primitive_index];
        OverlayPrimitive primitive;
        primitive.primitive_id =
            primitive_value["primitive_id"].AsString().ToStdString();
        primitive.interactive = primitive_value["interactive"].AsBool();
        const std::string globally_unique =
            layer.layer_id + "/" + primitive.primitive_id;
        if (!IsSafeName(primitive.primitive_id) ||
            !primitive_ids.insert(globally_unique).second) {
          return fail(-5, "invalid-primitive",
                      "scene primitive identifier is invalid or duplicated");
        }
        const std::string kind =
            primitive_value["kind"].AsString().ToStdString();
        if (kind == "polyline" || kind == "polygon") {
          primitive.kind = kind == "polyline" ? OverlayPrimitiveKind::kPolyline
                                              : OverlayPrimitiveKind::kPolygon;
          if (!primitive_value["points"].IsArray() ||
              !ParseOverlayStyle(primitive_value["style"], &primitive.style)) {
            return fail(-5, "invalid-primitive",
                        "line or polygon primitive is invalid");
          }
          const std::size_t required =
              kind == "polyline" ? std::size_t{2} : std::size_t{3};
          const std::size_t count =
              static_cast<std::size_t>(primitive_value["points"].Size());
          if (count < required || point_count > kScenePointLimit - count)
            return fail(-7, "scene-too-large",
                        "scene point count is invalid or exceeds policy");
          primitive.points.reserve(count);
          for (int point_index = 0;
               point_index < primitive_value["points"].Size(); ++point_index) {
            OverlayPoint point;
            if (!ParseOverlayPoint(primitive_value["points"][point_index],
                                   &point)) {
              return fail(-5, "invalid-coordinate",
                          "scene coordinate is invalid");
            }
            primitive.points.push_back(point);
          }
          point_count += count;
        } else if (kind == "circle") {
          primitive.kind = OverlayPrimitiveKind::kCircle;
          primitive.radius_metres = primitive_value["radius_metres"].AsDouble();
          if (!ParseOverlayPoint(primitive_value["centre"],
                                 &primitive.centre) ||
              !ParseOverlayStyle(primitive_value["style"], &primitive.style) ||
              !std::isfinite(primitive.radius_metres) ||
              primitive.radius_metres <= 0.0 ||
              primitive.radius_metres > 2'000'000.0) {
            return fail(-5, "invalid-circle", "circle primitive is invalid");
          }
        } else if (kind == "icon") {
          primitive.kind = OverlayPrimitiveKind::kIcon;
          primitive.width_pixels =
              static_cast<float>(primitive_value["width_pixels"].AsDouble());
          primitive.height_pixels =
              static_cast<float>(primitive_value["height_pixels"].AsDouble());
          if (primitive_value.HasMember("rotation_degrees"))
            primitive.rotation_degrees = static_cast<float>(
                primitive_value["rotation_degrees"].AsDouble());
          if (primitive_value.HasMember("anchor_x"))
            primitive.anchor_x =
                static_cast<float>(primitive_value["anchor_x"].AsDouble());
          if (primitive_value.HasMember("anchor_y"))
            primitive.anchor_y =
                static_cast<float>(primitive_value["anchor_y"].AsDouble());
          const wxString resource_name =
              primitive_value["resource_name"].AsString();
          if (!ParseOverlayPoint(primitive_value["position"],
                                 &primitive.centre) ||
              !SafeRelativePath(resource_name) ||
              !std::isfinite(primitive.width_pixels) ||
              !std::isfinite(primitive.height_pixels) ||
              !std::isfinite(primitive.rotation_degrees) ||
              !std::isfinite(primitive.anchor_x) ||
              !std::isfinite(primitive.anchor_y) ||
              primitive.rotation_degrees < -3600.0F ||
              primitive.rotation_degrees > 3600.0F ||
              primitive.anchor_x < 0.0F || primitive.anchor_x > 1.0F ||
              primitive.anchor_y < 0.0F || primitive.anchor_y > 1.0F ||
              primitive.width_pixels < 1.0F || primitive.height_pixels < 1.0F ||
              primitive.width_pixels > 512.0F ||
              primitive.height_pixels > 512.0F) {
            return fail(-5, "invalid-icon", "icon primitive is invalid");
          }
          const fs::path resource =
              (instance->package_root / fs::path(resource_name.ToStdString()))
                  .lexically_normal();
          std::error_code error;
          if (!fs::is_regular_file(resource, error))
            return fail(-6, "not-found", "scene icon resource was not found");
          primitive.resource_path = resource.string();
        } else if (kind == "text") {
          primitive.kind = OverlayPrimitiveKind::kText;
          primitive.text = primitive_value["value"].AsString().ToStdString();
          primitive.size_pixels =
              static_cast<float>(primitive_value["size_pixels"].AsDouble());
          if (primitive_value.HasMember("rotation_degrees"))
            primitive.rotation_degrees = static_cast<float>(
                primitive_value["rotation_degrees"].AsDouble());
          if (primitive_value.HasMember("horizontal_alignment"))
            primitive.horizontal_alignment =
                primitive_value["horizontal_alignment"]
                    .AsString()
                    .ToStdString();
          if (!ParseOverlayPoint(primitive_value["position"],
                                 &primitive.centre) ||
              !ParseOverlayColor(primitive_value["color"],
                                 &primitive.text_color) ||
              primitive.text.empty() || primitive.text.size() > 4096 ||
              !std::isfinite(primitive.size_pixels) ||
              !std::isfinite(primitive.rotation_degrees) ||
              primitive.rotation_degrees < -3600.0F ||
              primitive.rotation_degrees > 3600.0F ||
              (primitive.horizontal_alignment != "left" &&
               primitive.horizontal_alignment != "centre" &&
               primitive.horizontal_alignment != "right") ||
              primitive.size_pixels < 6.0F || primitive.size_pixels > 128.0F) {
            return fail(-5, "invalid-text", "text primitive is invalid");
          }
        } else {
          return fail(-5, "invalid-primitive",
                      "scene primitive kind is invalid");
        }
        layer.primitives.push_back(std::move(primitive));
      }
      update.layers.push_back(std::move(layer));
    }

    {
      std::lock_guard<std::mutex> lock(instance->state_mutex);
      const auto existing = instance->scenes.find(update.scene_id);
      if (existing != instance->scenes.end() &&
          update.revision <= existing->second.revision) {
        return fail(-8, "stale-revision", "scene update revision is not newer");
      }
      if (!request["replace"].AsBool() && existing != instance->scenes.end()) {
        OverlayScene merged = existing->second;
        merged.revision = update.revision;
        merged.canvas_target = update.canvas_target;
        merged.selected_canvases = update.selected_canvases;
        merged.render_phase = update.render_phase;
        for (auto& incoming_layer : update.layers) {
          auto layer =
              std::find_if(merged.layers.begin(), merged.layers.end(),
                           [&](const OverlayLayer& value) {
                             return value.layer_id == incoming_layer.layer_id;
                           });
          if (layer == merged.layers.end()) {
            merged.layers.push_back(std::move(incoming_layer));
            continue;
          }
          layer->z_index = incoming_layer.z_index;
          layer->visible = incoming_layer.visible;
          for (auto& incoming_primitive : incoming_layer.primitives) {
            auto primitive = std::find_if(
                layer->primitives.begin(), layer->primitives.end(),
                [&](const OverlayPrimitive& value) {
                  return value.primitive_id == incoming_primitive.primitive_id;
                });
            if (primitive == layer->primitives.end())
              layer->primitives.push_back(std::move(incoming_primitive));
            else
              *primitive = std::move(incoming_primitive);
          }
        }
        std::size_t merged_primitive_count = 0;
        std::size_t merged_point_count = 0;
        if (merged.layers.size() > kSceneLayerLimit)
          return fail(-7, "scene-too-large",
                      "merged scene exceeds the layer limit");
        for (const auto& layer : merged.layers) {
          if (merged_primitive_count >
              kScenePrimitiveLimit - layer.primitives.size())
            return fail(-7, "scene-too-large",
                        "merged scene exceeds the primitive limit");
          merged_primitive_count += layer.primitives.size();
          for (const auto& primitive : layer.primitives) {
            if (merged_point_count > kScenePointLimit - primitive.points.size())
              return fail(-7, "scene-too-large",
                          "merged scene exceeds the point limit");
            merged_point_count += primitive.points.size();
          }
        }
        instance->scenes[update.scene_id] = std::move(merged);
      } else {
        instance->scenes[update.scene_id] = std::move(update);
      }
    }
    instance->owner->PublishStateChanged();
    return finish(0, wxJSONValue(wxJSONTYPE_OBJECT));
  }

  if (operation_name.rfind("storage.", 0) == 0) {
    if (!instance->owner->Permitted(*instance, "storage.private"))
      return fail(-4, "permission-denied",
                  "private storage permission was not granted");
    const std::string name = request["name"].AsString().ToStdString();
    if (!IsSafeName(name))
      return fail(-5, "invalid-name", "private storage name is invalid");
    const fs::path target = instance->private_root / name;

    if (operation_name == "storage.read") {
      bool okay = false;
      const std::string contents =
          ReadSmallFile(target, kPrivateReadLimit, &okay);
      if (!okay)
        return fail(-6, "not-found", "private storage entry was not found");
      wxJSONValue response;
      response["base64"] = wxBase64Encode(contents.data(), contents.size());
      return finish(0, response);
    }
    if (operation_name == "storage.write-atomic") {
      if (!request["base64"].IsString())
        return fail(-5, "invalid-value", "private storage value is missing");
      const wxMemoryBuffer decoded = wxBase64Decode(
          request["base64"].AsString(), wxBase64DecodeMode_Strict);
      if (decoded.GetDataLen() > kPrivateReadLimit)
        return fail(-7, "value-too-large",
                    "private storage value exceeds 8 MiB");
      std::array<unsigned char, 8> random{};
      if (RAND_bytes(random.data(), random.size()) != 1)
        return fail(-8, "host-failure",
                    "could not create an atomic storage transaction");
      static constexpr char kHex[] = "0123456789abcdef";
      std::string suffix;
      suffix.reserve(random.size() * 2);
      for (const unsigned char byte : random) {
        suffix.push_back(kHex[byte >> 4]);
        suffix.push_back(kHex[byte & 0x0f]);
      }
      const fs::path temporary =
          instance->private_root / (".write-" + suffix + ".tmp");
      {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
          return fail(-8, "host-failure",
                      "could not open the atomic storage transaction");
        output.write(static_cast<const char*>(decoded.GetData()),
                     static_cast<std::streamsize>(decoded.GetDataLen()));
        output.flush();
        if (!output) {
          std::error_code ignored;
          fs::remove(temporary, ignored);
          return fail(-8, "host-failure",
                      "could not write the private storage entry");
        }
      }
      const wxString temporary_name = wxString::FromUTF8(temporary.string());
      const wxString target_name = wxString::FromUTF8(target.string());
      if (!wxRenameFile(temporary_name, target_name, true)) {
        wxRemoveFile(temporary_name);
        return fail(-8, "host-failure",
                    "could not publish the private storage entry atomically");
      }
      return finish(0, wxJSONValue(wxJSONTYPE_OBJECT));
    }
    if (operation_name == "storage.delete") {
      std::error_code error;
      const bool deleted = fs::remove(target, error);
      if (error)
        return fail(-8, "host-failure",
                    "could not delete the private storage entry");
      wxJSONValue response;
      response["deleted"] = deleted;
      return finish(0, response);
    }
    if (operation_name == "storage.list") {
      const std::string prefix = request["prefix"].AsString().ToStdString();
      if ((!prefix.empty() && !IsSafeName(prefix)) || prefix.size() > 128)
        return fail(-5, "invalid-prefix", "private storage prefix is invalid");
      wxJSONValue response(wxJSONTYPE_ARRAY);
      std::error_code error;
      std::vector<std::string> names;
      for (fs::directory_iterator item(instance->private_root, error), end;
           !error && item != end && names.size() < 4096;
           item.increment(error)) {
        if (!item->is_regular_file(error)) continue;
        const std::string candidate = item->path().filename().string();
        if (!IsSafeName(candidate) || candidate.rfind(prefix, 0) != 0) continue;
        names.push_back(candidate);
      }
      if (error)
        return fail(-8, "host-failure", "could not enumerate private storage");
      std::sort(names.begin(), names.end());
      for (const auto& name_value : names)
        response.Append(wxString::FromUTF8(name_value));
      return finish(0, response);
    }
    return fail(-9, "unsupported-operation",
                "unknown private storage operation");
  }

  if (operation_name == "events.subscribe") {
    CapabilityEventKind kind;
    const std::string kind_name = request["kind"].AsString().ToStdString();
    if (!ParseCapabilityEventKind(kind_name, &kind))
      return fail(-5, "invalid-event", "event kind is invalid");
    const char* permission = CapabilityEventPermission(kind);
    if (!permission || (*permission != '\0' &&
                        !instance->owner->Permitted(*instance, permission)))
      return fail(-4, "permission-denied",
                  "event subscription permission was not granted");
    CapabilityEventSubscription subscription;
    subscription.package_id = instance->id;
    subscription.kind = kind;
    subscription.topic_prefix =
        request["topic_prefix"].AsString().ToStdString();
    subscription.queue_limit = static_cast<std::size_t>(
        std::max<long>(1, request["queue_limit"].AsLong()));
    std::string diagnostic;
    std::uint64_t id = 0;
    if (!instance->owner->events.Subscribe(subscription, &diagnostic, &id))
      return fail(-6, "subscription-rejected", diagnostic);
    wxJSONValue response;
    response["subscription_id"] = static_cast<wxLongLong_t>(id);
    return finish(0, response);
  }
  if (operation_name == "events.unsubscribe") {
    const std::uint64_t id =
        static_cast<std::uint64_t>(request["subscription_id"].AsLong());
    if (id == 0 || !instance->owner->events.Unsubscribe(instance->id, id))
      return fail(-6, "not-found", "event subscription was not found");
    return finish(0, wxJSONValue(wxJSONTYPE_OBJECT));
  }

  if (operation_name == "timers.schedule" ||
      operation_name == "timers.cancel") {
    if (!instance->owner->Permitted(*instance, "timers.schedule"))
      return fail(-4, "permission-denied",
                  "timer scheduling permission was not granted");
    const std::string timer_id = request["timer_id"].AsString().ToStdString();
    if (!IsSafeName(timer_id))
      return fail(-5, "invalid-timer", "timer identifier is invalid");
    const TimerKey key{instance->id, timer_id};
    if (operation_name == "timers.cancel") {
      bool cancelled = false;
      {
        std::lock_guard<std::mutex> lock(instance->owner->timer_mutex);
        cancelled = instance->owner->timers.erase(key) != 0;
      }
      instance->owner->timer_changed.notify_all();
      wxJSONValue response;
      response["cancelled"] = cancelled;
      return finish(0, response);
    }

    const long delay_value = request["delay_milliseconds"].AsLong();
    const bool repeating = !request["repeat_milliseconds"].IsNull();
    const long repeat_value =
        repeating ? request["repeat_milliseconds"].AsLong() : 0;
    if (delay_value < static_cast<long>(kMinimumTimerMilliseconds) ||
        delay_value > static_cast<long>(kMaximumTimerMilliseconds) ||
        (repeating &&
         (repeat_value < static_cast<long>(kMinimumTimerMilliseconds) ||
          repeat_value > static_cast<long>(kMaximumTimerMilliseconds)))) {
      return fail(-5, "invalid-timer",
                  "timer interval is outside the 50 ms to 24 hour policy");
    }
    {
      std::lock_guard<std::mutex> lock(instance->owner->timer_mutex);
      std::size_t owned = 0;
      for (const auto& [candidate, ignored] : instance->owner->timers) {
        if (candidate.package_id == instance->id) ++owned;
      }
      if (instance->owner->timers.count(key) == 0 &&
          owned >= kTimerLimitPerPackage) {
        return fail(-7, "resource-limit",
                    "package already owns the maximum number of timers");
      }
      TimerRegistration timer;
      timer.instance = instance;
      timer.generation = instance->executor.Generation();
      timer.due = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(delay_value);
      timer.scheduled_unix_milliseconds = UnixMillisecondsNow() + delay_value;
      timer.repeat_milliseconds =
          repeating ? static_cast<std::uint32_t>(repeat_value) : 0;
      instance->owner->timers[key] = timer;
    }
    instance->owner->timer_changed.notify_all();
    return finish(0, wxJSONValue(wxJSONTYPE_OBJECT));
  }

  if (operation_name.rfind("rpc.", 0) == 0) {
    const bool registration =
        operation_name == "rpc.register" || operation_name == "rpc.unregister";
    if (registration &&
        !instance->owner->Permitted(*instance, "plugin.rpc.provide"))
      return fail(-4, "permission-denied",
                  "RPC provider permission was not granted");
    if (operation_name == "rpc.request" &&
        !instance->owner->Permitted(*instance, "plugin.rpc.request"))
      return fail(-4, "permission-denied",
                  "RPC request permission was not granted");
    if (operation_name == "rpc.respond" &&
        !instance->owner->Permitted(*instance, "plugin.rpc.provide"))
      return fail(-4, "permission-denied",
                  "RPC provider permission was not granted");

    if (registration) {
      const std::string service = request["service"].AsString().ToStdString();
      if (!IsSafeName(service))
        return fail(-5, "invalid-service", "RPC service identifier is invalid");
      std::lock_guard<std::mutex> lock(instance->owner->rpc_mutex);
      if (operation_name == "rpc.unregister") {
        const auto found = instance->owner->rpc_services.find(service);
        if (found == instance->owner->rpc_services.end() ||
            found->second != instance->id) {
          return fail(-6, "not-found", "RPC service is not registered");
        }
        instance->owner->rpc_services.erase(found);
        return finish(0, wxJSONValue(wxJSONTYPE_OBJECT));
      }
      std::size_t owned = 0;
      for (const auto& [ignored, package] : instance->owner->rpc_services) {
        if (package == instance->id) ++owned;
      }
      const auto found = instance->owner->rpc_services.find(service);
      if (found != instance->owner->rpc_services.end() &&
          found->second != instance->id)
        return fail(-8, "already-registered",
                    "RPC service is owned by another package");
      if (found == instance->owner->rpc_services.end() &&
          owned >= kRpcServiceLimitPerPackage)
        return fail(-7, "resource-limit",
                    "package already provides the maximum number of services");
      instance->owner->rpc_services[service] = instance->id;
      return finish(0, wxJSONValue(wxJSONTYPE_OBJECT));
    }

    const std::string target_package =
        request["target_package"].AsString().ToStdString();
    if (!IsPackageId(wxString::FromUTF8(target_package)))
      return fail(-5, "invalid-target", "RPC target package is invalid");
    Instance* target = instance->owner->Find(target_package);
    if (!target || !target->enabled || target->failed ||
        (target->portable_api != OCPN_PORTABLE_API_V03 &&
         target->portable_api != OCPN_PORTABLE_API_V04))
      return fail(-6, "target-unavailable",
                  "RPC target package is not enabled for API 0.3", true);

    if (operation_name == "rpc.request") {
      wxJSONValue rpc = request["request"];
      const std::string correlation_id =
          rpc["correlation_id"].AsString().ToStdString();
      const std::string service = rpc["service"].AsString().ToStdString();
      const std::string method = rpc["method"].AsString().ToStdString();
      const std::string content_type =
          rpc["content_type"].AsString().ToStdString();
      const std::string payload =
          rpc["payload_base64"].AsString().ToStdString();
      const long timeout = rpc["timeout_milliseconds"].AsLong();
      if (!IsSafeName(correlation_id) || !IsSafeName(service) ||
          !IsSafeName(method) || content_type.empty() ||
          content_type.size() > 128 ||
          payload.size() > (kRpcPayloadLimit * 4 / 3 + 8) ||
          timeout < static_cast<long>(kRpcMinimumTimeoutMilliseconds) ||
          timeout > static_cast<long>(kRpcMaximumTimeoutMilliseconds)) {
        return fail(-5, "invalid-request", "RPC request is invalid");
      }
      const RpcCallKey call_key{instance->id, correlation_id};
      {
        std::lock_guard<std::mutex> lock(instance->owner->rpc_mutex);
        const auto service_owner = instance->owner->rpc_services.find(service);
        if (service_owner == instance->owner->rpc_services.end() ||
            service_owner->second != target_package)
          return fail(-6, "service-unavailable",
                      "RPC target does not provide the requested service",
                      true);
        std::size_t outstanding = 0;
        for (const auto& [key, ignored] : instance->owner->rpc_calls) {
          if (key.source_package == instance->id) ++outstanding;
        }
        if (instance->owner->rpc_calls.count(call_key) != 0)
          return fail(-8, "duplicate-correlation",
                      "RPC correlation identifier is already outstanding");
        if (outstanding >= kRpcOutstandingLimitPerPackage)
          return fail(-7, "resource-limit",
                      "package already has too many outstanding RPC calls");
        instance->owner->rpc_calls.emplace(
            call_key, RpcCall{instance, instance->executor.Generation(),
                              target_package, service,
                              std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeout)});
      }
      const std::string encoded = JsonText(rpc);
      const std::uint64_t generation = target->executor.Generation();
      const auto posted = target->executor.Post(
          generation,
          [target, source = instance->id, encoded](std::uint64_t token) {
            if (!target->enabled || target->failed ||
                target->executor.Generation() != token)
              return;
            std::lock_guard<std::mutex> lock(target->runtime_mutex);
            if (!target->runtime) return;
            std::array<char, kErrorCapacity> error{};
            if (ocpn_portable_runtime_on_rpc_request(
                    target->runtime, source.data(), source.size(),
                    encoded.data(), encoded.size(), error.data(),
                    error.size()) != 0) {
              wxLogWarning(
                  "PPM RPC request handler failed target=%s diagnostic=%s",
                  target->id, error.data());
            }
          });
      if (posted != SerialExecutor::PostResult::kAccepted) {
        std::lock_guard<std::mutex> lock(instance->owner->rpc_mutex);
        instance->owner->rpc_calls.erase(call_key);
        return fail(-10, "target-busy", "RPC target queue is unavailable",
                    true);
      }
      instance->owner->timer_changed.notify_all();
      return finish(0, wxJSONValue(wxJSONTYPE_OBJECT));
    }

    wxJSONValue rpc = request["response"];
    const std::string correlation_id =
        rpc["correlation_id"].AsString().ToStdString();
    const long status = rpc["status"].AsLong();
    const std::string content_type =
        rpc["content_type"].AsString().ToStdString();
    const std::string payload = rpc["payload_base64"].AsString().ToStdString();
    const std::string response_diagnostic =
        rpc["diagnostic"].AsString().ToStdString();
    if (!IsSafeName(correlation_id) || status < 0 || status > 65'535 ||
        content_type.empty() || content_type.size() > 128 ||
        payload.size() > (kRpcPayloadLimit * 4 / 3 + 8) ||
        response_diagnostic.size() > kErrorCapacity)
      return fail(-5, "invalid-response",
                  "RPC response is invalid or exceeds host policy");
    const RpcCallKey call_key{target_package, correlation_id};
    {
      std::lock_guard<std::mutex> lock(instance->owner->rpc_mutex);
      const auto call = instance->owner->rpc_calls.find(call_key);
      if (call == instance->owner->rpc_calls.end() ||
          call->second.target_package != instance->id)
        return fail(-6, "not-found",
                    "matching outstanding RPC request was not found");
      instance->owner->rpc_calls.erase(call);
    }
    const std::string encoded = JsonText(rpc);
    const std::uint64_t generation = target->executor.Generation();
    const auto posted = target->executor.Post(
        generation,
        [target, source = instance->id, encoded](std::uint64_t token) {
          if (!target->enabled || target->failed ||
              target->executor.Generation() != token)
            return;
          std::lock_guard<std::mutex> lock(target->runtime_mutex);
          if (!target->runtime) return;
          std::array<char, kErrorCapacity> error{};
          if (ocpn_portable_runtime_on_rpc_response(
                  target->runtime, source.data(), source.size(), encoded.data(),
                  encoded.size(), error.data(), error.size()) != 0) {
            wxLogWarning(
                "PPM RPC response handler failed target=%s diagnostic=%s",
                target->id, error.data());
          }
        });
    if (posted != SerialExecutor::PostResult::kAccepted)
      return fail(-10, "target-busy",
                  "RPC response target queue is unavailable", true);
    return finish(0, wxJSONValue(wxJSONTYPE_OBJECT));
  }

  if (operation_name == "https.request") {
    if (!instance->owner->Permitted(*instance, "network.https"))
      return fail(-4, "permission-denied",
                  "controlled HTTPS permission was not granted");
    if (instance->https_domains.empty())
      return fail(-4, "domain-denied", "the package declares no HTTPS domains");
    const std::string request_id =
        request["request_id"].AsString().ToStdString();
    const std::string method = request["method"].AsString().ToStdString();
    const std::string body_base64 =
        request["body_base64"].AsString().ToStdString();
    const wxMemoryBuffer decoded = wxBase64Decode(
        wxString::FromUTF8(body_base64), wxBase64DecodeMode_Strict);
    if (!IsSafeName(request_id) ||
        (!body_base64.empty() && decoded.GetDataLen() == 0) ||
        !request["headers"].IsArray() || request["headers"].Size() > 64)
      return fail(-5, "invalid-request",
                  "HTTPS request identity, body or headers are invalid");
    HttpsRequest https_request;
    https_request.method = method;
    https_request.url = request["url"].AsString().ToStdString();
    https_request.timeout_milliseconds = static_cast<std::uint32_t>(
        std::max<long>(0, request["timeout_milliseconds"].AsLong()));
    https_request.maximum_response_bytes = static_cast<std::size_t>(
        std::max<long>(0, request["maximum_response_bytes"].AsLong()));
    const auto* body = static_cast<const std::uint8_t*>(decoded.GetData());
    if (body && decoded.GetDataLen() != 0)
      https_request.body.assign(body, body + decoded.GetDataLen());
    for (int index = 0; index < request["headers"].Size(); ++index) {
      wxJSONValue header = request["headers"][index];
      if (!header.IsObject() || !header["name"].IsString() ||
          !header["value"].IsString())
        return fail(-5, "invalid-request",
                    "HTTPS request contains a malformed header");
      https_request.headers.push_back(
          {header["name"].AsString().ToStdString(),
           header["value"].AsString().ToStdString()});
    }
    const auto now = std::chrono::steady_clock::now();
    while (!instance->https_history.empty() &&
           now - instance->https_history.front() > std::chrono::minutes(1))
      instance->https_history.pop_front();
    if (instance->https_history.size() >= 30)
      return fail(-9, "rate-limited",
                  "package exceeded the controlled HTTPS request rate", true);
    instance->https_history.push_back(now);
    HttpsResponse https_response;
    std::string https_diagnostic;
    if (!PerformControlledHttps(https_request, instance->https_domains,
                                &https_response, &https_diagnostic))
      return fail(-10, "https-failed", https_diagnostic, true);
    wxJSONValue response;
    response["status"] = https_response.status;
    response["headers"] = wxJSONValue(wxJSONTYPE_ARRAY);
    for (const auto& [name, value] : https_response.headers) {
      wxJSONValue header;
      header["name"] = wxString::FromUTF8(name);
      header["value"] = wxString::FromUTF8(value);
      response["headers"].Append(header);
    }
    response["body_base64"] =
        wxBase64Encode(https_response.body.data(), https_response.body.size());
    response["final_url"] = wxString::FromUTF8(https_response.final_url);
    return finish(0, response);
  }

  if (operation_name == "surfaces.open") {
    if (!instance->owner->Permitted(*instance, "ui.surfaces"))
      return fail(-4, "permission-denied",
                  "surface permission was not granted");
    const std::string surface_id =
        request["surface_id"].AsString().ToStdString();
    const std::string role = request["role"].AsString().ToStdString();
    static const std::set<std::string> roles{"tool-window",    "preferences",
                                             "options-page",   "modal-task",
                                             "dockable-panel", "inspector"};
    if (!IsSafeName(surface_id) || roles.count(role) == 0)
      return fail(-5, "invalid-surface",
                  "surface identifier or role is invalid");
    const int status = OpenNamedSurface(instance, surface_id, role);
    if (status != 0)
      return fail(status, status == -2 ? "not-found" : "surface-unavailable",
                  status == -2 ? "the declared surface was not found"
                               : "the surface could not be opened");
    return finish(0, wxJSONValue(wxJSONTYPE_OBJECT));
  }

  const bool action_operation = operation_name.rfind("actions.", 0) == 0;
  const bool navigation_read = operation_name == "navigation.list" ||
                               operation_name == "navigation.list-page" ||
                               operation_name == "navigation.get";
  const bool navigation_write = operation_name == "navigation.mutate";
  const bool navigation_output = operation_name == "navigation.send-nmea0183" ||
                                 operation_name == "navigation.send-nmea2000";
  const bool host_environment = operation_name == "host-environment.get";
  const bool communication_operation =
      operation_name == "communications.list-outputs";
  if (action_operation && !instance->owner->Permitted(*instance, "ui.commands"))
    return fail(-4, "permission-denied",
                "command registration permission was not granted");
  if (navigation_read &&
      !instance->owner->Permitted(*instance, "navigation.objects.read"))
    return fail(-4, "permission-denied",
                "navigation object read permission was not granted");
  if (navigation_write &&
      !instance->owner->Permitted(*instance, "navigation.objects.write") &&
      !instance->owner->Permitted(*instance, "navigation.routes.write"))
    return fail(-4, "permission-denied",
                "navigation object write permission was not granted");
  if (operation_name == "navigation.send-nmea0183" &&
      !instance->owner->Permitted(*instance, "navigation.nmea.write"))
    return fail(-4, "permission-denied",
                "NMEA output permission was not granted");
  if (operation_name == "navigation.send-nmea2000" &&
      !instance->owner->Permitted(*instance, "navigation.nmea2000.write"))
    return fail(-4, "permission-denied",
                "NMEA 2000 output permission was not granted");
  if (communication_operation &&
      !instance->owner->Permitted(*instance, "communications.outputs.read"))
    return fail(-4, "permission-denied",
                "communication output discovery permission was not granted");
  if (action_operation || navigation_read || navigation_write ||
      navigation_output || host_environment || communication_operation) {
    if (!instance->owner->author_ui_request)
      return fail(-10, "service-unavailable",
                  "the OpenCPN UI service is unavailable");
    auto status = std::make_shared<std::int32_t>(-10);
    auto response = std::make_shared<std::string>();
    const bool completed = instance->owner->RunUiService(
        [owner = instance->owner, package_id = instance->id, operation_name,
         request_text, status, response]() {
          *status = owner->author_ui_request(package_id, operation_name,
                                             request_text, response.get());
        },
        std::chrono::seconds(30));
    if (!completed)
      return fail(-11, "timeout",
                  "OpenCPN did not complete the requested UI operation", true);
    wxJSONValue value;
    if (response->empty()) {
      value = wxJSONValue(wxJSONTYPE_OBJECT);
    } else {
      wxJSONReader reader;
      if (reader.Parse(wxString::FromUTF8(*response), &value) != 0)
        return fail(-12, "invalid-host-response",
                    "OpenCPN returned invalid author-service JSON");
    }
    const std::int32_t host_status = *status;
    if (host_status == 0 && operation_name == "actions.unregister") {
      const std::string action_id =
          request["action_id"].AsString().ToStdString();
      instance->registered_actions.erase(
          std::remove_if(instance->registered_actions.begin(),
                         instance->registered_actions.end(),
                         [&](const RuntimeAction& action) {
                           return action.action_id == action_id;
                         }),
          instance->registered_actions.end());
    }
    return finish(host_status, value);
  }

  return fail(-9, "unsupported-operation",
              "the requested API 0.3 author service is not implemented");
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
    callbacks.send_plugin_message = SendPluginMessage;
    callbacks.charts_query_final_safety = ChartsQueryFinalSafety;
    callbacks.author_service_call = AuthorServiceCall;
    instance.runtime = ocpn_portable_runtime_create(
        instance.component_path.c_str(), &callbacks, instance.portable_api,
        instance.portable_world, error.data(), error.size());
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
      wxLogError("PPM package-initialize-failed id=%s diagnostic=%s",
                 instance.id, instance.diagnostic);
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
    wxLogError("PPM package-enable-failed id=%s diagnostic=%s", instance.id,
               instance.diagnostic);
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
  CancelPackageTimers(instance.id);
  ClearPackageRpc(instance.id);
  events.ClearPending(instance.id);
  instance.event_pump_scheduled = false;
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
  const wxString portable_api = manifest["portable_api"].AsString();
  const bool portable_api_v01 = portable_api == ">=0.1.0 <0.2.0";
  const bool portable_api_v02 = portable_api == ">=0.2.0 <0.3.0";
  const bool portable_api_v03 = portable_api == ">=0.3.0 <0.4.0";
  const bool portable_api_v04 = portable_api == ">=0.4.0 <0.5.0";
  const wxString portable_world = manifest["portable_world"].IsString()
                                      ? manifest["portable_world"].AsString()
                                      : "plugin";
  const bool supported_world =
      portable_world == "plugin" ||
      portable_world == "weather-routing-plugin" ||
      portable_world == "passage-weather-routing-plugin";
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
      (!portable_api_v01 && !portable_api_v02 && !portable_api_v03 &&
       !portable_api_v04) ||
      !supported_world || (portable_api_v01 && portable_world != "plugin") ||
      ((portable_api_v02 || portable_api_v03 || portable_api_v04) &&
       !manifest["portable_world"].IsString()) ||
      ((portable_api_v03 || portable_api_v04) && portable_world != "plugin") ||
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
  if (manifest["https_domains"].IsArray()) {
    for (int index = 0; index < manifest["https_domains"].Size(); ++index) {
      const std::string domain =
          manifest["https_domains"][index].AsString().ToStdString();
      if (!IsValidHttpsDomain(domain)) {
        if (diagnostic) *diagnostic = "installed HTTPS domain is invalid";
        return false;
      }
      instance->https_domains.insert(domain);
    }
  }
  instance->portable_api =
      portable_api_v04 ? OCPN_PORTABLE_API_V04
      : portable_api_v03
          ? OCPN_PORTABLE_API_V03
          : (portable_api_v02 ? OCPN_PORTABLE_API_V02 : OCPN_PORTABLE_API_V01);
  instance->portable_world = portable_world == "passage-weather-routing-plugin"
                                 ? OCPN_PORTABLE_WORLD_PASSAGE_ROUTING
                             : portable_world == "weather-routing-plugin"
                                 ? OCPN_PORTABLE_WORLD_WEATHER_ROUTING
                                 : OCPN_PORTABLE_WORLD_PLUGIN;
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
  const bool declares_event_subscriptions =
      manifest.HasMember("event_subscriptions");
  if (!instance->failed && declares_event_subscriptions) {
    const wxJSONValue subscriptions = manifest["event_subscriptions"];
    if (!subscriptions.IsArray() || subscriptions.Size() > 32) {
      instance->failed = true;
      instance->diagnostic =
          "manifest event subscriptions are invalid or exceed policy";
    } else {
      for (int index = 0; index < subscriptions.Size(); ++index) {
        const wxJSONValue declared = subscriptions.ItemAt(index);
        const wxJSONValue event_value = declared.ItemAt("event");
        const wxJSONValue prefix_value = declared.ItemAt("topic_prefix");
        const wxJSONValue queue_value = declared.ItemAt("queue_limit");
        CapabilityEventKind kind;
        const std::string event_name =
            event_value.IsString() ? event_value.AsString().ToStdString()
                                   : std::string();
        const std::string prefix =
            !declared.HasMember("topic_prefix")
                ? std::string()
                : (prefix_value.IsString()
                       ? prefix_value.AsString().ToStdString()
                       : std::string(
                             CapabilityEventBroker::kMaximumTopicBytes + 1,
                             'x'));
        const std::size_t queue_limit =
            !declared.HasMember("queue_limit")
                ? 32
                : (queue_value.IsInt() && queue_value.AsInt() > 0
                       ? static_cast<std::size_t>(queue_value.AsInt())
                       : 0);
        CapabilityEventSubscription subscription{
            instance->id, CapabilityEventKind::kNmea0183, prefix, queue_limit};
        std::string subscription_error;
        if (!declared.IsObject() ||
            !ParseCapabilityEventKind(event_name, &kind) ||
            instance->requested_permissions.count(
                CapabilityEventPermission(kind)) == 0) {
          instance->failed = true;
          instance->diagnostic =
              "manifest event subscription is invalid or lacks its "
              "corresponding permission";
          break;
        }
        subscription.kind = kind;
        if (!events.Subscribe(subscription, &subscription_error)) {
          instance->failed = true;
          instance->diagnostic =
              "manifest event subscription is invalid: " + subscription_error;
          break;
        }
      }
    }
  }
  if (!instance->failed && !declares_event_subscriptions &&
      instance->requested_permissions.count("navigation.nmea.read") != 0) {
    std::string subscription_error;
    if (!events.Subscribe(
            {instance->id, CapabilityEventKind::kNmea0183, "", 64},
            &subscription_error)) {
      instance->failed = true;
      instance->diagnostic =
          "legacy navigation subscription failed: " + subscription_error;
    }
  }
  const std::set<std::string> known_services = {
      "org.opencpn.environment.provider",
      "org.opencpn.vessel-performance.editor"};
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
    if (!LoadRoot(root, developer_mode, false, &diagnostic)) {
      okay = false;
      wxLogWarning("PPM package-static-load-failed root=%s diagnostic=%s",
                   root.string(), diagnostic);
    }
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
    events.RemovePackage(package_id);
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
  CancelPackageTimers(instance.id);
  ClearPackageRpc(instance.id);
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
                                       const std::string& action_id,
                                       RuntimeActionContext context) {
  const auto item = std::find_if(
      instances.begin(), instances.end(),
      [&](const auto& candidate) { return candidate->id == package_id; });
  if (item == instances.end()) return false;
  Instance& instance = **item;
  if (!instance.enabled || instance.failed || !instance.runtime) return true;
  const std::uint64_t generation = instance.executor.Generation();
  const auto posted = instance.executor.Post(
      generation, [&instance, action_id, context = std::move(context)](
                      std::uint64_t task_generation) {
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
        int status = 0;
        if (instance.portable_api == OCPN_PORTABLE_API_V04) {
          const std::map<std::string, std::uint32_t> locations{
              {"toolbar", 0},
              {"chart-context-menu", 1},
              {"ais-context-menu", 2},
              {"route-context-menu", 3},
              {"waypoint-context-menu", 4},
              {"track-context-menu", 5}};
          const auto location = locations.find(context.location);
          if (location == locations.end()) {
            instance.owner->Fail(instance, "action " + action_id,
                                 "invalid OPP action context location");
            return;
          }
          ocpn_portable_action_context invocation{};
          invocation.location = location->second;
          invocation.canvas_index = context.canvas_index;
          invocation.has_canvas_index = context.has_canvas_index ? 1U : 0U;
          invocation.latitude = context.latitude;
          invocation.longitude = context.longitude;
          invocation.has_position = context.has_position ? 1U : 0U;
          invocation.object_kind = context.object_kind.data();
          invocation.object_kind_len = context.object_kind.size();
          invocation.object_id = context.object_id.data();
          invocation.object_id_len = context.object_id.size();
          status = ocpn_portable_runtime_on_action_v04(
              instance.runtime, action_id.data(), action_id.size(), &invocation,
              error.data(), error.size());
        } else {
          status = ocpn_portable_runtime_on_action(
              instance.runtime, action_id.data(), action_id.size(),
              error.data(), error.size());
        }
        if (status != 0) {
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
  if (!instance->executor.WaitIdle(elapsed >= timeout
                                       ? std::chrono::milliseconds(0)
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
  const std::size_t topic_end = sentence.find_first_of(",*");
  const std::string topic =
      sentence.substr(0, std::min(topic_end, sentence.size()));
  events.Publish({CapabilityEventKind::kNmea0183, topic, sentence});
  for (auto& item : instances) {
    ScheduleEvents(*item);
  }
}

void RuntimeEngine::Impl::DeliverPluginMessage(
    const std::string& message_id, const std::string& message_body) {
  std::string diagnostic;
  if (message_id.empty() ||
      !events.Publish(
          {CapabilityEventKind::kPluginMessage, message_id, message_body},
          &diagnostic)) {
    if (!diagnostic.empty())
      wxLogWarning("PPM plugin-message-rejected diagnostic=%s", diagnostic);
    return;
  }
  for (auto& item : instances) ScheduleEvents(*item);
}

void RuntimeEngine::Impl::ScheduleEvents(Instance& instance) {
  if (!instance.enabled || instance.failed || !instance.runtime ||
      events.Pending(instance.id) == 0) {
    return;
  }
  bool expected = false;
  if (!instance.event_pump_scheduled.compare_exchange_strong(expected, true))
    return;
  const std::uint64_t generation = instance.executor.Generation();
  const auto posted = instance.executor.Post(
      generation, [this, &instance](std::uint64_t task_generation) {
        const auto finish = [this, &instance]() {
          instance.event_pump_scheduled = false;
          if (instance.enabled && !instance.failed &&
              events.Pending(instance.id) != 0) {
            ScheduleEvents(instance);
          }
        };
        if (task_generation != instance.executor.Generation() ||
            !instance.enabled || instance.failed) {
          finish();
          return;
        }
        const auto pending = events.Drain(instance.id, 64);
        std::lock_guard<std::mutex> lock(instance.runtime_mutex);
        for (const auto& event : pending) {
          if (!instance.runtime ||
              task_generation != instance.executor.Generation() ||
              !instance.enabled || instance.failed) {
            break;
          }
          std::array<char, kErrorCapacity> error{};
          const std::string operation = std::string("capability event ") +
                                        CapabilityEventName(event.kind);
          const int result = ocpn_portable_runtime_on_event(
              instance.runtime, static_cast<std::uint32_t>(event.kind),
              event.topic.data(), event.topic.size(), event.payload.data(),
              event.payload.size(), event.sequence, error.data(), error.size());
          if (result != 0) {
            Fail(instance, operation,
                 error[0] ? error.data() : "portable event handler failed");
            break;
          }
        }
        finish();
      });
  if (posted != SerialExecutor::PostResult::kAccepted)
    instance.event_pump_scheduled = false;
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

void RuntimeEngine::SetPluginMessageSender(PluginMessageSender callback) {
  impl_->SetPluginMessageSender(std::move(callback));
}

void RuntimeEngine::SetAuthorUiRequestCallback(AuthorUiRequest callback) {
  impl_->SetAuthorUiRequestCallback(std::move(callback));
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
                                 const std::string& action_id,
                                 RuntimeActionContext context) {
  return impl_->HandleAction(package_id, action_id, std::move(context));
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

bool RuntimeEngine::CalculateRouteBlocking(const std::string& package_id,
                                           RoutingRequest request,
                                           RoutingOutcome* outcome,
                                           std::string* diagnostic) {
  return impl_->CalculateRouteBlocking(package_id, std::move(request), outcome,
                                       diagnostic);
}

bool RuntimeEngine::CalculatePassageBlocking(const std::string& package_id,
                                             RoutingPassageRequest request,
                                             RoutingOutcome* outcome,
                                             std::string* diagnostic) {
  return impl_->CalculatePassageBlocking(package_id, std::move(request),
                                         outcome, diagnostic);
}

bool RuntimeEngine::BeginRouteAttempt(const std::string& package_id,
                                      std::string* diagnostic) {
  return impl_->BeginRouteAttempt(package_id, diagnostic);
}

bool RuntimeEngine::PreflightEnvironment(
    const std::string& package_id, double latitude, double longitude,
    const std::vector<std::int64_t>& unix_times,
    std::vector<std::uint8_t>* availability, std::string* diagnostic) {
  return impl_->PreflightEnvironment(package_id, latitude, longitude,
                                     unix_times, availability, diagnostic);
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
  const bool valid = std::isfinite(fix.Lat) && std::isfinite(fix.Lon) &&
                     fix.Lat >= -90.0 && fix.Lat <= 90.0 && fix.Lon >= -180.0 &&
                     fix.Lon <= 180.0;
  {
    std::lock_guard<std::mutex> lock(impl_->position_mutex);
    impl_->position_valid = valid;
    impl_->latitude = fix.Lat;
    impl_->longitude = fix.Lon;
    impl_->has_cog = std::isfinite(fix.Cog);
    impl_->has_sog = std::isfinite(fix.Sog);
    impl_->cog = impl_->has_cog ? fix.Cog : 0.0;
    impl_->sog = impl_->has_sog ? fix.Sog : 0.0;
    impl_->has_heading_true = std::isfinite(fix.Hdt);
    impl_->heading_true = impl_->has_heading_true ? fix.Hdt : 0.0;
    impl_->has_heading_magnetic = std::isfinite(fix.Hdm);
    impl_->heading_magnetic = impl_->has_heading_magnetic ? fix.Hdm : 0.0;
    impl_->has_magnetic_variation = std::isfinite(fix.Var);
    impl_->magnetic_variation = impl_->has_magnetic_variation ? fix.Var : 0.0;
    impl_->fix_unix_time = static_cast<std::int64_t>(fix.FixTime);
    impl_->satellites =
        static_cast<std::uint16_t>(std::clamp(fix.nSats, 0, 65'535));
  }
  if (valid) {
    wxJSONValue payload;
    payload["latitude"] = fix.Lat;
    payload["longitude"] = fix.Lon;
    if (std::isfinite(fix.Cog))
      payload["course_over_ground"] = fix.Cog;
    else
      payload["course_over_ground"] = wxJSONValue(wxJSONTYPE_NULL);
    if (std::isfinite(fix.Sog))
      payload["speed_over_ground"] = fix.Sog;
    else
      payload["speed_over_ground"] = wxJSONValue(wxJSONTYPE_NULL);
    payload["fix_unix_time"] = static_cast<wxLongLong_t>(fix.FixTime);
    payload["satellites"] = fix.nSats;
    payload["heading_true"] = std::isfinite(fix.Hdt)
                                  ? wxJSONValue(fix.Hdt)
                                  : wxJSONValue(wxJSONTYPE_NULL);
    payload["heading_magnetic"] = std::isfinite(fix.Hdm)
                                      ? wxJSONValue(fix.Hdm)
                                      : wxJSONValue(wxJSONTYPE_NULL);
    payload["magnetic_variation"] = std::isfinite(fix.Var)
                                        ? wxJSONValue(fix.Var)
                                        : wxJSONValue(wxJSONTYPE_NULL);
    impl_->PublishCapabilityEvent({CapabilityEventKind::kNavigationPosition,
                                   "vessel", JsonText(payload)});
  }
}

void RuntimeEngine::SetCursorPosition(double latitude, double longitude) {
  if (!std::isfinite(latitude) || !std::isfinite(longitude) ||
      std::abs(latitude) > 90.0 || std::abs(longitude) > 180.0)
    return;
  wxJSONValue payload;
  payload["latitude"] = latitude;
  payload["longitude"] = longitude;
  impl_->PublishCapabilityEvent(
      {CapabilityEventKind::kCursor, "chart", JsonText(payload)});
}

void RuntimeEngine::SetViewport(double west, double south, double east,
                                double north, double scale_ppm, double rotation,
                                int canvas_index) {
  if (!std::isfinite(west) || !std::isfinite(south) || !std::isfinite(east) ||
      !std::isfinite(north) || !std::isfinite(scale_ppm) ||
      !std::isfinite(rotation) || west >= east || south >= north)
    return;
  wxJSONValue payload;
  payload["west"] = west;
  payload["south"] = south;
  payload["east"] = east;
  payload["north"] = north;
  payload["scale_pixels_per_metre"] = scale_ppm;
  payload["rotation_radians"] = rotation;
  payload["canvas_index"] = canvas_index;
  impl_->PublishCapabilityEvent({CapabilityEventKind::kViewport,
                                 std::to_string(canvas_index),
                                 JsonText(payload)});
}

void RuntimeEngine::SetActiveLeg(double cross_track_error_nm,
                                 double bearing_degrees, double distance_nm,
                                 const std::string& waypoint_name,
                                 bool arrival) {
  if (!std::isfinite(cross_track_error_nm) || !std::isfinite(bearing_degrees) ||
      !std::isfinite(distance_nm))
    return;
  wxJSONValue payload;
  payload["cross_track_error_nm"] = cross_track_error_nm;
  payload["bearing_degrees_true"] = bearing_degrees;
  payload["distance_nm"] = distance_nm;
  payload["waypoint_name"] = wxString::FromUTF8(waypoint_name);
  payload["arrival"] = arrival;
  impl_->PublishCapabilityEvent(
      {CapabilityEventKind::kActiveLeg, waypoint_name, JsonText(payload)});
}

void RuntimeEngine::DeliverNavigationSentence(const std::string& sentence) {
  impl_->DeliverNavigationSentence(sentence);
}

void RuntimeEngine::DeliverNmea2000(std::uint32_t pgn,
                                    const std::string& source,
                                    const std::vector<std::uint8_t>& payload) {
  if (pgn == 0 || payload.empty() || payload.size() > 2048 ||
      source.size() > 256)
    return;
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(payload.size() * 2);
  for (const std::uint8_t byte : payload) {
    encoded.push_back(kHex[byte >> 4]);
    encoded.push_back(kHex[byte & 0x0f]);
  }
  wxJSONValue value;
  value["pgn"] = static_cast<long>(pgn);
  value["source"] = wxString::FromUTF8(source);
  value["payload_hex"] = wxString::FromUTF8(encoded);
  impl_->PublishCapabilityEvent({CapabilityEventKind::kNmea2000,
                                 "pgn/" + std::to_string(pgn),
                                 JsonText(value)});
}

void RuntimeEngine::DeliverAisSentence(const std::string& sentence) {
  if (sentence.empty() || sentence.size() > 1024) return;
  const std::size_t topic_end = sentence.find_first_of(",*");
  impl_->PublishCapabilityEvent(
      {CapabilityEventKind::kAisTarget,
       sentence.substr(0, std::min(topic_end, sentence.size())), sentence});
}

void RuntimeEngine::DeliverSignalK(const std::string& payload) {
  if (payload.empty() || payload.size() > 64 * 1024) return;
  impl_->PublishCapabilityEvent(
      {CapabilityEventKind::kSignalK, "OCPN_CORE_SIGNALK", payload});
}

void RuntimeEngine::DeliverHostEnvironment(const std::string& payload) {
  if (payload.empty() || payload.size() > 64 * 1024) return;
  impl_->PublishCapabilityEvent(
      {CapabilityEventKind::kHostEnvironment, "host", payload});
}

bool RuntimeEngine::DeliverPointerEvent(
    std::uint32_t kind, std::uint32_t button, std::uint32_t canvas_index,
    std::int32_t x_pixels, std::int32_t y_pixels, double latitude,
    double longitude, bool has_position, std::int32_t wheel_rotation,
    std::uint32_t modifiers, const std::string& hit_package_id,
    const std::string& hit_scene_id, const std::string& hit_primitive_id) {
  for (auto& item : impl_->instances) {
    auto& instance = *item;
    if (!instance.enabled || instance.failed || !instance.runtime ||
        (instance.portable_api != OCPN_PORTABLE_API_V03 &&
         instance.portable_api != OCPN_PORTABLE_API_V04) ||
        !impl_->Permitted(instance, "chart.input.pointer"))
      continue;
    std::lock_guard<std::mutex> lock(instance.runtime_mutex);
    std::array<char, kErrorCapacity> error{};
    std::uint8_t handled = 0;
    const bool owns_hit = hit_package_id == instance.id;
    const int status = ocpn_portable_runtime_on_pointer_event(
        instance.runtime, kind, button, canvas_index, x_pixels, y_pixels,
        latitude, longitude, has_position ? 1U : 0U, wheel_rotation, modifiers,
        owns_hit ? hit_scene_id.data() : nullptr,
        owns_hit ? hit_scene_id.size() : 0,
        owns_hit ? hit_primitive_id.data() : nullptr,
        owns_hit ? hit_primitive_id.size() : 0, &handled, error.data(),
        error.size());
    if (status != 0) {
      impl_->Fail(instance, "pointer event",
                  error[0] ? error.data() : "portable pointer event failed");
    } else if (handled != 0) {
      return true;
    }
  }
  return false;
}

bool RuntimeEngine::DeliverKeyEvent(std::uint32_t key_code,
                                    std::uint32_t unicode, bool has_unicode,
                                    bool pressed, bool repeat,
                                    std::uint32_t modifiers) {
  for (auto& item : impl_->instances) {
    auto& instance = *item;
    if (!instance.enabled || instance.failed || !instance.runtime ||
        (instance.portable_api != OCPN_PORTABLE_API_V03 &&
         instance.portable_api != OCPN_PORTABLE_API_V04) ||
        !impl_->Permitted(instance, "chart.input.keyboard"))
      continue;
    std::lock_guard<std::mutex> lock(instance.runtime_mutex);
    std::array<char, kErrorCapacity> error{};
    std::uint8_t handled = 0;
    const int status = ocpn_portable_runtime_on_key_event(
        instance.runtime, key_code, unicode, has_unicode ? 1U : 0U,
        pressed ? 1U : 0U, repeat ? 1U : 0U, modifiers, &handled, error.data(),
        error.size());
    if (status != 0) {
      impl_->Fail(instance, "keyboard event",
                  error[0] ? error.data() : "portable keyboard event failed");
    } else if (handled != 0) {
      return true;
    }
  }
  return false;
}

void RuntimeEngine::DeliverPluginMessage(const std::string& message_id,
                                         const std::string& message_body) {
  impl_->DeliverPluginMessage(message_id, message_body);
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
