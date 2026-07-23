#include "weather_routing_host.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include <wx/button.h>
#include <wx/app.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/datectrl.h>
#include <wx/dateevt.h>
#include <wx/datetime.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/fileconf.h>
#include <wx/filepicker.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/gauge.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/splitter.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/wfstream.h>
#include <wx/txtstrm.h>

#include "ocpn_plugin.h"
#include "portable_departure_time.h"
#include "portable_polar.h"
#include "portable_ui_menu.h"

#if defined(__WXOSX__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

namespace {
constexpr size_t kMaximumRoutePoints = 20'000;

class TimeCtrl final : public wxTextCtrl {
public:
  TimeCtrl(wxWindow* parent, wxWindowID id,
           const wxDateTime& value = wxDefaultDateTime,
           const wxPoint& position = wxDefaultPosition,
           const wxSize& size = wxDefaultSize)
      : wxTextCtrl(parent, id,
                   value.IsValid() ? value.Format("%H:%M") : "00:00",
                   position, size, wxTE_PROCESS_ENTER) {
    Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
      const wxDateTime parsed = ParsedValue();
      if (parsed.IsValid()) ChangeValue(parsed.Format("%H:%M"));
      event.Skip();
    });
  }

  void SetValue(const wxDateTime& value) {
    ChangeValue(value.IsValid() ? value.Format("%H:%M") : "00:00");
  }

  bool GetTime(int* hour, int* minute, int* second) const {
    const wxDateTime parsed = ParsedValue();
    if (!parsed.IsValid() || !hour || !minute || !second) return false;
    *hour = parsed.GetHour();
    *minute = parsed.GetMinute();
    *second = parsed.GetSecond();
    return true;
  }

  wxDateTime ParsedValue() const {
    wxDateTime result;
    wxString::const_iterator end;
    const wxString value = GetValue();
    if (!result.ParseTime(value, &end) || end != value.end())
      return wxInvalidDateTime;
    return result;
  }
};

enum PositionSource {
  kVesselPosition = 0,
  kOpenCpnWaypoint = 1,
  kChartCursor = 2,
  kManualCoordinates = 3,
};
void AddRow(wxFlexGridSizer* grid, wxWindow* parent, const wxString& label,
            wxWindow* control) {
  grid->Add(new wxStaticText(parent, wxID_ANY, label), 0,
            wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  grid->Add(control, 1, wxEXPAND);
}
bool Number(wxTextCtrl* control, double* value) {
  return control && control->GetValue().ToDouble(value) &&
         std::isfinite(*value);
}
bool LoadSurface(const wxString& package_root, const wxString& surface_resource,
                 wxString* title, wxArrayString* tabs,
                 std::map<wxString, wxString>* labels,
                 std::vector<PortableUiMenuDefinition>* menus,
                 wxJSONValue* definition, wxString* error) {
  wxFileName relative(surface_resource);
  if (surface_resource.empty() || relative.IsAbsolute() ||
      surface_resource.Find("..") != wxNOT_FOUND) {
    if (error) *error = "portable routing surface path is unsafe";
    return false;
  }
  wxFileInputStream input(package_root + wxFILE_SEP_PATH + surface_resource);
  wxJSONValue value;
  wxJSONReader reader;
  if (!input.IsOk() || reader.Parse(input, &value) != 0 || !value.IsObject() ||
      value["schema"].AsString() != "org.opencpn.portable-ui/0.2" ||
      value["surface"].AsString() != "routing-workbench" ||
      !value["title"].IsString() || !value["tabs"].IsArray() ||
      value["tabs"].Size() != 4 || !value["controls"].IsArray() ||
      !value["menus"].IsArray() || !value["manager"].IsObject() ||
      !value["manager"]["positions"]["columns"].IsArray() ||
      !value["manager"]["routings"]["columns"].IsArray() ||
      !value["manager"]["actions"].IsArray()) {
    if (error) *error = "portable weather-routing UI schema is incompatible";
    return false;
  }
  tabs->clear();
  for (int index = 0; index < value["tabs"].Size(); ++index) {
    if (!value["tabs"][index].IsString() ||
        value["tabs"][index].AsString().empty()) {
      if (error) *error = "portable weather-routing UI has an invalid tab";
      return false;
    }
    tabs->Add(value["tabs"][index].AsString());
  }
  const std::set<wxString> required_controls = {"start-latitude",
                                                "start-source",
                                                "start-waypoint",
                                                "start-longitude",
                                                "destination-latitude",
                                                "destination-source",
                                                "destination-waypoint",
                                                "destination-longitude",
                                                "refresh-positions",
                                                "use-opencpn-route",
                                                "opencpn-route",
                                                "departure-utc",
                                                "vessel-performance-file",
                                                "vessel-performance-status",
                                                "environment-provider",
                                                "avoid-unsafe",
                                                "minimum-wind-angle",
                                                "maximum-wind-angle",
                                                "maximum-true-wind",
                                                "maximum-apparent-wind",
                                                "maximum-wave",
                                                "maximum-opposing-wind-current",
                                                "land-safety-margin",
                                                "use-currents",
                                                "require-current-data",
                                                "use-waves",
                                                "require-wave-data",
                                                "maximum-latitude",
                                                "upwind-efficiency",
                                                "downwind-efficiency",
                                                "tack-penalty",
                                                "gybe-penalty",
                                                "allow-motor-sailing",
                                                "allow-motor",
                                                "motor-threshold",
                                                "motor-speed",
                                                "motor-sailing-boost",
                                                "motor-hysteresis",
                                                "minimum-motor-run",
                                                "mode-change-penalty",
                                                "maximum-motor-hours",
                                                "fuel-consumption",
                                                "maximum-fuel",
                                                "time-step",
                                                "heading-step",
                                                "adaptive-headings",
                                                "refined-heading-step",
                                                "spatial-cell",
                                                "labels-per-cell",
                                                "maximum-search-angle",
                                                "destination-tolerance",
                                                "maximum-hours",
                                                "maximum-states",
                                                "compare-departures",
                                                "departure-window",
                                                "departure-spacing",
                                                "departure-workers",
                                                "route-metrics",
                                                "departure-results",
                                                "route-schedule",
                                                "validation-diagnostics",
                                                "show-isochrones",
                                                "show-stability-corridor",
                                                "show-route-wind",
                                                "route-to-cursor",
                                                "boat-at-grib-time",
                                                "export-gpx",
                                                "send-to-opencpn",
                                                "calculate",
                                                "cancel",
                                                "progress"};
  std::set<wxString> controls;
  labels->clear();
  for (int index = 0; index < value["controls"].Size(); ++index) {
    wxJSONValue control = value["controls"][index];
    const wxString id = control["id"].AsString();
    const wxString tab = control["tab"].AsString();
    if (!control.IsObject() || id.empty() ||
        control["type"].AsString().empty() ||
        control["label"].AsString().empty() ||
        tabs->Index(tab) == wxNOT_FOUND || !controls.insert(id).second) {
      if (error) *error = "portable weather-routing UI has an invalid control";
      return false;
    }
    if (control["type"].AsString() == "table") {
      wxJSONValue columns = control["columns"];
      const int expected = id == "departure-results" ? 21
                           : id == "route-schedule"   ? 9
                                                       : 1;
      if (!columns.IsArray() || columns.Size() != expected) {
        if (error)
          *error = "portable weather-routing table has incompatible columns: " +
                   id;
        return false;
      }
      for (int column = 0; column < columns.Size(); ++column) {
        if (!columns[column].IsString() || columns[column].AsString().empty()) {
          if (error)
            *error = "portable weather-routing table has an invalid column: " +
                     id;
          return false;
        }
      }
    }
    (*labels)[id] = control["label"].AsString();
  }
  for (const auto& required : required_controls) {
    if (!controls.count(required)) {
      if (error)
        *error =
            "portable weather-routing UI omits required control " + required;
      return false;
    }
  }
  if (!ParsePortableUiMenus(value, menus, error)) return false;
  *title = value["title"].AsString();
  *definition = value;
  return true;
}
struct RoutingOutcome {
  struct InspectionLine {
    int64_t unix_time = 0;
    std::vector<ocpn_portable_route_point> points;
  };
  std::vector<ocpn_portable_route_point> points;
  std::vector<ocpn_portable_route_environment_point> route_environment;
  std::vector<InspectionLine> isochrones;
  std::vector<InspectionLine> traces;
  wxString diagnostic;
  double distance_nautical_miles = 0.0;
  uint64_t duration_seconds = 0;
  uint32_t states_examined = 0;
  double average_speed_knots = 0.0;
  double maximum_speed_knots = 0.0;
  double average_sog_knots = 0.0;
  double maximum_sog_knots = 0.0;
  double average_wind_knots = 0.0;
  double maximum_wind_knots = 0.0;
  double average_current_knots = 0.0;
  double maximum_current_knots = 0.0;
  uint32_t tacks = 0;
  uint64_t motor_seconds = 0;
  double estimated_fuel_litres = 0.0;
  uint32_t propulsion_transitions = 0;
  uint8_t comfort_level = 0;
  bool current_metrics_available = false;
  bool fuel_metrics_available = false;
  int64_t departure_unix_time = 0;
};

struct DepartureResult {
  RoutingOutcome outcome;
  wxString error;
  int64_t requested_departure_unix_time = 0;
  bool success = false;
};

bool CalculateRouteLeg(
                       const PortableWeatherRoutingHost::CalculateRoute&
                           calculate_route,
                       ocpn_portable_route_request* request,
                       const PortablePolarSet& performance,
                       RoutingOutcome* outcome, wxString* failure) {
  if (!calculate_route || !request || !outcome) {
    if (failure) *failure = "Invalid portable route calculation request";
    return false;
  }
  ppm::RoutingRequest portable_request;
  portable_request.parameters = *request;
  portable_request.parameters.polars = nullptr;
  portable_request.parameters.polar_count = 0;
  portable_request.polars.reserve(performance.grids.size());
  for (const auto& grid : performance.grids) {
    portable_request.polars.push_back(
        {grid.identity, grid.true_wind_speeds_knots,
         grid.true_wind_angles_degrees, grid.boat_speeds_knots});
  }
  ppm::RoutingOutcome result;
  std::string error;
  if (!calculate_route(std::move(portable_request), &result, &error)) {
    if (failure) *failure = wxString::FromUTF8(error);
    return false;
  }
  outcome->points = std::move(result.points);
  outcome->route_environment = std::move(result.route_environment);
  outcome->isochrones.reserve(result.isochrones.size());
  for (auto& line : result.isochrones)
    outcome->isochrones.push_back(
        {line.unix_time, std::move(line.points)});
  outcome->traces.reserve(result.traces.size());
  for (auto& line : result.traces)
    outcome->traces.push_back({line.unix_time, std::move(line.points)});
  outcome->diagnostic = wxString::FromUTF8(result.diagnostic);
  outcome->distance_nautical_miles = result.distance_nautical_miles;
  outcome->duration_seconds = result.duration_seconds;
  outcome->states_examined = result.states_examined;
  outcome->average_speed_knots = result.average_speed_knots;
  outcome->maximum_speed_knots = result.maximum_speed_knots;
  outcome->average_sog_knots = result.average_sog_knots;
  outcome->maximum_sog_knots = result.maximum_sog_knots;
  outcome->average_wind_knots = result.average_wind_knots;
  outcome->maximum_wind_knots = result.maximum_wind_knots;
  outcome->average_current_knots = result.average_current_knots;
  outcome->maximum_current_knots = result.maximum_current_knots;
  outcome->tacks = result.tacks;
  outcome->motor_seconds = result.motor_seconds;
  outcome->estimated_fuel_litres = result.estimated_fuel_litres;
  outcome->propulsion_transitions = result.propulsion_transitions;
  outcome->comfort_level = result.comfort_level;
  outcome->current_metrics_available = result.metrics_available & 1;
  outcome->fuel_metrics_available = result.metrics_available & 2;
  outcome->departure_unix_time = request->departure_unix_time;
  return true;
}

bool AppendRouteLeg(RoutingOutcome* passage, RoutingOutcome leg,
                    size_t leg_index, wxString* failure) {
  if (!passage || leg.points.size() < 2 ||
      leg.route_environment.size() != leg.points.size()) {
    if (failure) *failure = "Portable route leg returned inconsistent data";
    return false;
  }
  const size_t skip = passage->points.empty() ? 0 : 1;
  if (passage->points.size() + leg.points.size() - skip >
      kMaximumRoutePoints) {
    if (failure) *failure = "Combined multi-leg route exceeds the host limit";
    return false;
  }
  const double prior_seconds = static_cast<double>(passage->duration_seconds);
  const double leg_seconds = static_cast<double>(leg.duration_seconds);
  const double combined_seconds = prior_seconds + leg_seconds;
  auto weighted = [prior_seconds, leg_seconds, combined_seconds](double prior,
                                                                 double next) {
    return combined_seconds > 0.0
               ? (prior * prior_seconds + next * leg_seconds) /
                     combined_seconds
               : 0.0;
  };
  passage->average_speed_knots =
      weighted(passage->average_speed_knots, leg.average_speed_knots);
  passage->average_sog_knots =
      weighted(passage->average_sog_knots, leg.average_sog_knots);
  passage->average_wind_knots =
      weighted(passage->average_wind_knots, leg.average_wind_knots);
  if (leg.current_metrics_available) {
    passage->average_current_knots =
        passage->current_metrics_available
            ? weighted(passage->average_current_knots,
                       leg.average_current_knots)
            : leg.average_current_knots;
  }
  passage->maximum_speed_knots =
      std::max(passage->maximum_speed_knots, leg.maximum_speed_knots);
  passage->maximum_sog_knots =
      std::max(passage->maximum_sog_knots, leg.maximum_sog_knots);
  passage->maximum_wind_knots =
      std::max(passage->maximum_wind_knots, leg.maximum_wind_knots);
  passage->maximum_current_knots =
      std::max(passage->maximum_current_knots, leg.maximum_current_knots);
  passage->distance_nautical_miles += leg.distance_nautical_miles;
  passage->duration_seconds += leg.duration_seconds;
  passage->states_examined += leg.states_examined;
  passage->tacks += leg.tacks;
  passage->motor_seconds += leg.motor_seconds;
  passage->estimated_fuel_litres += leg.estimated_fuel_litres;
  passage->propulsion_transitions += leg.propulsion_transitions;
  passage->comfort_level = std::max(passage->comfort_level, leg.comfort_level);
  passage->current_metrics_available =
      passage->current_metrics_available || leg.current_metrics_available;
  passage->fuel_metrics_available =
      passage->fuel_metrics_available || leg.fuel_metrics_available;
  if (passage->departure_unix_time == 0)
    passage->departure_unix_time = leg.departure_unix_time;
  if (!passage->diagnostic.empty()) passage->diagnostic += "\n";
  passage->diagnostic += wxString::Format("Leg %zu: %s", leg_index + 1,
                                          leg.diagnostic);
  passage->points.insert(passage->points.end(), leg.points.begin() + skip,
                         leg.points.end());
  passage->route_environment.insert(passage->route_environment.end(),
                                    leg.route_environment.begin() + skip,
                                    leg.route_environment.end());
  passage->isochrones.insert(passage->isochrones.end(),
                             std::make_move_iterator(leg.isochrones.begin()),
                             std::make_move_iterator(leg.isochrones.end()));
  passage->traces.insert(passage->traces.end(),
                         std::make_move_iterator(leg.traces.begin()),
                         std::make_move_iterator(leg.traces.end()));
  return true;
}

wxString FormatElapsed(uint64_t seconds) {
  const uint64_t days = seconds / 86400;
  const uint64_t hours = (seconds % 86400) / 3600;
  const uint64_t minutes = (seconds % 3600) / 60;
  return days ? wxString::Format("%llud %02llu:%02llu",
                                 static_cast<unsigned long long>(days),
                                 static_cast<unsigned long long>(hours),
                                 static_cast<unsigned long long>(minutes))
              : wxString::Format("%02llu:%02llu",
                                 static_cast<unsigned long long>(hours),
                                 static_cast<unsigned long long>(minutes));
}

wxString FormatOffset(int64_t seconds) {
  const bool negative = seconds < 0;
  const uint64_t minutes =
      static_cast<uint64_t>(negative ? -(seconds / 60) : seconds / 60);
  return wxString::Format("%s%llu:%02llu", negative ? "-" : "+",
                          static_cast<unsigned long long>(minutes / 60),
                          static_cast<unsigned long long>(minutes % 60));
}

double InitialBearingDegrees(double latitude1, double longitude1,
                             double latitude2, double longitude2) {
  constexpr double radians = 3.14159265358979323846 / 180.0;
  const double phi1 = latitude1 * radians;
  const double phi2 = latitude2 * radians;
  const double lambda = (longitude2 - longitude1) * radians;
  const double y = std::sin(lambda) * std::cos(phi2);
  const double x = std::cos(phi1) * std::sin(phi2) -
                   std::sin(phi1) * std::cos(phi2) * std::cos(lambda);
  double bearing = std::atan2(y, x) / radians;
  if (bearing < 0.0) bearing += 360.0;
  return bearing;
}

double VectorBearingDegrees(double east, double north) {
  double bearing = std::atan2(east, north) * 180.0 /
                   3.14159265358979323846;
  if (bearing < 0.0) bearing += 360.0;
  return bearing;
}

double GreatCircleNauticalMiles(double latitude1, double longitude1,
                                double latitude2, double longitude2) {
  constexpr double radians = 3.14159265358979323846 / 180.0;
  constexpr double earth_radius_nm = 3440.065;
  const double dlat = (latitude2 - latitude1) * radians;
  const double dlon = (longitude2 - longitude1) * radians;
  const double phi1 = latitude1 * radians;
  const double phi2 = latitude2 * radians;
  const double a = std::sin(dlat / 2.0) * std::sin(dlat / 2.0) +
                   std::cos(phi1) * std::cos(phi2) *
                       std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
  return earth_radius_nm * 2.0 *
         std::atan2(std::sqrt(std::max(0.0, a)),
                    std::sqrt(std::max(0.0, 1.0 - a)));
}

bool InterpolateRoutePosition(
    const std::vector<ocpn_portable_route_point>& route, int64_t unix_time,
    double* latitude, double* longitude) {
  if (!latitude || !longitude || route.empty() ||
      unix_time < route.front().unix_time || unix_time > route.back().unix_time)
    return false;
  auto upper = std::lower_bound(
      route.begin(), route.end(), unix_time,
      [](const auto& point, int64_t time) { return point.unix_time < time; });
  if (upper == route.begin() || upper == route.end() ||
      upper->unix_time == unix_time) {
    const auto& point = upper == route.end() ? route.back() : *upper;
    *latitude = point.latitude;
    *longitude = point.longitude;
    return true;
  }
  const auto& before = *(upper - 1);
  const double fraction =
      static_cast<double>(unix_time - before.unix_time) /
      static_cast<double>(upper->unix_time - before.unix_time);
  *latitude = before.latitude + (upper->latitude - before.latitude) * fraction;
  const double delta =
      std::fmod(upper->longitude - before.longitude + 540.0, 360.0) - 180.0;
  *longitude =
      std::fmod(before.longitude + delta * fraction + 540.0, 360.0) - 180.0;
  return true;
}

wxPoint SampleRouteFraction(const std::vector<ocpn_portable_route_point>& route,
                            double fraction, PlugIn_ViewPort* viewport) {
  if (route.empty()) return wxPoint();
  const double scaled =
      fraction * static_cast<double>(route.size() > 1 ? route.size() - 1 : 0);
  const size_t lower = static_cast<size_t>(std::floor(scaled));
  const size_t upper = std::min(lower + 1, route.size() - 1);
  const double part = scaled - static_cast<double>(lower);
  const double latitude =
      route[lower].latitude + (route[upper].latitude - route[lower].latitude) * part;
  const double longitude_delta =
      std::fmod(route[upper].longitude - route[lower].longitude + 540.0, 360.0) -
      180.0;
  const double longitude =
      std::fmod(route[lower].longitude + longitude_delta * part + 540.0, 360.0) -
      180.0;
  wxPoint result;
  GetCanvasPixLL(viewport, &result, latitude, longitude);
  return result;
}

wxPoint Project(PlugIn_ViewPort* viewport, double latitude, double longitude) {
  wxPoint result;
  GetCanvasPixLL(viewport, &result, latitude, longitude);
  return result;
}

void DrawRouteWindBarb(wxDC& dc, const wxPoint& origin, double east_knots,
                       double north_knots, const wxColour& colour) {
  const double speed = std::hypot(east_knots, north_knots);
  if (!std::isfinite(speed) || speed < 0.1) return;
  dc.SetPen(wxPen(colour, 2, wxPENSTYLE_SOLID));
  dc.SetBrush(wxBrush(colour));
  if (speed < 2.5) {
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawCircle(origin, 3);
    return;
  }
  const double staff_x = -east_knots / speed;
  const double staff_y = north_knots / speed;
  const double perpendicular_x = -staff_y;
  const double perpendicular_y = staff_x;
  constexpr double length = 24.0;
  const wxPoint tip(
      origin.x + static_cast<int>(std::lround(staff_x * length)),
      origin.y + static_cast<int>(std::lround(staff_y * length)));
  dc.DrawLine(origin.x, origin.y, tip.x, tip.y);
  int remaining = static_cast<int>(std::floor((speed + 2.5) / 5.0)) * 5;
  double offset = 0.0;
  while (remaining >= 50) {
    const wxPoint first(
        tip.x - static_cast<int>(std::lround(staff_x * offset)),
        tip.y - static_cast<int>(std::lround(staff_y * offset)));
    const wxPoint second(
        tip.x - static_cast<int>(std::lround(staff_x * (offset + 5.0))),
        tip.y - static_cast<int>(std::lround(staff_y * (offset + 5.0))));
    wxPoint triangle[3] = {
        first, second,
        wxPoint(first.x + static_cast<int>(std::lround(perpendicular_x * 10.0 +
                                                       staff_x * 3.0)),
                first.y + static_cast<int>(std::lround(perpendicular_y * 10.0 +
                                                       staff_y * 3.0)))};
    dc.DrawPolygon(3, triangle);
    remaining -= 50;
    offset += 7.0;
  }
  while (remaining >= 10) {
    const wxPoint base(
        tip.x - static_cast<int>(std::lround(staff_x * offset)),
        tip.y - static_cast<int>(std::lround(staff_y * offset)));
    dc.DrawLine(base.x, base.y,
                base.x + static_cast<int>(std::lround(perpendicular_x * 10.0 +
                                                      staff_x * 3.0)),
                base.y + static_cast<int>(std::lround(perpendicular_y * 10.0 +
                                                      staff_y * 3.0)));
    remaining -= 10;
    offset += 5.0;
  }
  if (remaining >= 5) {
    const wxPoint base(
        tip.x - static_cast<int>(std::lround(staff_x * (offset + 1.5))),
        tip.y - static_cast<int>(std::lround(staff_y * (offset + 1.5))));
    dc.DrawLine(base.x, base.y,
                base.x + static_cast<int>(std::lround(perpendicular_x * 6.0 +
                                                      staff_x * 2.0)),
                base.y + static_cast<int>(std::lround(perpendicular_y * 6.0 +
                                                      staff_y * 2.0)));
  }
}
}  // namespace

class PortableWeatherRoutingHost::Impl {
public:
  Impl(wxWindow* parent_value, wxFileConfig* config_value,
       CalculateRoute route_calculator, std::function<void()> route_canceller,
       wxString package_root_value, wxString plugin_id_value,
       wxString surface_resource_value, std::function<wxString()> summary,
       std::function<std::vector<PortableNavigationPosition>()> waypoints,
       std::function<std::vector<PortableNavigationRoute>()> routes_value,
       std::function<bool(const wxString&,
                          const std::vector<PortableNavigationPosition>&,
                          wxString*)>
           route_creator,
       std::function<bool(PortableNavigationPosition*)> vessel,
       std::function<bool(PortableNavigationPosition*)> cursor,
       std::function<bool(int64_t*)> environment_time,
       std::function<bool(double, double, const std::vector<int64_t>&,
                          std::vector<uint8_t>*, wxString*)>
           environment_preflight,
       double latitude, double longitude)
      : parent(parent_value),
        config(config_value),
        calculate_route(std::move(route_calculator)),
        cancel_routes(std::move(route_canceller)),
        package_root(std::move(package_root_value)),
        plugin_id(std::move(plugin_id_value)),
        surface_resource(std::move(surface_resource_value)),
        dataset_summary(std::move(summary)),
        list_waypoints(std::move(waypoints)),
        list_routes(std::move(routes_value)),
        create_route(std::move(route_creator)),
        vessel_position(std::move(vessel)),
        cursor_position(std::move(cursor)),
        displayed_environment_time(std::move(environment_time)),
        preflight_environment(std::move(environment_preflight)),
        initial_latitude(latitude),
        initial_longitude(longitude) {}
  ~Impl() { Shutdown(); }
  bool Show(wxString* error);
  bool Render(wxDC& dc, PlugIn_ViewPort* viewport);
  bool RenderGL(PlugIn_ViewPort* viewport);
  void ReportProgress(unsigned percent, const wxString& message);
  bool Cancelled() const { return cancelled.load(); }
  void Shutdown();

private:
  void CreateFrame();
  void CreateEditor();
  void ShowEditor(size_t tab = 0);
  void PopulateManagerPositions();
  void PopulateManagerRouting(const wxString& state = "Ready");
  void DispatchSurfaceAction(const wxString& action);
  wxPanel* CreateRoutePanel(wxNotebook* book);
  wxPanel* CreateSafetyPanel(wxNotebook* book);
  wxPanel* CreateAdvancedPanel(wxNotebook* book);
  wxPanel* CreateResultsPanel(wxNotebook* book);
  void RefreshEnvironmentSummary();
  void RefreshNavigationPositions(bool initial = false);
  bool ApplyPositionSource(bool start, bool report_error = true);
  void UpdatePositionControls(bool start);
  void RelayoutRoutePanel();
  PortableDepartureZone SelectedDepartureZone() const;
  bool GetDepartureUnixTime(int64_t* unix_time, wxString* error) const;
  void SetDepartureUnixTime(int64_t unix_time);
  void UpdateDepartureSummary();
  wxString FormatRoutingTime(int64_t unix_time) const;
  void UpdateTimeColumnLabels();
  bool LoadVesselPerformance(const wxString& path, bool report_error = true);
  void LoadSettings();
  void SaveSettings();
  void Start();
  void Finish(std::vector<DepartureResult> results, int64_t nominal_departure);
  void PopulateDepartureResults();
  void SelectDepartureResult(size_t index);
  void ExportGpx();
  void SendToOpenCpn();
  const wxString& Label(const wxString& id) const {
    return surface_labels.at(id);
  }

  wxWindow* parent = nullptr;
  wxFileConfig* config = nullptr;
  CalculateRoute calculate_route;
  std::function<void()> cancel_routes;
  wxString package_root;
  wxString plugin_id;
  wxString surface_resource;
  wxString surface_title;
  wxString current_dataset_summary;
  wxArrayString surface_tabs;
  wxString configured_route_id;
  std::map<wxString, wxString> surface_labels;
  std::vector<PortableUiMenuDefinition> surface_menus;
  wxJSONValue surface_definition;
  bool surface_loaded = false;
  std::function<wxString()> dataset_summary;
  std::function<std::vector<PortableNavigationPosition>()> list_waypoints;
  std::function<std::vector<PortableNavigationRoute>()> list_routes;
  std::function<bool(const wxString&,
                     const std::vector<PortableNavigationPosition>&,
                     wxString*)>
      create_route;
  std::function<bool(PortableNavigationPosition*)> vessel_position;
  std::function<bool(PortableNavigationPosition*)> cursor_position;
  std::function<bool(int64_t*)> displayed_environment_time;
  std::function<bool(double, double, const std::vector<int64_t>&,
                     std::vector<uint8_t>*, wxString*)>
      preflight_environment;
  std::vector<PortableNavigationPosition> waypoints;
  std::vector<PortableNavigationRoute> navigation_routes;
  double initial_latitude = 0.0;
  double initial_longitude = 0.0;
  wxFrame* frame = nullptr;
  wxDialog* editor = nullptr;
  wxNotebook* notebook = nullptr;
  wxListCtrl *manager_positions = nullptr, *manager_routings = nullptr;
  wxButton *manager_compute = nullptr, *manager_edit = nullptr,
           *manager_export = nullptr, *manager_stop = nullptr;
  std::map<wxString, wxMenuItem*> surface_menu_items;
  wxScrolledWindow* route_panel = nullptr;
  wxTextCtrl *start_lat = nullptr, *start_lon = nullptr, *dest_lat = nullptr,
             *dest_lon = nullptr;
  wxChoice *start_source = nullptr, *start_waypoint = nullptr,
           *dest_source = nullptr, *dest_waypoint = nullptr,
           *route_choice = nullptr, *departure_timezone = nullptr;
  wxDatePickerCtrl* departure_date = nullptr;
  TimeCtrl* departure_time = nullptr;
  wxStaticText* departure_summary = nullptr;
  wxButton *departure_now = nullptr, *departure_grib_time = nullptr;
  std::vector<PortableDepartureZone> departure_time_zones;
  wxCheckBox* use_opencpn_route = nullptr;
  wxButton* refresh_positions = nullptr;
  wxFilePickerCtrl* vessel_performance_file = nullptr;
  wxSpinCtrl *time_step = nullptr, *heading_step = nullptr,
             *refined_heading_step = nullptr, *labels_per_cell = nullptr,
             *max_hours = nullptr, *max_states = nullptr,
             *departure_window = nullptr, *departure_spacing = nullptr,
             *departure_workers = nullptr, *min_wind_angle = nullptr,
             *max_wind_angle = nullptr, *maximum_latitude = nullptr,
             *upwind_efficiency = nullptr, *downwind_efficiency = nullptr,
             *tack_penalty = nullptr, *gybe_penalty = nullptr,
             *maximum_search_angle = nullptr;
  wxCheckBox *avoid_land = nullptr, *limit_true_wind = nullptr,
             *limit_apparent_wind = nullptr, *limit_waves = nullptr,
             *limit_opposing_wind_current = nullptr,
             *use_currents = nullptr, *require_current_data = nullptr,
             *use_waves = nullptr, *require_wave_data = nullptr,
             *compare_departures = nullptr, *adaptive_headings = nullptr,
             *allow_motor_sailing = nullptr, *allow_motor = nullptr,
             *limit_motor_hours = nullptr, *limit_fuel = nullptr;
  wxCheckBox *show_isochrones = nullptr, *show_stability_corridor = nullptr,
             *show_route_wind = nullptr, *route_to_cursor = nullptr,
             *boat_at_grib_time = nullptr;
  wxTextCtrl *max_true_wind = nullptr, *max_apparent_wind = nullptr,
             *max_wave = nullptr, *max_opposing_wind_current = nullptr,
             *land_safety_margin = nullptr, *destination_tolerance = nullptr,
             *spatial_cell = nullptr, *motor_threshold = nullptr,
             *motor_speed = nullptr, *motor_sailing_boost = nullptr,
             *motor_hysteresis = nullptr, *maximum_motor_hours = nullptr,
             *fuel_consumption = nullptr, *maximum_fuel = nullptr;
  wxSpinCtrl *minimum_motor_run = nullptr, *mode_change_penalty = nullptr;
  wxStaticText *provider = nullptr, *vessel_performance_status = nullptr,
               *status = nullptr, *metrics = nullptr;
  wxListCtrl *departure_results = nullptr, *route_schedule = nullptr;
  wxTextCtrl* validation_diagnostics = nullptr;
  wxGauge* gauge = nullptr;
  wxTimer environment_refresh_timer;
  wxButton *calculate = nullptr, *cancel = nullptr, *export_gpx = nullptr,
           *send_to_opencpn = nullptr;
  std::atomic<bool> cancelled{false};
  std::shared_ptr<std::atomic<bool>> alive =
      std::make_shared<std::atomic<bool>>(true);
  std::atomic<unsigned> departure_runs{1};
  std::atomic<unsigned> departures_completed{0};
  std::thread worker;
  std::vector<ocpn_portable_route_point> route;
  std::vector<ocpn_portable_route_environment_point> route_environment;
  std::vector<std::vector<ocpn_portable_route_point>> alternative_routes;
  std::vector<RoutingOutcome::InspectionLine> isochrones;
  std::vector<RoutingOutcome::InspectionLine> traces;
  std::vector<DepartureResult> departure_result_rows;
  size_t selected_departure_result = std::numeric_limits<size_t>::max();
  size_t best_departure_result = std::numeric_limits<size_t>::max();
  int64_t nominal_departure_unix_time = 0;
  bool updating_departure_selection = false;
  std::shared_ptr<const PortablePolarSet> vessel_performance;
  std::atomic<bool> stopped{false};
};

wxPanel* PortableWeatherRoutingHost::Impl::CreateRoutePanel(wxNotebook* book) {
  route_panel = new wxScrolledWindow(book, wxID_ANY, wxDefaultPosition,
                                     wxDefaultSize, wxVSCROLL);
  route_panel->SetScrollRate(0, route_panel->FromDIP(12));
  auto* panel = route_panel;
  auto* root = new wxBoxSizer(wxVERTICAL);
  auto* grid = new wxFlexGridSizer(2, 8, 8);
  grid->AddGrowableCol(1);
  start_lat = new wxTextCtrl(panel, wxID_ANY,
                             wxString::Format("%.6f", initial_latitude));
  start_lon = new wxTextCtrl(panel, wxID_ANY,
                             wxString::Format("%.6f", initial_longitude));
  dest_lat = new wxTextCtrl(panel, wxID_ANY,
                            wxString::Format("%.6f", initial_latitude + 1.0));
  dest_lon = new wxTextCtrl(panel, wxID_ANY,
                            wxString::Format("%.6f", initial_longitude + 1.5));
  auto* departure_editor = new wxPanel(panel);
  auto* departure_editor_root = new wxBoxSizer(wxVERTICAL);
  auto* departure_fields = new wxBoxSizer(wxHORIZONTAL);
  departure_date =
      new wxDatePickerCtrl(departure_editor, wxID_ANY, wxDefaultDateTime,
                           wxDefaultPosition, wxDefaultSize, wxDP_DEFAULT);
  departure_time =
      new TimeCtrl(departure_editor, wxID_ANY, wxDefaultDateTime,
                   wxDefaultPosition, departure_editor->FromDIP(wxSize(80, -1)));
  departure_timezone = new wxChoice(departure_editor, wxID_ANY);
  departure_time_zones.clear();
  auto add_zone = [this](const wxString& label,
                         const PortableDepartureZone& zone) {
    departure_timezone->Append(label);
    departure_time_zones.push_back(zone);
  };
  add_zone("UTC", {});
  add_zone("System local — " + PortableSystemTimeZoneName(),
           {PortableDepartureZoneKind::kSystemLocal, 0});
  const std::vector<int> fixed_offsets = {
      -720, -660, -600, -570, -540, -480, -420, -360, -300, -240,
      -210, -180, -120, -60,  60,   120,  180,  210,  240,  270,
      300,  330,  345,  360,  390,  420,  480,  525,  540,  570,
      600,  630,  660,  720,  765,  780,  840};
  for (const int offset : fixed_offsets) {
    const char sign = offset < 0 ? '-' : '+';
    const int absolute = std::abs(offset);
    add_zone(wxString::Format("Fixed UTC%c%02d:%02d", sign, absolute / 60,
                              absolute % 60),
             {PortableDepartureZoneKind::kFixedOffset, offset});
  }
  departure_timezone->SetSelection(0);
  departure_timezone->SetMinSize(
      departure_editor->FromDIP(wxSize(190, -1)));
  departure_date->SetToolTip("Departure calendar date in the selected timezone");
  departure_time->SetToolTip(
      "Departure clock time in 24-hour HH:MM format");
  departure_timezone->SetToolTip(
      "Timezone used for departure entry and route schedules");
  departure_fields->Add(departure_date, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                        6);
  departure_fields->Add(departure_time, 0,
                        wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  departure_fields->Add(departure_timezone, 1, wxALIGN_CENTER_VERTICAL);
  departure_editor_root->Add(departure_fields, 0, wxEXPAND);
  auto* departure_actions = new wxBoxSizer(wxHORIZONTAL);
  departure_now = new wxButton(departure_editor, wxID_ANY, "Now",
                               wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  departure_grib_time =
      new wxButton(departure_editor, wxID_ANY, "Displayed GRIB time");
  departure_actions->Add(departure_now, 0, wxRIGHT, 6);
  departure_actions->Add(departure_grib_time, 0);
  departure_editor_root->Add(departure_actions, 0, wxTOP, 6);
  departure_summary = new wxStaticText(departure_editor, wxID_ANY, wxEmptyString);
  departure_editor_root->Add(departure_summary, 0, wxEXPAND | wxTOP, 6);
  departure_editor->SetSizer(departure_editor_root);
  SetDepartureUnixTime(
      ((static_cast<int64_t>(wxDateTime::Now().GetTicks()) + 899) / 900) *
      900);
  vessel_performance_file = new wxFilePickerCtrl(
      panel, wxID_ANY, wxEmptyString, Label("vessel-performance-file"),
      "OpenCPN boat or polar (*.xml;*.pol)|*.xml;*.pol|All files|*",
      wxDefaultPosition, wxDefaultSize,
      wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
  vessel_performance_file->SetMinSize(
      wxSize(panel->FromDIP(360), wxDefaultCoord));
  const wxArrayString position_sources = {
      "Current vessel position", "OpenCPN waypoint",
      "Latest chart cursor position", "Manual coordinates"};
  start_source = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                              position_sources);
  start_waypoint = new wxChoice(panel, wxID_ANY);
  dest_source = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             position_sources);
  dest_waypoint = new wxChoice(panel, wxID_ANY);
  use_opencpn_route =
      new wxCheckBox(panel, wxID_ANY, Label("use-opencpn-route"));
  route_choice = new wxChoice(panel, wxID_ANY);
  route_choice->Enable(false);
  refresh_positions = new wxButton(panel, wxID_ANY, Label("refresh-positions"));
  AddRow(grid, panel, Label("start-source"), start_source);
  AddRow(grid, panel, Label("start-waypoint"), start_waypoint);
  AddRow(grid, panel, Label("start-latitude"), start_lat);
  AddRow(grid, panel, Label("start-longitude"), start_lon);
  AddRow(grid, panel, Label("destination-source"), dest_source);
  AddRow(grid, panel, Label("destination-waypoint"), dest_waypoint);
  AddRow(grid, panel, Label("destination-latitude"), dest_lat);
  AddRow(grid, panel, Label("destination-longitude"), dest_lon);
  AddRow(grid, panel, Label("opencpn-route"), route_choice);
  AddRow(grid, panel, Label("departure-utc"), departure_editor);
  AddRow(grid, panel, Label("vessel-performance-file"),
         vessel_performance_file);
  vessel_performance_status =
      new wxStaticText(panel, wxID_ANY, Label("vessel-performance-status"));
  current_dataset_summary = dataset_summary();
  provider = new wxStaticText(panel, wxID_ANY,
                              "Environment: " + current_dataset_summary);
  root->Add(grid, 0, wxEXPAND | wxALL, 12);
  root->Add(use_opencpn_route, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(refresh_positions, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(vessel_performance_status, 0,
            wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(new wxStaticLine(panel), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
  root->Add(provider, 0, wxEXPAND | wxALL, 12);
  auto* broker_note = new wxStaticText(
      panel, wxID_ANY,
      "The Wasm route engine consumes environmental values through iGRIB's "
      "typed host-brokered provider service.");
  broker_note->Wrap(panel->FromDIP(680));
  root->Add(broker_note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  panel->SetSizer(root);
  panel->FitInside();
  start_source->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    UpdatePositionControls(true);
    ApplyPositionSource(true);
  });
  dest_source->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    UpdatePositionControls(false);
    ApplyPositionSource(false);
  });
  start_waypoint->Bind(wxEVT_CHOICE,
                       [this](wxCommandEvent&) { ApplyPositionSource(true); });
  dest_waypoint->Bind(wxEVT_CHOICE,
                      [this](wxCommandEvent&) { ApplyPositionSource(false); });
  use_opencpn_route->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    const bool enabled = use_opencpn_route->GetValue();
    route_choice->Enable(enabled && !navigation_routes.empty());
    start_source->Enable(!enabled);
    start_waypoint->Enable(!enabled);
    dest_source->Enable(!enabled);
    dest_waypoint->Enable(!enabled);
    start_lat->Enable(!enabled);
    start_lon->Enable(!enabled);
    dest_lat->Enable(!enabled);
    dest_lon->Enable(!enabled);
    if (enabled && route_choice->GetSelection() != wxNOT_FOUND) {
      const auto& selected = navigation_routes[route_choice->GetSelection()];
      if (selected.points.size() >= 2) {
        start_lat->SetValue(wxString::Format("%.6f", selected.points.front().latitude));
        start_lon->SetValue(wxString::Format("%.6f", selected.points.front().longitude));
        dest_lat->SetValue(wxString::Format("%.6f", selected.points.back().latitude));
        dest_lon->SetValue(wxString::Format("%.6f", selected.points.back().longitude));
      }
    }
  });
  route_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    if (!use_opencpn_route->GetValue() ||
        route_choice->GetSelection() == wxNOT_FOUND)
      return;
    const auto& selected = navigation_routes[route_choice->GetSelection()];
    if (selected.points.size() < 2) return;
    start_lat->SetValue(wxString::Format("%.6f", selected.points.front().latitude));
    start_lon->SetValue(wxString::Format("%.6f", selected.points.front().longitude));
    dest_lat->SetValue(wxString::Format("%.6f", selected.points.back().latitude));
    dest_lon->SetValue(wxString::Format("%.6f", selected.points.back().longitude));
  });
  refresh_positions->Bind(
      wxEVT_BUTTON, [this](wxCommandEvent&) { RefreshNavigationPositions(); });
  vessel_performance_file->Bind(
      wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent&) {
        LoadVesselPerformance(vessel_performance_file->GetPath());
      });
  departure_date->Bind(wxEVT_DATE_CHANGED,
                       [this](wxDateEvent&) { UpdateDepartureSummary(); });
  departure_time->Bind(wxEVT_TIME_CHANGED,
                       [this](wxDateEvent&) { UpdateDepartureSummary(); });
  departure_timezone->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    UpdateDepartureSummary();
    UpdateTimeColumnLabels();
    PopulateDepartureResults();
    if (selected_departure_result != std::numeric_limits<size_t>::max())
      SelectDepartureResult(selected_departure_result);
    SaveSettings();
  });
  departure_now->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    SetDepartureUnixTime(
        ((static_cast<int64_t>(wxDateTime::Now().GetTicks()) + 59) / 60) * 60);
  });
  departure_grib_time->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    int64_t forecast_time = 0;
    if (displayed_environment_time &&
        displayed_environment_time(&forecast_time)) {
      SetDepartureUnixTime(forecast_time);
    } else if (status) {
      status->SetLabel("No displayed iGRIB forecast time is available");
    }
  });
  return panel;
}

void PortableWeatherRoutingHost::Impl::RelayoutRoutePanel() {
  if (!route_panel) return;
  const int wrap_width = std::max(
      route_panel->FromDIP(320),
      route_panel->GetClientSize().GetWidth() - route_panel->FromDIP(24));
  if (vessel_performance_status) vessel_performance_status->Wrap(wrap_width);
  if (provider) provider->Wrap(wrap_width);
  route_panel->Layout();
  route_panel->FitInside();
}

PortableDepartureZone
PortableWeatherRoutingHost::Impl::SelectedDepartureZone() const {
  if (!departure_timezone) return {};
  const int selection = departure_timezone->GetSelection();
  if (selection < 0 ||
      static_cast<size_t>(selection) >= departure_time_zones.size())
    return {};
  return departure_time_zones[static_cast<size_t>(selection)];
}

bool PortableWeatherRoutingHost::Impl::GetDepartureUnixTime(
    int64_t* unix_time, wxString* error) const {
  if (!departure_date || !departure_time) {
    if (error) *error = "Departure controls are unavailable";
    return false;
  }
  const wxDateTime clock = departure_time->ParsedValue();
  if (!clock.IsValid()) {
    if (error) *error = "Enter the departure time as HH:MM";
    return false;
  }
  return PortableDepartureToUnix(
      departure_date->GetValue(), clock.GetHour(), clock.GetMinute(),
      SelectedDepartureZone(), unix_time, error);
}

void PortableWeatherRoutingHost::Impl::SetDepartureUnixTime(
    int64_t unix_time) {
  if (!departure_date || !departure_time) return;
  const wxDateTime wall =
      PortableDepartureWallTime(unix_time, SelectedDepartureZone());
  departure_date->SetValue(wall);
  departure_time->SetValue(wall);
  UpdateDepartureSummary();
}

void PortableWeatherRoutingHost::Impl::UpdateDepartureSummary() {
  if (!departure_summary) return;
  int64_t unix_time = 0;
  wxString error;
  if (!GetDepartureUnixTime(&unix_time, &error)) {
    departure_summary->SetLabel(error);
    departure_summary->SetToolTip(error);
  } else {
    const PortableDepartureZone zone = SelectedDepartureZone();
    const wxString selected = FormatPortableDepartureTime(unix_time, zone);
    const wxString utc = FormatPortableDepartureTime(unix_time, {});
    departure_summary->SetLabel(
        zone.kind == PortableDepartureZoneKind::kUtc
            ? "Routing time: " + utc
            : "Routing time: " + selected + " = " + utc);
    departure_summary->SetToolTip(
        "The portable routing engine receives the unambiguous UTC value");
  }
  RelayoutRoutePanel();
}

wxString PortableWeatherRoutingHost::Impl::FormatRoutingTime(
    int64_t unix_time) const {
  return FormatPortableDepartureTime(unix_time, SelectedDepartureZone());
}

void PortableWeatherRoutingHost::Impl::UpdateTimeColumnLabels() {
  if (!route_schedule) return;
  const wxString zone = PortableDepartureZoneLabel(
      SelectedDepartureZone(), static_cast<int64_t>(wxDateTime::Now().GetTicks()));
  wxListItem column;
  column.SetMask(wxLIST_MASK_TEXT);
  column.SetText("Time (" + zone + ")");
  route_schedule->SetColumn(0, column);
}

bool PortableWeatherRoutingHost::Impl::LoadVesselPerformance(
    const wxString& path, bool report_error) {
  const wxScopedCharBuffer encoded = path.ToUTF8();
  PortablePolarSet loaded;
  std::string failure;
  if (path.empty() || !encoded.data() ||
      !LoadPortablePolarSet(std::filesystem::u8path(encoded.data()), &loaded,
                            &failure)) {
    vessel_performance.reset();
    if (vessel_performance_status)
      vessel_performance_status->SetLabel(
          "No usable vessel polar loaded" +
          (failure.empty() ? wxString() : ": " + wxString::FromUTF8(failure)));
    RelayoutRoutePanel();
    if (report_error && status)
      status->SetLabel("Vessel performance failed: " +
                       wxString::FromUTF8(failure));
    return false;
  }
  size_t cells = 0;
  for (const auto& grid : loaded.grids) cells += grid.boat_speeds_knots.size();
  vessel_performance =
      std::make_shared<const PortablePolarSet>(std::move(loaded));
  if (vessel_performance_status)
    vessel_performance_status->SetLabel(
        wxString::Format("Polar: %zu table(s), %zu performance cells — %s",
                         vessel_performance->grids.size(), cells,
                         wxFileName(path).GetFullName()));
  RelayoutRoutePanel();
  if (report_error && status)
    status->SetLabel("Vessel performance loaded and validated");
  return true;
}

void PortableWeatherRoutingHost::Impl::UpdatePositionControls(bool start) {
  wxChoice* source = start ? start_source : dest_source;
  wxChoice* waypoint = start ? start_waypoint : dest_waypoint;
  wxTextCtrl* latitude = start ? start_lat : dest_lat;
  wxTextCtrl* longitude = start ? start_lon : dest_lon;
  if (!source || !waypoint || !latitude || !longitude) return;
  const int selected = source->GetSelection();
  waypoint->Enable(selected == kOpenCpnWaypoint && !waypoints.empty());
  const bool manual = selected == kManualCoordinates;
  latitude->SetEditable(manual);
  longitude->SetEditable(manual);
}

bool PortableWeatherRoutingHost::Impl::ApplyPositionSource(bool start,
                                                           bool report_error) {
  wxChoice* source = start ? start_source : dest_source;
  wxChoice* waypoint = start ? start_waypoint : dest_waypoint;
  wxTextCtrl* latitude = start ? start_lat : dest_lat;
  wxTextCtrl* longitude = start ? start_lon : dest_lon;
  if (!source || !waypoint || !latitude || !longitude) return false;
  PortableNavigationPosition selected;
  bool available = false;
  switch (source->GetSelection()) {
    case kVesselPosition:
      available = vessel_position && vessel_position(&selected);
      break;
    case kOpenCpnWaypoint: {
      const int index = waypoint->GetSelection();
      if (index >= 0 && static_cast<size_t>(index) < waypoints.size()) {
        selected = waypoints[static_cast<size_t>(index)];
        available = true;
      }
      break;
    }
    case kChartCursor:
      available = cursor_position && cursor_position(&selected);
      break;
    case kManualCoordinates:
      return true;
    default:
      break;
  }
  if (!available) {
    if (report_error && status)
      status->SetLabel(start ? "The selected start position is unavailable"
                             : "The selected destination is unavailable");
    return false;
  }
  latitude->SetValue(wxString::Format("%.6f", selected.latitude));
  longitude->SetValue(wxString::Format("%.6f", selected.longitude));
  if (report_error && status)
    status->SetLabel(
        wxString::Format("%s: %s (%.5f, %.5f)", start ? "Start" : "Destination",
                         selected.name, selected.latitude, selected.longitude));
  return true;
}

void PortableWeatherRoutingHost::Impl::RefreshNavigationPositions(
    bool initial) {
  const wxString start_id =
      start_waypoint && start_waypoint->GetSelection() != wxNOT_FOUND &&
              static_cast<size_t>(start_waypoint->GetSelection()) <
                  waypoints.size()
          ? waypoints[static_cast<size_t>(start_waypoint->GetSelection())].id
          : wxString();
  const wxString destination_id =
      dest_waypoint && dest_waypoint->GetSelection() != wxNOT_FOUND &&
              static_cast<size_t>(dest_waypoint->GetSelection()) <
                  waypoints.size()
          ? waypoints[static_cast<size_t>(dest_waypoint->GetSelection())].id
          : wxString();
  const wxString route_id =
      route_choice && route_choice->GetSelection() != wxNOT_FOUND &&
              static_cast<size_t>(route_choice->GetSelection()) <
                  navigation_routes.size()
          ? navigation_routes[static_cast<size_t>(route_choice->GetSelection())]
                .id
          : configured_route_id;
  waypoints = list_waypoints ? list_waypoints()
                             : std::vector<PortableNavigationPosition>();
  navigation_routes = list_routes ? list_routes()
                                  : std::vector<PortableNavigationRoute>();
  start_waypoint->Clear();
  dest_waypoint->Clear();
  int restored_start = wxNOT_FOUND;
  int restored_destination = wxNOT_FOUND;
  for (size_t index = 0; index < waypoints.size(); ++index) {
    const auto& point = waypoints[index];
    const wxString display = wxString::Format("%s  —  %.5f, %.5f", point.name,
                                              point.latitude, point.longitude);
    start_waypoint->Append(display);
    dest_waypoint->Append(display);
    if (point.id == start_id) restored_start = static_cast<int>(index);
    if (point.id == destination_id)
      restored_destination = static_cast<int>(index);
  }
  route_choice->Clear();
  int restored_route = wxNOT_FOUND;
  for (size_t index = 0; index < navigation_routes.size(); ++index) {
    const auto& candidate = navigation_routes[index];
    route_choice->Append(wxString::Format("%s — %zu waypoints", candidate.name,
                                         candidate.points.size()));
    if (candidate.id == route_id) restored_route = static_cast<int>(index);
  }
  if (!navigation_routes.empty())
    route_choice->SetSelection(restored_route == wxNOT_FOUND ? 0
                                                             : restored_route);
  if (route_choice->GetSelection() != wxNOT_FOUND)
    configured_route_id =
        navigation_routes[static_cast<size_t>(route_choice->GetSelection())].id;
  route_choice->Enable(use_opencpn_route && use_opencpn_route->GetValue() &&
                       !navigation_routes.empty());
  if (!waypoints.empty()) {
    start_waypoint->SetSelection(
        restored_start == wxNOT_FOUND ? 0 : restored_start);
    dest_waypoint->SetSelection(
        restored_destination == wxNOT_FOUND
            ? static_cast<int>(waypoints.size() > 1 ? waypoints.size() - 1 : 0)
            : restored_destination);
  }
  if (initial) {
    PortableNavigationPosition current;
    if (vessel_position && vessel_position(&current))
      start_source->SetSelection(kVesselPosition);
    else if (!waypoints.empty())
      start_source->SetSelection(kOpenCpnWaypoint);
    else if (cursor_position && cursor_position(&current))
      start_source->SetSelection(kChartCursor);
    else
      start_source->SetSelection(kManualCoordinates);
    if (!waypoints.empty())
      dest_source->SetSelection(kOpenCpnWaypoint);
    else if (cursor_position && cursor_position(&current))
      dest_source->SetSelection(kChartCursor);
    else
      dest_source->SetSelection(kManualCoordinates);
  }
  UpdatePositionControls(true);
  UpdatePositionControls(false);
  ApplyPositionSource(true, false);
  ApplyPositionSource(false, false);
  if (!initial && status)
    status->SetLabel(wxString::Format(
        "Loaded %zu OpenCPN waypoint(s) and %zu route(s)", waypoints.size(),
        navigation_routes.size()));
  PopulateManagerPositions();
  if (manager_routings && manager_routings->GetItemCount() == 0)
    PopulateManagerRouting("Not computed");
}

wxPanel* PortableWeatherRoutingHost::Impl::CreateSafetyPanel(wxNotebook* book) {
  auto* panel = new wxScrolledWindow(book);
  panel->SetScrollRate(0, 12);
  auto* root = new wxBoxSizer(wxVERTICAL);
  avoid_land = new wxCheckBox(panel, wxID_ANY, Label("avoid-unsafe"));
  avoid_land->SetValue(true);
  min_wind_angle = new wxSpinCtrl(panel, wxID_ANY);
  min_wind_angle->SetRange(0, 180);
  min_wind_angle->SetValue(40);
  max_wind_angle = new wxSpinCtrl(panel, wxID_ANY);
  max_wind_angle->SetRange(0, 180);
  max_wind_angle->SetValue(180);
  limit_true_wind = new wxCheckBox(panel, wxID_ANY, Label("maximum-true-wind"));
  limit_true_wind->SetValue(true);
  max_true_wind = new wxTextCtrl(panel, wxID_ANY, "50");
  limit_apparent_wind =
      new wxCheckBox(panel, wxID_ANY, Label("maximum-apparent-wind"));
  limit_apparent_wind->SetValue(true);
  max_apparent_wind = new wxTextCtrl(panel, wxID_ANY, "50");
  limit_waves = new wxCheckBox(panel, wxID_ANY, Label("maximum-wave"));
  limit_waves->SetValue(true);
  max_wave = new wxTextCtrl(panel, wxID_ANY, "8.0");
  limit_opposing_wind_current = new wxCheckBox(
      panel, wxID_ANY, Label("maximum-opposing-wind-current"));
  limit_opposing_wind_current->SetValue(false);
  max_opposing_wind_current = new wxTextCtrl(panel, wxID_ANY, "0.0");
  max_opposing_wind_current->Enable(false);
  land_safety_margin = new wxTextCtrl(panel, wxID_ANY, "0.4");
  use_currents = new wxCheckBox(panel, wxID_ANY, Label("use-currents"));
  use_currents->SetValue(true);
  require_current_data =
      new wxCheckBox(panel, wxID_ANY, Label("require-current-data"));
  require_current_data->SetValue(false);
  use_waves = new wxCheckBox(panel, wxID_ANY, Label("use-waves"));
  use_waves->SetValue(true);
  require_wave_data =
      new wxCheckBox(panel, wxID_ANY, Label("require-wave-data"));
  require_wave_data->SetValue(true);
  maximum_latitude = new wxSpinCtrl(panel, wxID_ANY);
  maximum_latitude->SetRange(1, 90);
  maximum_latitude->SetValue(89);
  upwind_efficiency = new wxSpinCtrl(panel, wxID_ANY);
  upwind_efficiency->SetRange(10, 150);
  upwind_efficiency->SetValue(100);
  downwind_efficiency = new wxSpinCtrl(panel, wxID_ANY);
  downwind_efficiency->SetRange(10, 150);
  downwind_efficiency->SetValue(100);
  tack_penalty = new wxSpinCtrl(panel, wxID_ANY);
  tack_penalty->SetRange(0, 3600);
  tack_penalty->SetValue(300);
  gybe_penalty = new wxSpinCtrl(panel, wxID_ANY);
  gybe_penalty->SetRange(0, 3600);
  gybe_penalty->SetValue(300);
  allow_motor_sailing =
      new wxCheckBox(panel, wxID_ANY, Label("allow-motor-sailing"));
  allow_motor = new wxCheckBox(panel, wxID_ANY, Label("allow-motor"));
  motor_threshold = new wxTextCtrl(panel, wxID_ANY, "3.0");
  motor_speed = new wxTextCtrl(panel, wxID_ANY, "5.5");
  motor_sailing_boost = new wxTextCtrl(panel, wxID_ANY, "1.5");
  motor_hysteresis = new wxTextCtrl(panel, wxID_ANY, "0.2");
  minimum_motor_run = new wxSpinCtrl(panel, wxID_ANY);
  minimum_motor_run->SetRange(0, 86400);
  minimum_motor_run->SetValue(1800);
  mode_change_penalty = new wxSpinCtrl(panel, wxID_ANY);
  mode_change_penalty->SetRange(0, 3600);
  mode_change_penalty->SetValue(120);
  limit_motor_hours =
      new wxCheckBox(panel, wxID_ANY, Label("maximum-motor-hours"));
  maximum_motor_hours = new wxTextCtrl(panel, wxID_ANY, "24.0");
  fuel_consumption = new wxTextCtrl(panel, wxID_ANY, "2.5");
  limit_fuel = new wxCheckBox(panel, wxID_ANY, Label("maximum-fuel"));
  maximum_fuel = new wxTextCtrl(panel, wxID_ANY, "100.0");
  auto* grid = new wxFlexGridSizer(2, 8, 8);
  grid->AddGrowableCol(1);
  AddRow(grid, panel, Label("minimum-wind-angle"), min_wind_angle);
  AddRow(grid, panel, Label("maximum-wind-angle"), max_wind_angle);
  grid->Add(limit_true_wind, 0, wxALIGN_CENTER_VERTICAL);
  grid->Add(max_true_wind, 1, wxEXPAND);
  grid->Add(limit_apparent_wind, 0, wxALIGN_CENTER_VERTICAL);
  grid->Add(max_apparent_wind, 1, wxEXPAND);
  grid->Add(limit_waves, 0, wxALIGN_CENTER_VERTICAL);
  grid->Add(max_wave, 1, wxEXPAND);
  grid->Add(limit_opposing_wind_current, 0, wxALIGN_CENTER_VERTICAL);
  grid->Add(max_opposing_wind_current, 1, wxEXPAND);
  AddRow(grid, panel, Label("land-safety-margin"), land_safety_margin);
  AddRow(grid, panel, Label("maximum-latitude"), maximum_latitude);
  AddRow(grid, panel, Label("upwind-efficiency"), upwind_efficiency);
  AddRow(grid, panel, Label("downwind-efficiency"), downwind_efficiency);
  AddRow(grid, panel, Label("tack-penalty"), tack_penalty);
  AddRow(grid, panel, Label("gybe-penalty"), gybe_penalty);
  root->Add(avoid_land, 0, wxALL, 12);
  root->Add(use_currents, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(require_current_data, 0, wxLEFT | wxRIGHT | wxBOTTOM, 28);
  root->Add(use_waves, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(require_wave_data, 0, wxLEFT | wxRIGHT | wxBOTTOM, 28);
  root->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(new wxStaticLine(panel), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
  root->Add(allow_motor_sailing, 0, wxALL, 12);
  root->Add(allow_motor, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  auto* propulsion_grid = new wxFlexGridSizer(2, 8, 8);
  propulsion_grid->AddGrowableCol(1);
  AddRow(propulsion_grid, panel, Label("motor-threshold"), motor_threshold);
  AddRow(propulsion_grid, panel, Label("motor-speed"), motor_speed);
  AddRow(propulsion_grid, panel, Label("motor-sailing-boost"),
         motor_sailing_boost);
  AddRow(propulsion_grid, panel, Label("motor-hysteresis"), motor_hysteresis);
  AddRow(propulsion_grid, panel, Label("minimum-motor-run"),
         minimum_motor_run);
  AddRow(propulsion_grid, panel, Label("mode-change-penalty"),
         mode_change_penalty);
  propulsion_grid->Add(limit_motor_hours, 0, wxALIGN_CENTER_VERTICAL);
  propulsion_grid->Add(maximum_motor_hours, 1, wxEXPAND);
  AddRow(propulsion_grid, panel, Label("fuel-consumption"), fuel_consumption);
  propulsion_grid->Add(limit_fuel, 0, wxALIGN_CENTER_VERTICAL);
  propulsion_grid->Add(maximum_fuel, 1, wxEXPAND);
  root->Add(propulsion_grid, 0,
            wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(new wxStaticText(panel, wxID_ANY,
                             "Safety responses distinguish covered, unsafe, "
                             "missing coverage and unknown. They remain "
                             "advisory and must be checked by the navigator."),
            0, wxEXPAND | wxALL, 12);
  panel->SetSizer(root);
  use_currents->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    require_current_data->Enable(use_currents->GetValue());
    limit_opposing_wind_current->Enable(use_currents->GetValue());
    max_opposing_wind_current->Enable(
        use_currents->GetValue() &&
        limit_opposing_wind_current->GetValue());
  });
  use_waves->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    require_wave_data->Enable(use_waves->GetValue());
    limit_waves->Enable(use_waves->GetValue());
    max_wave->Enable(use_waves->GetValue() && limit_waves->GetValue());
  });
  limit_waves->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    max_wave->Enable(use_waves->GetValue() && limit_waves->GetValue());
  });
  limit_true_wind->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    max_true_wind->Enable(limit_true_wind->GetValue());
  });
  limit_opposing_wind_current->Bind(wxEVT_CHECKBOX,
                                    [this](wxCommandEvent&) {
    max_opposing_wind_current->Enable(
        limit_opposing_wind_current->GetValue() &&
        use_currents->GetValue());
  });
  limit_apparent_wind->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    max_apparent_wind->Enable(limit_apparent_wind->GetValue());
  });
  auto update_propulsion = [this] {
    const bool enabled = allow_motor_sailing->GetValue() ||
                         allow_motor->GetValue();
    motor_threshold->Enable(enabled);
    motor_speed->Enable(enabled && allow_motor->GetValue());
    motor_sailing_boost->Enable(enabled && allow_motor_sailing->GetValue());
    motor_hysteresis->Enable(enabled);
    minimum_motor_run->Enable(enabled);
    mode_change_penalty->Enable(enabled);
    limit_motor_hours->Enable(enabled);
    maximum_motor_hours->Enable(enabled && limit_motor_hours->GetValue());
    fuel_consumption->Enable(enabled);
    limit_fuel->Enable(enabled);
    maximum_fuel->Enable(enabled && limit_fuel->GetValue());
  };
  allow_motor_sailing->Bind(
      wxEVT_CHECKBOX, [update_propulsion](wxCommandEvent&) {
        update_propulsion();
      });
  allow_motor->Bind(wxEVT_CHECKBOX,
                    [update_propulsion](wxCommandEvent&) {
                      update_propulsion();
                    });
  limit_motor_hours->Bind(
      wxEVT_CHECKBOX, [update_propulsion](wxCommandEvent&) {
        update_propulsion();
      });
  limit_fuel->Bind(wxEVT_CHECKBOX,
                   [update_propulsion](wxCommandEvent&) {
                     update_propulsion();
                   });
  update_propulsion();
  return panel;
}

wxPanel* PortableWeatherRoutingHost::Impl::CreateAdvancedPanel(
    wxNotebook* book) {
  auto* panel = new wxPanel(book);
  auto* grid = new wxFlexGridSizer(2, 8, 8);
  grid->AddGrowableCol(1);
  time_step = new wxSpinCtrl(panel, wxID_ANY);
  time_step->SetRange(300, 21600);
  time_step->SetValue(3600);
  heading_step = new wxSpinCtrl(panel, wxID_ANY);
  heading_step->SetRange(5, 45);
  heading_step->SetValue(15);
  adaptive_headings =
      new wxCheckBox(panel, wxID_ANY, Label("adaptive-headings"));
  adaptive_headings->SetValue(true);
  refined_heading_step = new wxSpinCtrl(panel, wxID_ANY);
  refined_heading_step->SetRange(1, 45);
  refined_heading_step->SetValue(5);
  spatial_cell = new wxTextCtrl(panel, wxID_ANY, "3.0");
  labels_per_cell = new wxSpinCtrl(panel, wxID_ANY);
  labels_per_cell->SetRange(1, 8);
  labels_per_cell->SetValue(2);
  maximum_search_angle = new wxSpinCtrl(panel, wxID_ANY);
  maximum_search_angle->SetRange(30, 180);
  maximum_search_angle->SetValue(120);
  destination_tolerance = new wxTextCtrl(panel, wxID_ANY, "1.0");
  max_hours = new wxSpinCtrl(panel, wxID_ANY);
  max_hours->SetRange(6, 720);
  max_hours->SetValue(120);
  max_states = new wxSpinCtrl(panel, wxID_ANY);
  max_states->SetRange(1000, 500000);
  max_states->SetValue(80000);
  compare_departures =
      new wxCheckBox(panel, wxID_ANY, Label("compare-departures"));
  departure_window = new wxSpinCtrl(panel, wxID_ANY);
  departure_window->SetRange(1, 24);
  departure_window->SetValue(6);
  departure_spacing = new wxSpinCtrl(panel, wxID_ANY);
  departure_spacing->SetRange(1, 12);
  departure_spacing->SetValue(1);
  departure_workers = new wxSpinCtrl(panel, wxID_ANY);
  departure_workers->SetRange(1, 8);
  departure_workers->SetValue(4);
  AddRow(grid, panel, Label("time-step"), time_step);
  AddRow(grid, panel, Label("heading-step"), heading_step);
  AddRow(grid, panel, Label("refined-heading-step"), refined_heading_step);
  AddRow(grid, panel, Label("spatial-cell"), spatial_cell);
  AddRow(grid, panel, Label("labels-per-cell"), labels_per_cell);
  AddRow(grid, panel, Label("maximum-search-angle"), maximum_search_angle);
  AddRow(grid, panel, Label("destination-tolerance"), destination_tolerance);
  AddRow(grid, panel, Label("maximum-hours"), max_hours);
  AddRow(grid, panel, Label("maximum-states"), max_states);
  AddRow(grid, panel, Label("departure-window"), departure_window);
  AddRow(grid, panel, Label("departure-spacing"), departure_spacing);
  AddRow(grid, panel, Label("departure-workers"), departure_workers);
  auto* root = new wxBoxSizer(wxVERTICAL);
  root->Add(adaptive_headings, 0, wxLEFT | wxRIGHT | wxTOP, 12);
  root->Add(compare_departures, 0, wxLEFT | wxRIGHT | wxTOP, 12);
  root->Add(grid, 0, wxEXPAND | wxALL, 12);
  root->Add(new wxStaticText(
                panel, wxID_ANY,
                "Independent Wasm searches run in parallel up to the selected "
                "worker limit. "
                "The shortest completed passage is selected; other successful "
                "routes remain as thin comparison overlays."),
            0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  panel->SetSizer(root);
  adaptive_headings->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    refined_heading_step->Enable(adaptive_headings->GetValue());
  });
  return panel;
}

wxPanel* PortableWeatherRoutingHost::Impl::CreateResultsPanel(
    wxNotebook* book) {
  auto* panel = new wxPanel(book);
  auto* root = new wxBoxSizer(wxVERTICAL);
  metrics = new wxStaticText(panel, wxID_ANY, "No route calculated");
  root->Add(metrics, 0, wxEXPAND | wxALL, 12);
  departure_results =
      new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
  departure_results->SetName(Label("departure-results"));
  auto table_columns = [this](const wxString& id) {
    wxArrayString result;
    for (int index = 0; index < surface_definition["controls"].Size(); ++index) {
      wxJSONValue control = surface_definition["controls"][index];
      if (control["id"].AsString() != id) continue;
      for (int column = 0; column < control["columns"].Size(); ++column)
        result.Add(control["columns"][column].AsString());
      break;
    }
    return result;
  };
  const wxArrayString result_columns = table_columns("departure-results");
  for (size_t column = 0; column < result_columns.size(); ++column)
    departure_results->InsertColumn(static_cast<int>(column),
                                    result_columns[column]);
  departure_results->SetMinSize(wxSize(-1, panel->FromDIP(150)));
  departure_results->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& event) {
    if (updating_departure_selection) return;
    const long data = departure_results->GetItemData(event.GetIndex());
    if (data >= 0 && static_cast<size_t>(data) < departure_result_rows.size() &&
        departure_result_rows[static_cast<size_t>(data)].success) {
      SelectDepartureResult(static_cast<size_t>(data));
    } else if (selected_departure_result !=
               std::numeric_limits<size_t>::max()) {
      SelectDepartureResult(selected_departure_result);
    }
  });
  root->Add(departure_results, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(new wxStaticText(panel, wxID_ANY, Label("route-schedule")), 0,
            wxEXPAND | wxLEFT | wxRIGHT, 12);
  route_schedule =
      new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
  const wxArrayString schedule_columns = table_columns("route-schedule");
  for (size_t column = 0; column < schedule_columns.size(); ++column)
    route_schedule->InsertColumn(static_cast<int>(column),
                                 schedule_columns[column]);
  route_schedule->SetMinSize(wxSize(-1, panel->FromDIP(135)));
  root->Add(route_schedule, 1,
            wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(
      new wxStaticText(panel, wxID_ANY, Label("validation-diagnostics")), 0,
      wxEXPAND | wxLEFT | wxRIGHT, 12);
  validation_diagnostics = new wxTextCtrl(
      panel, wxID_ANY, "No route validation result", wxDefaultPosition,
      wxSize(-1, panel->FromDIP(70)), wxTE_MULTILINE | wxTE_READONLY);
  root->Add(validation_diagnostics, 0,
            wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  show_isochrones = new wxCheckBox(panel, wxID_ANY, Label("show-isochrones"));
  show_stability_corridor =
      new wxCheckBox(panel, wxID_ANY, Label("show-stability-corridor"));
  show_route_wind =
      new wxCheckBox(panel, wxID_ANY, Label("show-route-wind"));
  route_to_cursor = new wxCheckBox(panel, wxID_ANY, Label("route-to-cursor"));
  boat_at_grib_time =
      new wxCheckBox(panel, wxID_ANY, Label("boat-at-grib-time"));
  root->Add(show_isochrones, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(show_stability_corridor, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(show_route_wind, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(route_to_cursor, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(boat_at_grib_time, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  for (auto* toggle : {show_isochrones, show_stability_corridor,
                       show_route_wind, route_to_cursor, boat_at_grib_time}) {
    toggle->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
      SaveSettings();
      if (parent) parent->Refresh();
    });
  }
  root->Add(
      new wxStaticText(panel, wxID_ANY,
                       "Route overlays and exported GPX are planning outputs, "
                       "not safe or authoritative navigation routes."),
      0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  export_gpx = new wxButton(panel, wxID_ANY, Label("export-gpx"));
  export_gpx->Enable(false);
  export_gpx->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ExportGpx(); });
  send_to_opencpn =
      new wxButton(panel, wxID_ANY, Label("send-to-opencpn"));
  send_to_opencpn->Enable(false);
  send_to_opencpn->Bind(wxEVT_BUTTON,
                        [this](wxCommandEvent&) { SendToOpenCpn(); });
  auto* output_actions = new wxBoxSizer(wxHORIZONTAL);
  output_actions->Add(export_gpx, 0, wxRIGHT, 8);
  output_actions->Add(send_to_opencpn, 0);
  root->Add(output_actions, 0, wxALL, 12);
  panel->SetSizer(root);
  return panel;
}

void PortableWeatherRoutingHost::Impl::LoadSettings() {
  const wxString bundled_polar = package_root + wxFILE_SEP_PATH + "resources" +
                                 wxFILE_SEP_PATH +
                                 "Nicholson35_Mk1_cruising_realistic.pol";
  show_isochrones->SetValue(true);
  show_stability_corridor->SetValue(true);
  show_route_wind->SetValue(true);
  route_to_cursor->SetValue(false);
  boat_at_grib_time->SetValue(true);
  if (!config) {
    vessel_performance_file->SetPath(bundled_polar);
    LoadVesselPerformance(bundled_polar, false);
    UpdateDepartureSummary();
    UpdateTimeColumnLabels();
    return;
  }
  const wxString old_path = config->GetPath();
  config->SetPath("/PortablePlugins/" + plugin_id + "/Routing");
  const wxString performance_path =
      config->Read("vesselPerformancePath", bundled_polar);
  vessel_performance_file->SetPath(performance_path);
  LoadVesselPerformance(performance_path, false);
  int64_t initial_departure = 0;
  wxString departure_error;
  const bool have_initial_departure =
      GetDepartureUnixTime(&initial_departure, &departure_error);
  PortableDepartureZone configured_zone;
  const wxString configured_zone_setting =
      config->Read("departureTimeZone", "UTC");
  if (ParsePortableDepartureZoneSetting(configured_zone_setting,
                                        &configured_zone)) {
    auto configured = std::find(departure_time_zones.begin(),
                                departure_time_zones.end(), configured_zone);
    if (configured == departure_time_zones.end() &&
        configured_zone.kind == PortableDepartureZoneKind::kFixedOffset) {
      const int offset = configured_zone.offset_minutes;
      const char sign = offset < 0 ? '-' : '+';
      const int absolute = std::abs(offset);
      departure_timezone->Append(wxString::Format(
          "Fixed UTC%c%02d:%02d", sign, absolute / 60, absolute % 60));
      departure_time_zones.push_back(configured_zone);
      configured = std::prev(departure_time_zones.end());
    }
    if (configured != departure_time_zones.end())
      departure_timezone->SetSelection(
          static_cast<int>(std::distance(departure_time_zones.begin(),
                                         configured)));
  }
  if (have_initial_departure) SetDepartureUnixTime(initial_departure);
  avoid_land->SetValue(config->ReadBool("avoidUnsafeCharts", true));
  use_opencpn_route->SetValue(config->ReadBool("useOpenCpnRoute", false));
  configured_route_id = config->Read("openCpnRouteId", wxEmptyString);
  min_wind_angle->SetValue(config->ReadLong("minimumTrueWindAngle", 40));
  max_wind_angle->SetValue(config->ReadLong("maximumTrueWindAngle", 180));
  limit_true_wind->SetValue(config->ReadBool("limitTrueWind", true));
  max_true_wind->SetValue(config->Read("maximumTrueWind", "50"));
  limit_apparent_wind->SetValue(config->ReadBool("limitApparentWind", true));
  max_apparent_wind->SetValue(config->Read("maximumApparentWind", "50"));
  limit_waves->SetValue(config->ReadBool("limitWaves", true));
  max_wave->SetValue(config->Read("maximumWave", "8.0"));
  limit_opposing_wind_current->SetValue(
      config->ReadBool("limitOpposingWindCurrent", false));
  max_opposing_wind_current->SetValue(
      config->Read("maximumOpposingWindCurrent", "0.0"));
  land_safety_margin->SetValue(config->Read("landSafetyMarginNm", "0.4"));
  use_currents->SetValue(config->ReadBool("useCurrents", true));
  require_current_data->SetValue(
      config->ReadBool("requireCurrentData", false));
  use_waves->SetValue(config->ReadBool("useWaves", true));
  require_wave_data->SetValue(config->ReadBool("requireWaveData", true));
  maximum_latitude->SetValue(config->ReadLong("maximumLatitude", 89));
  upwind_efficiency->SetValue(config->ReadLong("upwindEfficiency", 100));
  downwind_efficiency->SetValue(config->ReadLong("downwindEfficiency", 100));
  tack_penalty->SetValue(config->ReadLong("tackPenaltySeconds", 300));
  gybe_penalty->SetValue(config->ReadLong("gybePenaltySeconds", 300));
  allow_motor_sailing->SetValue(
      config->ReadBool("allowMotorSailing", false));
  allow_motor->SetValue(config->ReadBool("allowMotor", false));
  motor_threshold->SetValue(config->Read("motorThresholdKnots", "3.0"));
  motor_speed->SetValue(config->Read("motorSpeedKnots", "5.5"));
  motor_sailing_boost->SetValue(
      config->Read("motorSailingBoostKnots", "1.5"));
  motor_hysteresis->SetValue(
      config->Read("motorCrossoverHysteresisKnots", "0.2"));
  minimum_motor_run->SetValue(
      config->ReadLong("minimumMotorRunSeconds", 1800));
  mode_change_penalty->SetValue(
      config->ReadLong("modeChangePenaltySeconds", 120));
  limit_motor_hours->SetValue(
      config->ReadBool("limitMotorHours", false));
  maximum_motor_hours->SetValue(
      config->Read("maximumMotorHours", "24.0"));
  fuel_consumption->SetValue(
      config->Read("fuelConsumptionLitresPerHour", "2.5"));
  limit_fuel->SetValue(config->ReadBool("limitFuel", false));
  maximum_fuel->SetValue(config->Read("maximumFuelLitres", "100.0"));
  time_step->SetValue(config->ReadLong("timeStepSeconds", 3600));
  heading_step->SetValue(config->ReadLong("headingStepDegrees", 15));
  adaptive_headings->SetValue(config->ReadBool("adaptiveHeadings", true));
  refined_heading_step->SetValue(
      config->ReadLong("refinedHeadingStepDegrees", 5));
  spatial_cell->SetValue(config->Read("spatialCellNm", "3.0"));
  labels_per_cell->SetValue(config->ReadLong("labelsPerCell", 2));
  maximum_search_angle->SetValue(
      config->ReadLong("maximumSearchAngleDegrees", 120));
  destination_tolerance->SetValue(
      config->Read("destinationToleranceNm", "1.0"));
  max_hours->SetValue(config->ReadLong("maximumHours", 120));
  max_states->SetValue(config->ReadLong("maximumStates", 80000));
  compare_departures->SetValue(config->ReadBool("compareDepartures", false));
  departure_window->SetValue(config->ReadLong("departureWindowHours", 6));
  departure_spacing->SetValue(config->ReadLong("departureSpacingHours", 1));
  departure_workers->SetValue(config->ReadLong("departureWorkers", 4));
  show_isochrones->SetValue(config->ReadBool("showIsochrones", true));
  show_stability_corridor->SetValue(
      config->ReadBool("showStabilityCorridor", true));
  show_route_wind->SetValue(config->ReadBool("showRouteWind", true));
  route_to_cursor->SetValue(config->ReadBool("routeToCursor", false));
  boat_at_grib_time->SetValue(config->ReadBool("boatAtGribTime", true));
  config->SetPath(old_path);

  require_current_data->Enable(use_currents->GetValue());
  require_wave_data->Enable(use_waves->GetValue());
  limit_waves->Enable(use_waves->GetValue());
  max_wave->Enable(use_waves->GetValue() && limit_waves->GetValue());
  max_true_wind->Enable(limit_true_wind->GetValue());
  max_apparent_wind->Enable(limit_apparent_wind->GetValue());
  limit_opposing_wind_current->Enable(use_currents->GetValue());
  max_opposing_wind_current->Enable(
      use_currents->GetValue() &&
      limit_opposing_wind_current->GetValue());
  refined_heading_step->Enable(adaptive_headings->GetValue());
  route_choice->Enable(use_opencpn_route->GetValue() &&
                       !navigation_routes.empty());
  const bool propulsion_enabled =
      allow_motor_sailing->GetValue() || allow_motor->GetValue();
  motor_threshold->Enable(propulsion_enabled);
  motor_speed->Enable(propulsion_enabled && allow_motor->GetValue());
  motor_sailing_boost->Enable(propulsion_enabled &&
                              allow_motor_sailing->GetValue());
  motor_hysteresis->Enable(propulsion_enabled);
  minimum_motor_run->Enable(propulsion_enabled);
  mode_change_penalty->Enable(propulsion_enabled);
  limit_motor_hours->Enable(propulsion_enabled);
  maximum_motor_hours->Enable(propulsion_enabled &&
                              limit_motor_hours->GetValue());
  fuel_consumption->Enable(propulsion_enabled);
  limit_fuel->Enable(propulsion_enabled);
  maximum_fuel->Enable(propulsion_enabled && limit_fuel->GetValue());
  UpdateDepartureSummary();
  UpdateTimeColumnLabels();
}

void PortableWeatherRoutingHost::Impl::SaveSettings() {
  if (!config) return;
  const wxString old_path = config->GetPath();
  config->SetPath("/PortablePlugins/" + plugin_id + "/Routing");
  config->Write("settingsSchema", 6L);
  if (vessel_performance_file)
    config->Write("vesselPerformancePath", vessel_performance_file->GetPath());
  config->Write("departureTimeZone",
                 PortableDepartureZoneSetting(SelectedDepartureZone()));
  config->Write("avoidUnsafeCharts", avoid_land->GetValue());
  config->Write("useOpenCpnRoute", use_opencpn_route->GetValue());
  if (route_choice && route_choice->GetSelection() != wxNOT_FOUND &&
      static_cast<size_t>(route_choice->GetSelection()) <
          navigation_routes.size())
    config->Write("openCpnRouteId",
                   navigation_routes[route_choice->GetSelection()].id);
  config->Write("minimumTrueWindAngle",
                 static_cast<long>(min_wind_angle->GetValue()));
  config->Write("maximumTrueWindAngle",
                 static_cast<long>(max_wind_angle->GetValue()));
  config->Write("limitTrueWind", limit_true_wind->GetValue());
  config->Write("maximumTrueWind", max_true_wind->GetValue());
  config->Write("limitApparentWind", limit_apparent_wind->GetValue());
  config->Write("maximumApparentWind", max_apparent_wind->GetValue());
  config->Write("limitWaves", limit_waves->GetValue());
  config->Write("maximumWave", max_wave->GetValue());
  config->Write("limitOpposingWindCurrent",
                 limit_opposing_wind_current->GetValue());
  config->Write("maximumOpposingWindCurrent",
                 max_opposing_wind_current->GetValue());
  config->Write("landSafetyMarginNm", land_safety_margin->GetValue());
  config->Write("useCurrents", use_currents->GetValue());
  config->Write("requireCurrentData", require_current_data->GetValue());
  config->Write("useWaves", use_waves->GetValue());
  config->Write("requireWaveData", require_wave_data->GetValue());
  config->Write("maximumLatitude",
                 static_cast<long>(maximum_latitude->GetValue()));
  config->Write("upwindEfficiency",
                 static_cast<long>(upwind_efficiency->GetValue()));
  config->Write("downwindEfficiency",
                 static_cast<long>(downwind_efficiency->GetValue()));
  config->Write("tackPenaltySeconds",
                 static_cast<long>(tack_penalty->GetValue()));
  config->Write("gybePenaltySeconds",
                 static_cast<long>(gybe_penalty->GetValue()));
  config->Write("allowMotorSailing", allow_motor_sailing->GetValue());
  config->Write("allowMotor", allow_motor->GetValue());
  config->Write("motorThresholdKnots", motor_threshold->GetValue());
  config->Write("motorSpeedKnots", motor_speed->GetValue());
  config->Write("motorSailingBoostKnots",
                 motor_sailing_boost->GetValue());
  config->Write("motorCrossoverHysteresisKnots",
                 motor_hysteresis->GetValue());
  config->Write("minimumMotorRunSeconds",
                 static_cast<long>(minimum_motor_run->GetValue()));
  config->Write("modeChangePenaltySeconds",
                 static_cast<long>(mode_change_penalty->GetValue()));
  config->Write("limitMotorHours", limit_motor_hours->GetValue());
  config->Write("maximumMotorHours", maximum_motor_hours->GetValue());
  config->Write("fuelConsumptionLitresPerHour",
                 fuel_consumption->GetValue());
  config->Write("limitFuel", limit_fuel->GetValue());
  config->Write("maximumFuelLitres", maximum_fuel->GetValue());
  config->Write("timeStepSeconds", static_cast<long>(time_step->GetValue()));
  config->Write("headingStepDegrees",
                 static_cast<long>(heading_step->GetValue()));
  config->Write("adaptiveHeadings", adaptive_headings->GetValue());
  config->Write("refinedHeadingStepDegrees",
                 static_cast<long>(refined_heading_step->GetValue()));
  config->Write("spatialCellNm", spatial_cell->GetValue());
  config->Write("labelsPerCell",
                 static_cast<long>(labels_per_cell->GetValue()));
  config->Write("maximumSearchAngleDegrees",
                 static_cast<long>(maximum_search_angle->GetValue()));
  config->Write("destinationToleranceNm", destination_tolerance->GetValue());
  config->Write("maximumHours", static_cast<long>(max_hours->GetValue()));
  config->Write("maximumStates", static_cast<long>(max_states->GetValue()));
  config->Write("compareDepartures", compare_departures->GetValue());
  config->Write("departureWindowHours",
                 static_cast<long>(departure_window->GetValue()));
  config->Write("departureSpacingHours",
                 static_cast<long>(departure_spacing->GetValue()));
  config->Write("departureWorkers",
                 static_cast<long>(departure_workers->GetValue()));
  config->Write("showIsochrones", show_isochrones->GetValue());
  config->Write("showStabilityCorridor",
                 show_stability_corridor->GetValue());
  config->Write("showRouteWind", show_route_wind->GetValue());
  config->Write("routeToCursor", route_to_cursor->GetValue());
  config->Write("boatAtGribTime", boat_at_grib_time->GetValue());
  config->SetPath(old_path);
  config->Flush();
}

void PortableWeatherRoutingHost::Impl::CreateEditor() {
  editor = new wxDialog(frame, wxID_ANY, "Routing Configuration — " + surface_title,
                        wxDefaultPosition, wxSize(820, 760),
                        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  editor->SetMinSize(editor->FromDIP(wxSize(680, 560)));
  auto* root = new wxBoxSizer(wxVERTICAL);
  notebook = new wxNotebook(editor, wxID_ANY);
  notebook->AddPage(CreateRoutePanel(notebook), surface_tabs[0]);
  notebook->AddPage(CreateSafetyPanel(notebook), surface_tabs[1]);
  notebook->AddPage(CreateAdvancedPanel(notebook), surface_tabs[2]);
  notebook->AddPage(CreateResultsPanel(notebook), surface_tabs[3]);
  root->Add(notebook, 1, wxEXPAND | wxALL, 8);
  auto* buttons = new wxBoxSizer(wxHORIZONTAL);
  calculate = new wxButton(editor, wxID_ANY, Label("calculate"));
  cancel = new wxButton(editor, wxID_ANY, Label("cancel"));
  cancel->Enable(false);
  auto* close = new wxButton(editor, wxID_CLOSE, "Close");
  buttons->AddStretchSpacer();
  buttons->Add(calculate, 0, wxRIGHT, 8);
  buttons->Add(cancel, 0, wxRIGHT, 8);
  buttons->Add(close);
  root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  calculate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Start(); });
  cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    cancelled.store(true);
    if (cancel_routes) cancel_routes();
    if (status) status->SetLabel("Cancelling…");
  });
  close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    SaveSettings();
    editor->Hide();
  });
  editor->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
    SaveSettings();
    editor->Hide();
    event.Veto();
  });
  editor->SetSizer(root);
}

void PortableWeatherRoutingHost::Impl::ShowEditor(size_t tab) {
  if (!editor) return;
  // Developer startup actions can create the workbench before OpenCPN's
  // deferred initialization has loaded navobj.db.  Refresh here so opening the
  // configuration always sees the current waypoints and routes, even when the
  // initial snapshot was necessarily empty.
  RefreshNavigationPositions(false);
  if (notebook && tab < static_cast<size_t>(notebook->GetPageCount()))
    notebook->SetSelection(static_cast<int>(tab));
  RefreshEnvironmentSummary();
  RelayoutRoutePanel();
  editor->Layout();
  editor->Show();
  editor->Raise();
}

void PortableWeatherRoutingHost::Impl::RefreshEnvironmentSummary() {
  if (!provider || !dataset_summary) return;
  const wxString latest = dataset_summary();
  if (latest == current_dataset_summary) return;
  current_dataset_summary = latest;
  provider->SetLabel("Environment: " + current_dataset_summary);
  RelayoutRoutePanel();
  if (editor) editor->Layout();
}

void PortableWeatherRoutingHost::Impl::PopulateManagerPositions() {
  if (!manager_positions) return;
  manager_positions->DeleteAllItems();
  auto add = [this](const PortableNavigationPosition& point,
                    const wxString& source) {
    const long row = manager_positions->InsertItem(
        manager_positions->GetItemCount(), point.name);
    manager_positions->SetItem(row, 1,
                               wxString::Format("%.6f", point.latitude));
    manager_positions->SetItem(row, 2,
                               wxString::Format("%.6f", point.longitude));
    manager_positions->SetItem(row, 3, source);
  };
  PortableNavigationPosition vessel;
  if (vessel_position && vessel_position(&vessel)) add(vessel, "Vessel");
  for (const auto& waypoint : waypoints) add(waypoint, "OpenCPN waypoint");
  PortableNavigationPosition cursor;
  if (cursor_position && cursor_position(&cursor)) add(cursor, "Chart cursor");
  for (int column = 0; column < manager_positions->GetColumnCount(); ++column)
    manager_positions->SetColumnWidth(column, wxLIST_AUTOSIZE_USEHEADER);
}

void PortableWeatherRoutingHost::Impl::PopulateManagerRouting(
    const wxString& state) {
  if (!manager_routings || !start_source || !dest_source) return;
  manager_routings->DeleteAllItems();
  auto position_name = [this](bool start) {
    wxChoice* source = start ? start_source : dest_source;
    wxChoice* waypoint = start ? start_waypoint : dest_waypoint;
    if (source->GetSelection() == kOpenCpnWaypoint &&
        waypoint->GetSelection() != wxNOT_FOUND)
      return waypoint->GetStringSelection().BeforeFirst(wxUniChar(0x2014)).Trim();
    return source->GetStringSelection();
  };
  const long row = manager_routings->InsertItem(0, state);
  manager_routings->SetItem(row, 1, position_name(true));
  manager_routings->SetItem(row, 2, position_name(false));
  int64_t configured_departure = 0;
  wxString departure_error;
  manager_routings->SetItem(
      row, 3,
      GetDepartureUnixTime(&configured_departure, &departure_error)
          ? FormatRoutingTime(configured_departure)
          : "Invalid departure");
  if (selected_departure_result != std::numeric_limits<size_t>::max() &&
      selected_departure_result < departure_result_rows.size() &&
      departure_result_rows[selected_departure_result].success) {
    const auto& outcome =
        departure_result_rows[selected_departure_result].outcome;
    manager_routings->SetItem(
        row, 4,
        FormatRoutingTime(outcome.departure_unix_time +
                          static_cast<int64_t>(outcome.duration_seconds)));
    manager_routings->SetItem(row, 5, FormatElapsed(outcome.duration_seconds));
    manager_routings->SetItem(
        row, 6, wxString::Format("%.1f NM", outcome.distance_nautical_miles));
  } else {
    for (int column = 4; column <= 6; ++column)
      manager_routings->SetItem(row, column, "N/A");
  }
  manager_routings->SetItemState(row, wxLIST_STATE_SELECTED,
                                 wxLIST_STATE_SELECTED);
  for (int column = 0; column < manager_routings->GetColumnCount(); ++column)
    manager_routings->SetColumnWidth(column, wxLIST_AUTOSIZE_USEHEADER);
}

void PortableWeatherRoutingHost::Impl::DispatchSurfaceAction(
    const wxString& action) {
  if (action == "close") {
    frame->Close();
  } else if (action == "refresh-positions") {
    RefreshNavigationPositions();
  } else if (action == "new-routing") {
    SetDepartureUnixTime(
        ((static_cast<int64_t>(wxDateTime::Now().GetTicks()) + 899) / 900) *
        900);
    ShowEditor(0);
    PopulateManagerRouting("Not computed");
  } else if (action == "edit-routing" || action == "show-configuration") {
    ShowEditor(0);
  } else if (action == "show-results") {
    ShowEditor(3);
  } else if (action == "compute-routing") {
    Start();
  } else if (action == "stop-routing") {
    cancelled.store(true);
    if (cancel_routes) cancel_routes();
    if (status) status->SetLabel("Cancelling…");
  } else if (action == "export-gpx") {
    ExportGpx();
  } else if (action == "send-to-opencpn") {
    SendToOpenCpn();
  } else if (action == "about") {
    wxMessageBox(
        surface_title +
            " is a portable WebAssembly weather-routing component. It "
            "consumes an immutable environmental dataset through typed host "
            "services, escalates from forward isochrones through reverse "
            "recovery and a time-dependent graph fallback, and independently "
            "revalidates every delivered route.",
        "About " + surface_title, wxOK | wxICON_INFORMATION, frame);
  } else if (action == "show-isochrones" ||
             action == "show-stability-corridor" ||
             action == "show-route-wind" || action == "route-to-cursor" ||
             action == "boat-at-grib-time") {
    wxCheckBox* toggle = action == "show-isochrones" ? show_isochrones
                         : action == "show-stability-corridor"
                             ? show_stability_corridor
                         : action == "show-route-wind" ? show_route_wind
                         : action == "route-to-cursor" ? route_to_cursor
                                                        : boat_at_grib_time;
    const auto item = surface_menu_items.find(action);
    if (toggle && item != surface_menu_items.end())
      toggle->SetValue(item->second->IsChecked());
    SaveSettings();
    if (parent) parent->Refresh();
  }
}

void PortableWeatherRoutingHost::Impl::CreateFrame() {
  frame = new wxFrame(parent, wxID_ANY, surface_title, wxDefaultPosition,
                      wxSize(940, 620),
                      wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT);
  frame->SetMinSize(frame->FromDIP(wxSize(720, 460)));

  auto* menu_bar = new wxMenuBar;
  for (const auto& menu_definition : surface_menus) {
    auto* menu = new wxMenu;
    for (const auto& item_definition : menu_definition.items) {
      if (item_definition.separator) {
        menu->AppendSeparator();
        continue;
      }
      const wxString& action = item_definition.id;
      wxString label = item_definition.label;
      const wxString& accelerator = item_definition.accelerator;
      if (!accelerator.empty()) label += "\t" + accelerator;
      const wxItemKind kind =
          item_definition.checkable ? wxITEM_CHECK : wxITEM_NORMAL;
      wxMenuItem* item = menu->Append(wxID_ANY, label, wxEmptyString, kind);
      surface_menu_items[action] = item;
      frame->Bind(wxEVT_MENU,
                  [this, action](wxCommandEvent&) {
                    DispatchSurfaceAction(action);
                  },
                  item->GetId());
    }
    menu_bar->Append(menu, menu_definition.label);
  }
  frame->SetMenuBar(menu_bar);

  auto* root = new wxBoxSizer(wxVERTICAL);
  auto* splitter = new wxSplitterWindow(frame, wxID_ANY, wxDefaultPosition,
                                        wxDefaultSize, wxSP_3D);
  splitter->SetSashGravity(0.32);
  splitter->SetMinimumPaneSize(frame->FromDIP(180));
  auto* positions_panel = new wxPanel(splitter);
  auto* positions_root = new wxStaticBoxSizer(
      wxVERTICAL, positions_panel,
      surface_definition["manager"]["positions"]["label"].AsString());
  manager_positions = new wxListCtrl(
      positions_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
      wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
  for (int index = 0;
       index < surface_definition["manager"]["positions"]["columns"].Size();
       ++index)
    manager_positions->InsertColumn(
        index, surface_definition["manager"]["positions"]["columns"][index]
                   .AsString());
  positions_root->Add(manager_positions, 1, wxEXPAND | wxALL, 5);
  positions_panel->SetSizer(positions_root);

  auto* routings_panel = new wxPanel(splitter);
  auto* routings_root = new wxStaticBoxSizer(
      wxVERTICAL, routings_panel,
      surface_definition["manager"]["routings"]["label"].AsString());
  manager_routings = new wxListCtrl(
      routings_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
      wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
  for (int index = 0;
       index < surface_definition["manager"]["routings"]["columns"].Size();
       ++index)
    manager_routings->InsertColumn(
        index, surface_definition["manager"]["routings"]["columns"][index]
                   .AsString());
  manager_routings->Bind(wxEVT_LIST_ITEM_ACTIVATED,
                         [this](wxListEvent&) { ShowEditor(0); });
  routings_root->Add(manager_routings, 1, wxEXPAND | wxALL, 5);
  auto* actions = new wxBoxSizer(wxHORIZONTAL);
  for (int index = 0;
       index < surface_definition["manager"]["actions"].Size(); ++index) {
    wxJSONValue action_definition =
        surface_definition["manager"]["actions"][index];
    const wxString action = action_definition["id"].AsString();
    auto* button = new wxButton(
        routings_panel, wxID_ANY, action_definition["label"].AsString());
    button->Bind(wxEVT_BUTTON,
                 [this, action](wxCommandEvent&) {
                   DispatchSurfaceAction(action);
                 });
    actions->Add(button, 0, wxRIGHT, 6);
    if (action == "compute-routing") manager_compute = button;
    if (action == "edit-routing") manager_edit = button;
    if (action == "export-gpx") manager_export = button;
  }
  manager_stop = new wxButton(routings_panel, wxID_ANY, "&Stop");
  manager_stop->Enable(false);
  manager_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    DispatchSurfaceAction("stop-routing");
  });
  actions->Add(manager_stop, 0, wxRIGHT, 6);
  routings_root->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
  routings_panel->SetSizer(routings_root);
  splitter->SplitVertically(positions_panel, routings_panel,
                            frame->FromDIP(285));
  root->Add(splitter, 1, wxEXPAND | wxALL, 5);
  status = new wxStaticText(frame, wxID_ANY, "Ready");
  gauge = new wxGauge(frame, wxID_ANY, 100);
  root->Add(status, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
  root->Add(gauge, 0, wxEXPAND | wxALL, 12);
  frame->SetSizer(root);

  CreateEditor();
  environment_refresh_timer.SetOwner(frame);
  frame->Bind(
      wxEVT_TIMER,
      [this](wxTimerEvent&) { RefreshEnvironmentSummary(); },
      environment_refresh_timer.GetId());
  environment_refresh_timer.Start(1000);
  LoadSettings();
  RefreshNavigationPositions(true);
  PopulateManagerPositions();
  PopulateManagerRouting("Not computed");
  for (const auto& [action, toggle] :
       std::initializer_list<std::pair<wxString, wxCheckBox*>>{
           {"show-isochrones", show_isochrones},
           {"show-stability-corridor", show_stability_corridor},
           {"show-route-wind", show_route_wind},
           {"route-to-cursor", route_to_cursor},
           {"boat-at-grib-time", boat_at_grib_time}}) {
    const auto item = surface_menu_items.find(action);
    if (item != surface_menu_items.end()) item->second->Check(toggle->GetValue());
  }
  if (manager_export) manager_export->Enable(false);
  frame->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
    SaveSettings();
    if (worker.joinable()) {
      cancelled.store(true);
      if (cancel_routes) cancel_routes();
    }
    if (editor) editor->Hide();
    frame->Hide();
    event.Veto();
  });
  frame->Layout();
}

bool PortableWeatherRoutingHost::Impl::Show(wxString* error) {
  if (stopped.load() || !calculate_route) {
    if (error) *error = "routing host is unavailable";
    return false;
  }
  if (!surface_loaded) {
    if (!LoadSurface(package_root, surface_resource, &surface_title,
                     &surface_tabs, &surface_labels, &surface_menus,
                     &surface_definition, error))
      return false;
    surface_loaded = true;
  }
  if (!frame)
    CreateFrame();
  else
    RefreshNavigationPositions(false);
  RefreshEnvironmentSummary();
  RelayoutRoutePanel();
  frame->Layout();
  frame->Show();
  frame->Raise();
  return true;
}

void PortableWeatherRoutingHost::Impl::Start() {
  if (worker.joinable()) return;
  const bool use_route = use_opencpn_route && use_opencpn_route->GetValue();
  if (!use_route &&
      (!ApplyPositionSource(true) || !ApplyPositionSource(false)))
    return;
  ocpn_portable_route_request request{};
  wxString departure_error;
  if (!Number(start_lat, &request.start_latitude) ||
      !Number(start_lon, &request.start_longitude) ||
      !Number(dest_lat, &request.destination_latitude) ||
      !Number(dest_lon, &request.destination_longitude) ||
      !GetDepartureUnixTime(&request.departure_unix_time, &departure_error)) {
    status->SetLabel(departure_error.empty()
                         ? "Enter valid positions and a departure time"
                         : departure_error);
    return;
  }
  std::vector<PortableNavigationPosition> routing_gates;
  if (use_route) {
    const int selection = route_choice ? route_choice->GetSelection()
                                       : wxNOT_FOUND;
    if (selection == wxNOT_FOUND || selection < 0 ||
        static_cast<size_t>(selection) >= navigation_routes.size() ||
        navigation_routes[static_cast<size_t>(selection)].points.size() < 2) {
      status->SetLabel(
          "Select an OpenCPN route containing at least two valid waypoints");
      return;
    }
    routing_gates =
        navigation_routes[static_cast<size_t>(selection)].points;
    request.start_latitude = routing_gates.front().latitude;
    request.start_longitude = routing_gates.front().longitude;
    request.destination_latitude = routing_gates.back().latitude;
    request.destination_longitude = routing_gates.back().longitude;
  } else {
    routing_gates = {{"manual:start", "Start", request.start_latitude,
                      request.start_longitude},
                     {"manual:destination", "Destination",
                      request.destination_latitude,
                      request.destination_longitude}};
  }
  const auto selected_performance = vessel_performance;
  if (!selected_performance || selected_performance->grids.empty()) {
    status->SetLabel("Load a valid OpenCPN boat .xml or polar .pol file first");
    return;
  }
  request.time_step_seconds = time_step->GetValue();
  request.heading_step_degrees = heading_step->GetValue();
  request.refined_heading_step_degrees = refined_heading_step->GetValue();
  request.adaptive_headings = adaptive_headings->GetValue();
  if (request.refined_heading_step_degrees > request.heading_step_degrees) {
    status->SetLabel(
        "Refined heading resolution cannot exceed coarse heading resolution");
    return;
  }
  if (!Number(spatial_cell, &request.spatial_cell_nautical_miles) ||
      request.spatial_cell_nautical_miles < 0.25 ||
      request.spatial_cell_nautical_miles > 30.0) {
    status->SetLabel("Search spatial cell must be between 0.25 and 30 NM");
    return;
  }
  request.labels_per_cell = labels_per_cell->GetValue();
  request.max_hours = max_hours->GetValue();
  request.max_states = max_states->GetValue();
  request.avoid_unsafe_charts = avoid_land->GetValue();
  request.min_true_wind_angle_degrees = min_wind_angle->GetValue();
  request.max_true_wind_angle_degrees = max_wind_angle->GetValue();
  if (request.min_true_wind_angle_degrees >
      request.max_true_wind_angle_degrees) {
    status->SetLabel(
        "Minimum true-wind angle cannot exceed maximum true-wind angle");
    return;
  }
  if (limit_true_wind->GetValue() &&
      (!Number(max_true_wind, &request.max_wind_knots) ||
       request.max_wind_knots <= 0.0)) {
    status->SetLabel("Maximum true-wind speed must be a positive number");
    return;
  }
  if (limit_apparent_wind->GetValue() &&
      (!Number(max_apparent_wind, &request.max_apparent_wind_knots) ||
       request.max_apparent_wind_knots <= 0.0)) {
    status->SetLabel("Maximum apparent-wind speed must be a positive number");
    return;
  }
  if (use_waves->GetValue() && limit_waves->GetValue() &&
      (!Number(max_wave, &request.max_wave_metres) ||
       request.max_wave_metres < 0.0)) {
    status->SetLabel("Maximum wave height must be a non-negative number");
    return;
  }
  if (!Number(land_safety_margin,
              &request.land_safety_margin_nautical_miles) ||
      request.land_safety_margin_nautical_miles < 0.0 ||
      request.land_safety_margin_nautical_miles > 20.0) {
    status->SetLabel("Land safety margin must be between 0 and 20 NM");
    return;
  }
  if (use_currents->GetValue() &&
      limit_opposing_wind_current->GetValue() &&
      (!Number(max_opposing_wind_current,
               &request.max_opposing_wind_current_knots_squared) ||
       request.max_opposing_wind_current_knots_squared <= 0.0)) {
    status->SetLabel(
        "Wind-against-current limit must be a positive value in kt squared");
    return;
  }
  if (!Number(destination_tolerance, &request.destination_tolerance_nm) ||
      request.destination_tolerance_nm < 0.05 ||
      request.destination_tolerance_nm > 20.0) {
    status->SetLabel("Destination tolerance must be between 0.05 and 20 NM");
    return;
  }
  request.maximum_latitude_degrees = maximum_latitude->GetValue();
  request.upwind_efficiency = upwind_efficiency->GetValue() / 100.0;
  request.downwind_efficiency = downwind_efficiency->GetValue() / 100.0;
  request.tack_penalty_seconds = tack_penalty->GetValue();
  request.gybe_penalty_seconds = gybe_penalty->GetValue();
  request.allow_motor_sailing = allow_motor_sailing->GetValue();
  request.allow_motor = allow_motor->GetValue();
  const bool propulsion_enabled =
      request.allow_motor_sailing || request.allow_motor;
  if (propulsion_enabled) {
    if (!Number(motor_threshold,
                &request.motor_below_sailing_speed_knots) ||
        request.motor_below_sailing_speed_knots < 0.0 ||
        request.motor_below_sailing_speed_knots > 30.0) {
      status->SetLabel(
          "Propulsion sailing-speed threshold must be between 0 and 30 kt");
      return;
    }
    if (request.allow_motor &&
        (!Number(motor_speed, &request.motor_speed_knots) ||
         request.motor_speed_knots <= 0.0 ||
         request.motor_speed_knots > 50.0)) {
      status->SetLabel("Motor-only speed must be between 0 and 50 kt");
      return;
    }
    if (request.allow_motor_sailing &&
        (!Number(motor_sailing_boost,
                 &request.motor_sailing_boost_knots) ||
         request.motor_sailing_boost_knots < 0.0 ||
         request.motor_sailing_boost_knots > 30.0)) {
      status->SetLabel(
          "Motor-sailing speed increase must be between 0 and 30 kt");
      return;
    }
    if (!Number(motor_hysteresis,
                &request.motor_crossover_hysteresis_knots) ||
        request.motor_crossover_hysteresis_knots < 0.0 ||
        request.motor_crossover_hysteresis_knots > 10.0) {
      status->SetLabel(
          "Propulsion crossover hysteresis must be between 0 and 10 kt");
      return;
    }
    request.minimum_motor_run_seconds = minimum_motor_run->GetValue();
    request.mode_change_penalty_seconds = mode_change_penalty->GetValue();
    if (!Number(fuel_consumption,
                &request.fuel_consumption_litres_per_hour) ||
        request.fuel_consumption_litres_per_hour <= 0.0 ||
        request.fuel_consumption_litres_per_hour > 1000.0) {
      status->SetLabel(
          "Fuel consumption must be between 0 and 1000 litres/hour");
      return;
    }
    request.limits_available |= 32;
    if (limit_motor_hours->GetValue()) {
      double motor_hours = 0.0;
      if (!Number(maximum_motor_hours, &motor_hours) || motor_hours <= 0.0 ||
          motor_hours >
              static_cast<double>(std::numeric_limits<uint32_t>::max()) /
                  3600.0) {
        status->SetLabel("Maximum propulsion time is outside the valid range");
        return;
      }
      request.maximum_motor_seconds =
          static_cast<uint32_t>(std::llround(motor_hours * 3600.0));
      request.limits_available |= 16;
    }
    if (limit_fuel->GetValue()) {
      if (!Number(maximum_fuel, &request.maximum_fuel_litres) ||
          request.maximum_fuel_litres <= 0.0 ||
          request.maximum_fuel_litres > 1000000.0) {
        status->SetLabel("Maximum fuel must be a positive number");
        return;
      }
      request.limits_available |= 64;
    }
  }
  request.maximum_search_angle_degrees = maximum_search_angle->GetValue();
  request.use_currents = use_currents->GetValue();
  request.require_current_data =
      use_currents->GetValue() && require_current_data->GetValue();
  request.use_waves = use_waves->GetValue();
  request.require_wave_data =
      use_waves->GetValue() && require_wave_data->GetValue();
  if (limit_true_wind->GetValue()) request.limits_available |= 1;
  if (use_waves->GetValue() && limit_waves->GetValue())
    request.limits_available |= 2;
  if (limit_apparent_wind->GetValue()) request.limits_available |= 4;
  if (use_currents->GetValue() &&
      limit_opposing_wind_current->GetValue())
    request.limits_available |= 8;
  const unsigned run_count =
      compare_departures->GetValue()
          ? static_cast<unsigned>(departure_window->GetValue() /
                                      departure_spacing->GetValue() +
                                  1)
          : 1;
  const int64_t departure_step_seconds =
      static_cast<int64_t>(departure_spacing->GetValue()) * 3600;
  const unsigned parallel_worker_limit = departure_workers->GetValue();
  std::vector<int64_t> departure_times;
  departure_times.reserve(run_count);
  for (unsigned run = 0; run < run_count; ++run)
    departure_times.push_back(request.departure_unix_time +
                              static_cast<int64_t>(run) *
                                  departure_step_seconds);
  SaveSettings();
  cancelled.store(false);
  calculate->Enable(false);
  cancel->Enable(true);
  if (manager_compute) manager_compute->Enable(false);
  if (manager_stop) manager_stop->Enable(true);
  if (manager_export) manager_export->Enable(false);
  vessel_performance_file->Enable(false);
  export_gpx->Enable(false);
  if (send_to_opencpn) send_to_opencpn->Enable(false);
  route.clear();
  route_environment.clear();
  alternative_routes.clear();
  isochrones.clear();
  traces.clear();
  departure_result_rows.clear();
  selected_departure_result = std::numeric_limits<size_t>::max();
  best_departure_result = std::numeric_limits<size_t>::max();
  if (departure_results) departure_results->DeleteAllItems();
  if (route_schedule) route_schedule->DeleteAllItems();
  if (validation_diagnostics)
    validation_diagnostics->SetValue("Calculation in progress…");
  gauge->SetValue(0);
  status->SetLabel("Checking iGRIB coverage for requested departures…");
  PopulateManagerRouting("Calculating");
  departure_runs.store(run_count);
  departures_completed.store(0);
  const PortableDepartureZone display_zone = SelectedDepartureZone();
  worker = std::thread([this, request, run_count, departure_step_seconds,
                        departure_times = std::move(departure_times),
                        parallel_worker_limit, selected_performance,
                        display_zone,
                        routing_gates = std::move(routing_gates)] {
    std::vector<DepartureResult> results(run_count);
    for (unsigned run = 0; run < run_count; ++run)
      results[run].requested_departure_unix_time = departure_times[run];
    std::vector<uint8_t> environmental_availability;
    wxString preflight_error;
    if (!preflight_environment(request.start_latitude, request.start_longitude,
                               departure_times, &environmental_availability,
                               &preflight_error) ||
        environmental_availability.size() != departure_times.size()) {
      if (preflight_error.empty())
        preflight_error =
            "environmental provider returned the wrong preflight batch size";
      for (auto& result : results)
        result.error = "Environmental preflight failed: " + preflight_error;
      departures_completed.store(run_count);
    } else {
      for (unsigned run = 0; run < run_count; ++run) {
        if ((environmental_availability[run] & 1) != 0) continue;
        results[run].error =
            "iGRIB has no wind data at the start position for departure " +
            FormatPortableDepartureTime(departure_times[run], display_zone);
        departures_completed.fetch_add(1);
      }
    }
    std::atomic<unsigned> next_departure{0};
    const unsigned parallelism = std::min(parallel_worker_limit, run_count);
    std::vector<std::thread> workers;
    workers.reserve(parallelism);
    for (unsigned worker_index = 0; worker_index < parallelism;
         ++worker_index) {
      workers.emplace_back([&, worker_index] {
        (void)worker_index;
        while (!cancelled.load()) {
          const unsigned run = next_departure.fetch_add(1);
          if (run >= run_count) break;
          auto& departure_result = results[run];
          if (!departure_result.error.empty()) continue;
          auto candidate_request = request;
          candidate_request.departure_unix_time +=
              static_cast<int64_t>(run) * departure_step_seconds;
          RoutingOutcome passage;
          passage.departure_unix_time = candidate_request.departure_unix_time;
          const int64_t passage_deadline =
              candidate_request.departure_unix_time +
              static_cast<int64_t>(candidate_request.max_hours) * 3600;
          bool ok = true;
          wxString leg_error;
          for (size_t leg_index = 0;
               leg_index + 1 < routing_gates.size() && ok; ++leg_index) {
            if (cancelled.load()) {
              leg_error = "Cancelled";
              ok = false;
              break;
            }
            candidate_request.start_latitude =
                routing_gates[leg_index].latitude;
            candidate_request.start_longitude =
                routing_gates[leg_index].longitude;
            candidate_request.destination_latitude =
                routing_gates[leg_index + 1].latitude;
            candidate_request.destination_longitude =
                routing_gates[leg_index + 1].longitude;
            if (!passage.points.empty())
              candidate_request.departure_unix_time =
                  passage.points.back().unix_time;
            const int64_t remaining_seconds =
                passage_deadline - candidate_request.departure_unix_time;
            if (remaining_seconds <= 0) {
              leg_error = "The overall passage duration limit was reached";
              ok = false;
              break;
            }
            candidate_request.max_hours = static_cast<uint32_t>(
                std::max<int64_t>(1, (remaining_seconds + 3599) / 3600));
            const uint32_t remaining_states =
                request.max_states > passage.states_examined
                    ? request.max_states - passage.states_examined
                    : 0;
            if (remaining_states == 0) {
              leg_error = "The overall route-state limit was reached";
              ok = false;
              break;
            }
            candidate_request.max_states = remaining_states;
            if (request.limits_available & 16) {
              if (passage.motor_seconds >= request.maximum_motor_seconds) {
                leg_error =
                    "The overall propulsion-time limit was reached";
                ok = false;
                break;
              }
              candidate_request.maximum_motor_seconds =
                  request.maximum_motor_seconds -
                  static_cast<uint32_t>(passage.motor_seconds);
            }
            if (request.limits_available & 64) {
              if (passage.estimated_fuel_litres >=
                  request.maximum_fuel_litres) {
                leg_error = "The overall fuel-use limit was reached";
                ok = false;
                break;
              }
              candidate_request.maximum_fuel_litres =
                  request.maximum_fuel_litres -
                  passage.estimated_fuel_litres;
            }
            RoutingOutcome leg;
            ok = CalculateRouteLeg(calculate_route, &candidate_request,
                                   *selected_performance, &leg, &leg_error) &&
                 AppendRouteLeg(&passage, std::move(leg), leg_index,
                                &leg_error);
            if (!ok)
              leg_error = wxString::Format("Leg %zu (%s to %s): %s",
                                           leg_index + 1,
                                           routing_gates[leg_index].name,
                                           routing_gates[leg_index + 1].name,
                                           leg_error);
          }
          if (!ok) {
            departure_result.error = leg_error;
            departures_completed.fetch_add(1);
            continue;
          }
          departure_result.outcome = std::move(passage);
          departure_result.success = true;
          departures_completed.fetch_add(1);
        }
      });
    }
    for (auto& route_worker : workers) route_worker.join();

    const auto live = alive;
    wxTheApp->CallAfter(
        [this, live, results = std::move(results),
         nominal_departure = request.departure_unix_time]() mutable {
          if (!live->load()) return;
          Finish(std::move(results), nominal_departure);
        });
  });
}

void PortableWeatherRoutingHost::Impl::Finish(
    std::vector<DepartureResult> results, int64_t nominal_departure) {
  if (worker.joinable()) worker.join();
  calculate->Enable(true);
  cancel->Enable(false);
  if (manager_compute) manager_compute->Enable(true);
  if (manager_stop) manager_stop->Enable(false);
  vessel_performance_file->Enable(true);
  departure_result_rows = std::move(results);
  nominal_departure_unix_time = nominal_departure;
  best_departure_result = std::numeric_limits<size_t>::max();
  uint64_t shortest_duration = std::numeric_limits<uint64_t>::max();
  wxString last_error = "No departure produced a route";
  for (size_t index = 0; index < departure_result_rows.size(); ++index) {
    const auto& result = departure_result_rows[index];
    if (!result.success) {
      if (!result.error.empty()) last_error = result.error;
      continue;
    }
    if (result.outcome.duration_seconds < shortest_duration) {
      shortest_duration = result.outcome.duration_seconds;
      best_departure_result = index;
    }
  }
  PopulateDepartureResults();
  if (best_departure_result == std::numeric_limits<size_t>::max()) {
    status->SetLabel("Failed: " + last_error);
    metrics->SetLabel("No successful departure route");
    PopulateManagerRouting("Failed");
    if (validation_diagnostics)
      validation_diagnostics->SetValue(last_error);
    if (notebook && departure_result_rows.size() > 1) notebook->SetSelection(3);
    return;
  }
  gauge->SetValue(100);
  SelectDepartureResult(best_departure_result);
  if (notebook && departure_result_rows.size() > 1) notebook->SetSelection(3);
}

void PortableWeatherRoutingHost::Impl::PopulateDepartureResults() {
  if (!departure_results) return;
  departure_results->DeleteAllItems();
  for (size_t index = 0; index < departure_result_rows.size(); ++index) {
    const auto& result = departure_result_rows[index];
    const int64_t departure = result.requested_departure_unix_time;
    const long row = departure_results->InsertItem(
        departure_results->GetItemCount(),
        index == best_departure_result ? "Best" : wxString());
    departure_results->SetItemData(row, static_cast<long>(index));
    departure_results->SetItem(
        row, 1, FormatOffset(departure - nominal_departure_unix_time));
    departure_results->SetItem(row, 2, FormatRoutingTime(departure));
    if (result.success) {
      const auto& outcome = result.outcome;
      departure_results->SetItem(
          row, 3,
          FormatRoutingTime(outcome.departure_unix_time +
                            static_cast<int64_t>(outcome.duration_seconds)));
      departure_results->SetItem(row, 4,
                                 FormatElapsed(outcome.duration_seconds));
      departure_results->SetItem(
          row, 5, wxString::Format("%.1f NM", outcome.distance_nautical_miles));
      departure_results->SetItem(
          row, 6, wxString::Format("%.1f kt", outcome.average_speed_knots));
      departure_results->SetItem(
          row, 7, wxString::Format("%.1f kt", outcome.average_sog_knots));
      departure_results->SetItem(
          row, 8, wxString::Format("%.1f kt", outcome.maximum_sog_knots));
      departure_results->SetItem(
          row, 9, wxString::Format("%.1f kt", outcome.average_wind_knots));
      departure_results->SetItem(
          row, 10, wxString::Format("%.1f kt", outcome.maximum_wind_knots));
      departure_results->SetItem(
          row, 11,
          outcome.current_metrics_available
              ? wxString::Format("%.1f kt", outcome.average_current_knots)
              : "N/A");
      departure_results->SetItem(
          row, 12,
          outcome.current_metrics_available
              ? wxString::Format("%.1f kt", outcome.maximum_current_knots)
              : "N/A");
      departure_results->SetItem(row, 13,
                                 wxString::Format("%u", outcome.tacks));
      departure_results->SetItem(
          row, 14,
          outcome.fuel_metrics_available
              ? FormatElapsed(outcome.motor_seconds)
              : "N/A");
      departure_results->SetItem(
          row, 15,
          outcome.fuel_metrics_available
              ? wxString::Format("%.1f L", outcome.estimated_fuel_litres)
              : "N/A");
      departure_results->SetItem(
          row, 16,
          outcome.fuel_metrics_available
              ? wxString::Format("%u", outcome.propulsion_transitions)
              : "N/A");
      const wxString comfort = outcome.comfort_level == 1   ? "Good"
                               : outcome.comfort_level == 2 ? "Bumpy"
                               : outcome.comfort_level == 3 ? "Difficult"
                                                            : "N/A";
      departure_results->SetItem(row, 17, comfort);
      departure_results->SetItem(
          row, 18, wxString::Format("%u", outcome.states_examined));
      departure_results->SetItem(
          row, 19, wxString::Format("%zu", outcome.isochrones.size()));
      departure_results->SetItem(row, 20, "Complete");
    } else {
      for (int column = 3; column <= 19; ++column)
        departure_results->SetItem(row, column, "N/A");
      departure_results->SetItem(
          row, 20, result.error.empty() ? "Failed" : result.error);
    }
  }
  for (int column = 0; column < 21; ++column)
    departure_results->SetColumnWidth(column, wxLIST_AUTOSIZE_USEHEADER);
}

void PortableWeatherRoutingHost::Impl::SelectDepartureResult(size_t index) {
  if (index >= departure_result_rows.size() ||
      !departure_result_rows[index].success)
    return;
  selected_departure_result = index;
  const auto& selected = departure_result_rows[index].outcome;
  route = selected.points;
  route_environment = selected.route_environment;
  isochrones = selected.isochrones;
  traces = selected.traces;
  alternative_routes.clear();
  for (size_t candidate = 0; candidate < departure_result_rows.size();
       ++candidate) {
    if (candidate != index && departure_result_rows[candidate].success)
      alternative_routes.push_back(
          departure_result_rows[candidate].outcome.points);
  }
  status->SetLabel(selected.diagnostic);
  wxString propulsion_metrics;
  if (selected.fuel_metrics_available) {
    propulsion_metrics = wxString::Format(
        " · propulsion %s · %.1f L · %u mode changes",
        FormatElapsed(selected.motor_seconds),
        selected.estimated_fuel_litres, selected.propulsion_transitions);
  }
  metrics->SetLabel(wxString::Format(
      "%s%s · %zu points · %zu isochrones · %.1f NM · %.1f hours · %u "
      "states · departure %s%s",
      index == best_departure_result ? "Best passage · "
                                     : "Selected passage · ",
      FormatElapsed(selected.duration_seconds), route.size(), isochrones.size(),
      selected.distance_nautical_miles, selected.duration_seconds / 3600.0,
      selected.states_examined, FormatRoutingTime(selected.departure_unix_time),
      propulsion_metrics));
  export_gpx->Enable(!route.empty());
  if (send_to_opencpn) send_to_opencpn->Enable(!route.empty());
  if (manager_export) manager_export->Enable(!route.empty());
  PopulateManagerRouting(index == best_departure_result ? "Best" : "Selected");
  if (departure_results) {
    updating_departure_selection = true;
    for (long row = 0; row < departure_results->GetItemCount(); ++row) {
      const bool chosen =
          departure_results->GetItemData(row) == static_cast<wxUIntPtr>(index);
      departure_results->SetItemState(row, chosen ? wxLIST_STATE_SELECTED : 0,
                                      wxLIST_STATE_SELECTED);
      if (chosen) departure_results->EnsureVisible(row);
    }
    updating_departure_selection = false;
  }
  if (validation_diagnostics) {
    validation_diagnostics->SetValue(
        selected.diagnostic +
        "\nHost validation: route geometry, environmental profile and "
        "batched chart-safety result accepted. The route remains a planning "
        "output requiring navigator review.");
  }
  if (route_schedule) {
    route_schedule->DeleteAllItems();
    constexpr size_t kMaximumScheduleRows = 2000;
    const size_t stride =
        std::max<size_t>(1, (route.size() + kMaximumScheduleRows - 1) /
                                kMaximumScheduleRows);
    for (size_t point_index = 0; point_index < route.size();
         point_index += stride) {
      const auto& point = route[point_index];
      const auto& next = route[std::min(point_index + stride,
                                        route.size() - 1)];
      const int64_t seconds = std::max<int64_t>(1, next.unix_time -
                                                      point.unix_time);
      const double course =
          InitialBearingDegrees(point.latitude, point.longitude,
                                next.latitude, next.longitude);
      const double sog =
          next.unix_time > point.unix_time
              ? GreatCircleNauticalMiles(point.latitude, point.longitude,
                                         next.latitude, next.longitude) *
                    3600.0 / static_cast<double>(seconds)
              : 0.0;
      const long row = route_schedule->InsertItem(
          route_schedule->GetItemCount(), FormatRoutingTime(point.unix_time));
      route_schedule->SetItem(
          row, 1, wxString::Format("%.5f", point.latitude));
      route_schedule->SetItem(
          row, 2, wxString::Format("%.5f", point.longitude));
      route_schedule->SetItem(row, 3,
                              wxString::Format("%03.0f°", course));
      route_schedule->SetItem(row, 4,
                              wxString::Format("%.1f kt", sog));
      if (point_index < route_environment.size()) {
        const auto& environment = route_environment[point_index];
        const double wind_speed =
            std::hypot(environment.wind_u_knots, environment.wind_v_knots);
        const double wind_from = VectorBearingDegrees(
            -environment.wind_u_knots, -environment.wind_v_knots);
        route_schedule->SetItem(
            row, 5,
            wxString::Format("%.1f kt %03.0f° from", wind_speed, wind_from));
        route_schedule->SetItem(
            row, 6,
            (environment.available & 2)
                ? wxString::Format("%.1f m", environment.wave_height_metres)
                : "N/A");
        if (environment.available & 1) {
          const double current_speed = std::hypot(
              environment.current_u_knots, environment.current_v_knots);
          const double current_toward = VectorBearingDegrees(
              environment.current_u_knots, environment.current_v_knots);
          route_schedule->SetItem(
              row, 7,
              wxString::Format("%.1f kt %03.0f°", current_speed,
                               current_toward));
        } else {
          route_schedule->SetItem(row, 7, "N/A");
        }
      } else {
        route_schedule->SetItem(row, 5, "N/A");
        route_schedule->SetItem(row, 6, "N/A");
        route_schedule->SetItem(row, 7, "N/A");
      }
      route_schedule->SetItem(row, 8, "iGRIB immutable dataset");
    }
    if (!route.empty() && (route.size() - 1) % stride != 0) {
      const auto& point = route.back();
      const long row = route_schedule->InsertItem(
          route_schedule->GetItemCount(), FormatRoutingTime(point.unix_time));
      route_schedule->SetItem(row, 1,
                              wxString::Format("%.5f", point.latitude));
      route_schedule->SetItem(row, 2,
                              wxString::Format("%.5f", point.longitude));
      for (int column = 3; column <= 7; ++column)
        route_schedule->SetItem(row, column, "—");
      route_schedule->SetItem(row, 8, "Arrival");
    }
    for (int column = 0; column < 9; ++column)
      route_schedule->SetColumnWidth(column, wxLIST_AUTOSIZE_USEHEADER);
  }
  if (parent) parent->Refresh();
  RequestRefresh(GetOCPNCanvasWindow());
}

void PortableWeatherRoutingHost::Impl::ReportProgress(unsigned percent,
                                                      const wxString& message) {
  if (!frame || stopped.load()) return;
  const unsigned runs = std::max(1u, departure_runs.load());
  const unsigned completed = std::min(departures_completed.load(), runs);
  const unsigned overall =
      runs > 1 ? std::min(99u, completed * 100 / runs) : percent;
  const wxString labelled =
      runs > 1 ? wxString::Format("Parallel departures — %u/%u complete — %s",
                                  completed, runs, message)
               : message;
  const auto live = alive;
  wxTheApp->CallAfter([this, live, overall, labelled] {
    if (!live->load() || !frame || stopped.load()) return;
    gauge->SetValue(overall);
    status->SetLabel(labelled);
  });
}

bool PortableWeatherRoutingHost::Impl::Render(wxDC& dc,
                                              PlugIn_ViewPort* viewport) {
  if (!viewport || route.size() < 2) return false;
  if (show_stability_corridor && show_stability_corridor->GetValue() &&
      !alternative_routes.empty()) {
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(wxColour(220, 80, 190, 45)));
    constexpr size_t kCorridorSamples = 48;
    for (const auto& alternative : alternative_routes) {
      if (alternative.size() < 2) continue;
      std::vector<wxPoint> corridor;
      corridor.reserve(kCorridorSamples * 2);
      for (size_t index = 0; index < kCorridorSamples; ++index)
        corridor.push_back(SampleRouteFraction(
            route, static_cast<double>(index) / (kCorridorSamples - 1),
            viewport));
      for (size_t index = kCorridorSamples; index-- > 0;)
        corridor.push_back(SampleRouteFraction(
            alternative, static_cast<double>(index) / (kCorridorSamples - 1),
            viewport));
      dc.DrawPolygon(static_cast<int>(corridor.size()), corridor.data());
    }
  }
  if (show_isochrones && show_isochrones->GetValue()) {
    dc.SetPen(wxPen(wxColour(225, 105, 195, 175), 2));
    for (const auto& contour : isochrones) {
      if (contour.points.size() < 2) continue;
      std::vector<wxPoint> line;
      line.reserve(contour.points.size());
      for (const auto& point : contour.points)
        line.push_back(Project(viewport, point.latitude, point.longitude));
      dc.DrawLines(static_cast<int>(line.size()), line.data());
    }
  }
  dc.SetPen(wxPen(wxColour(190, 120, 180), 2));
  for (const auto& alternative : alternative_routes) {
    if (alternative.size() < 2) continue;
    std::vector<wxPoint> comparison;
    comparison.reserve(alternative.size());
    for (const auto& point : alternative)
      comparison.push_back(
          Project(viewport, point.latitude, point.longitude));
    dc.DrawLines(static_cast<int>(comparison.size()), comparison.data());
  }
  std::vector<wxPoint> points;
  points.reserve(route.size());
  for (const auto& point : route)
    points.push_back(Project(viewport, point.latitude, point.longitude));
  dc.SetPen(wxPen(wxColour(255, 255, 255, 220), 7));
  dc.DrawLines(static_cast<int>(points.size()), points.data());
  dc.SetPen(wxPen(wxColour(235, 45, 175), 4));
  dc.DrawLines(static_cast<int>(points.size()), points.data());

  if (show_route_wind && show_route_wind->GetValue()) {
    wxPoint previous(std::numeric_limits<int>::min(),
                     std::numeric_limits<int>::min());
    for (const auto& environment : route_environment) {
      const wxPoint origin =
          Project(viewport, environment.latitude, environment.longitude);
      const long dx = static_cast<long>(origin.x) - previous.x;
      const long dy = static_cast<long>(origin.y) - previous.y;
      if (previous.x != std::numeric_limits<int>::min() &&
          dx * dx + dy * dy < 45L * 45L)
        continue;
      DrawRouteWindBarb(dc, origin, environment.wind_u_knots,
                        environment.wind_v_knots, wxColour(185, 20, 155));
      previous = origin;
    }
  }

  if (route_to_cursor && route_to_cursor->GetValue() && cursor_position) {
    PortableNavigationPosition cursor;
    if (cursor_position(&cursor)) {
      const wxPoint cursor_pixel =
          Project(viewport, cursor.latitude, cursor.longitude);
      const RoutingOutcome::InspectionLine* closest = nullptr;
      long closest_squared = 32L * 32L;
      for (const auto& trace : traces) {
        if (trace.points.size() < 2) continue;
        const auto& endpoint = trace.points.back();
        const wxPoint pixel =
            Project(viewport, endpoint.latitude, endpoint.longitude);
        const long dx = pixel.x - cursor_pixel.x;
        const long dy = pixel.y - cursor_pixel.y;
        const long squared = dx * dx + dy * dy;
        if (squared <= closest_squared) {
          closest_squared = squared;
          closest = &trace;
        }
      }
      if (closest) {
        std::vector<wxPoint> inspection;
        inspection.reserve(closest->points.size());
        for (const auto& point : closest->points)
          inspection.push_back(
              Project(viewport, point.latitude, point.longitude));
        dc.SetPen(
            wxPen(wxColour(255, 255, 255, 220), 6, wxPENSTYLE_SHORT_DASH));
        dc.DrawLines(static_cast<int>(inspection.size()), inspection.data());
        dc.SetPen(wxPen(wxColour(135, 25, 150), 3, wxPENSTYLE_SHORT_DASH));
        dc.DrawLines(static_cast<int>(inspection.size()), inspection.data());
        dc.SetBrush(wxBrush(wxColour(135, 25, 150)));
        dc.DrawCircle(inspection.back(), 5);
      }
    }
  }

  if (boat_at_grib_time && boat_at_grib_time->GetValue() &&
      displayed_environment_time) {
    int64_t display_time = 0;
    double boat_latitude = 0.0;
    double boat_longitude = 0.0;
    if (displayed_environment_time(&display_time) &&
        InterpolateRoutePosition(route, display_time, &boat_latitude,
                                 &boat_longitude)) {
      const wxPoint boat =
          Project(viewport, boat_latitude, boat_longitude);
      dc.SetPen(wxPen(wxColour(255, 255, 255), 3));
      dc.SetBrush(wxBrush(wxColour(235, 45, 175)));
      dc.DrawCircle(boat, 8);
      dc.SetPen(wxPen(wxColour(80, 20, 80), 1));
      dc.DrawCircle(boat, 4);
    }
  }
  return true;
}

bool PortableWeatherRoutingHost::Impl::RenderGL(PlugIn_ViewPort* viewport) {
  if (!viewport || route.size() < 2) return false;
  auto vertex = [viewport](double latitude, double longitude) {
    const wxPoint pixel = Project(viewport, latitude, longitude);
    glVertex2i(pixel.x, pixel.y);
  };
  auto line = [&vertex](const std::vector<ocpn_portable_route_point>& points,
                        GLubyte red, GLubyte green, GLubyte blue,
                        GLubyte alpha, GLfloat width) {
    if (points.size() < 2) return;
    glColor4ub(red, green, blue, alpha);
    glLineWidth(width);
    glBegin(GL_LINE_STRIP);
    for (const auto& point : points)
      vertex(point.latitude, point.longitude);
    glEnd();
  };

  glPushAttrib(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT | GL_LINE_BIT |
               GL_CURRENT_BIT);
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  if (show_stability_corridor && show_stability_corridor->GetValue()) {
    glColor4ub(220, 80, 190, 45);
    constexpr std::size_t kSamples = 48;
    for (const auto& alternative : alternative_routes) {
      if (alternative.size() < 2) continue;
      glBegin(GL_TRIANGLE_STRIP);
      for (std::size_t index = 0; index < kSamples; ++index) {
        const double fraction =
            static_cast<double>(index) / static_cast<double>(kSamples - 1);
        const wxPoint primary = SampleRouteFraction(route, fraction, viewport);
        const wxPoint comparison =
            SampleRouteFraction(alternative, fraction, viewport);
        glVertex2i(primary.x, primary.y);
        glVertex2i(comparison.x, comparison.y);
      }
      glEnd();
    }
  }
  if (show_isochrones && show_isochrones->GetValue()) {
    for (const auto& contour : isochrones)
      line(contour.points, 225, 105, 195, 175, 2.0F);
  }
  for (const auto& alternative : alternative_routes)
    line(alternative, 190, 120, 180, 210, 2.0F);
  line(route, 255, 255, 255, 220, 7.0F);
  line(route, 235, 45, 175, 255, 4.0F);

  if (show_route_wind && show_route_wind->GetValue()) {
    wxPoint previous(std::numeric_limits<int>::min(),
                     std::numeric_limits<int>::min());
    glColor4ub(185, 20, 155, 255);
    glLineWidth(2.0F);
    glBegin(GL_LINES);
    for (const auto& environment : route_environment) {
      const double speed =
          std::hypot(environment.wind_u_knots, environment.wind_v_knots);
      if (!std::isfinite(speed) || speed < 2.5) continue;
      const wxPoint origin =
          Project(viewport, environment.latitude, environment.longitude);
      const long dx = static_cast<long>(origin.x) - previous.x;
      const long dy = static_cast<long>(origin.y) - previous.y;
      if (previous.x != std::numeric_limits<int>::min() &&
          dx * dx + dy * dy < 45L * 45L)
        continue;
      const double staff_x = -environment.wind_u_knots / speed;
      const double staff_y = environment.wind_v_knots / speed;
      glVertex2i(origin.x, origin.y);
      glVertex2i(origin.x + static_cast<int>(std::lround(staff_x * 24.0)),
                 origin.y +
                     static_cast<int>(std::lround(staff_y * 24.0)));
      previous = origin;
    }
    glEnd();
  }

  if (route_to_cursor && route_to_cursor->GetValue() && cursor_position) {
    PortableNavigationPosition cursor;
    if (cursor_position(&cursor)) {
      const wxPoint cursor_pixel =
          Project(viewport, cursor.latitude, cursor.longitude);
      const RoutingOutcome::InspectionLine* closest = nullptr;
      long closest_squared = 32L * 32L;
      for (const auto& trace : traces) {
        if (trace.points.size() < 2) continue;
        const auto& endpoint = trace.points.back();
        const wxPoint pixel =
            Project(viewport, endpoint.latitude, endpoint.longitude);
        const long dx = pixel.x - cursor_pixel.x;
        const long dy = pixel.y - cursor_pixel.y;
        const long squared = dx * dx + dy * dy;
        if (squared <= closest_squared) {
          closest_squared = squared;
          closest = &trace;
        }
      }
      if (closest) line(closest->points, 135, 25, 150, 255, 3.0F);
    }
  }

  if (boat_at_grib_time && boat_at_grib_time->GetValue() &&
      displayed_environment_time) {
    std::int64_t display_time = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    if (displayed_environment_time(&display_time) &&
        InterpolateRoutePosition(route, display_time, &latitude, &longitude)) {
      const wxPoint centre = Project(viewport, latitude, longitude);
      glColor4ub(235, 45, 175, 255);
      glBegin(GL_TRIANGLE_FAN);
      glVertex2i(centre.x, centre.y);
      for (int index = 0; index <= 20; ++index) {
        const double angle =
            static_cast<double>(index) * 2.0 * 3.14159265358979323846 / 20.0;
        glVertex2i(
            centre.x + static_cast<int>(std::lround(std::cos(angle) * 8)),
            centre.y + static_cast<int>(std::lround(std::sin(angle) * 8)));
      }
      glEnd();
    }
  }
  glPopAttrib();
  return true;
}

void PortableWeatherRoutingHost::Impl::ExportGpx() {
  if (route.empty()) return;
  wxString filename = plugin_id.AfterLast('.') + "-route.gpx";
  wxFileDialog dialog(frame, "Export portable weather route", wxEmptyString,
                      filename, "GPX files (*.gpx)|*.gpx",
                      wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
  if (dialog.ShowModal() != wxID_OK) return;
  wxFileOutputStream file(dialog.GetPath());
  if (!file.IsOk()) return;
  wxTextOutputStream out(file);
  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" "
         "creator=\"OpenCPN portable routing runtime\" "
         "xmlns=\"http://www.topografix.com/GPX/1/"
         "1\"><rte><name>Portable weather-routing passage</name>\n";
  for (const auto& point : route)
    out << wxString::Format(
        "<rtept lat=\"%.8f\" lon=\"%.8f\"><time>%s</time></rtept>\n",
        point.latitude, point.longitude,
        wxDateTime(static_cast<time_t>(point.unix_time))
                .ToUTC()
                .FormatISOCombined('T') +
            "Z");
  out << "</rte></gpx>\n";
  status->SetLabel("GPX route exported");
}

void PortableWeatherRoutingHost::Impl::SendToOpenCpn() {
  if (route.size() < 2 || !create_route) return;
  constexpr size_t kMaximumOpenCpnRoutePoints = 2000;
  const size_t stride =
      std::max<size_t>(1, (route.size() + kMaximumOpenCpnRoutePoints - 1) /
                              kMaximumOpenCpnRoutePoints);
  std::vector<PortableNavigationPosition> points;
  points.reserve(std::min(route.size(), kMaximumOpenCpnRoutePoints) + 1);
  for (size_t index = 0; index < route.size(); index += stride) {
    const auto& point = route[index];
    points.push_back({plugin_id + wxString::Format(":%zu", index),
                      index == 0 ? "Route start"
                                 : wxString::Format("Route point %zu", index),
                      point.latitude, point.longitude});
  }
  if (points.back().latitude != route.back().latitude ||
      points.back().longitude != route.back().longitude)
    points.push_back({plugin_id + ":destination", "Route destination",
                      route.back().latitude, route.back().longitude});
  points.front().name = "Route start";
  points.back().name = "Route destination";
  wxString error;
  const wxString name =
      surface_title + " " + FormatRoutingTime(route.front().unix_time);
  if (!create_route(name, points, &error)) {
    status->SetLabel("Could not add route to OpenCPN: " + error);
    return;
  }
  status->SetLabel(
      "Selected planning route added to OpenCPN; review it before use");
  RefreshNavigationPositions(false);
  PopulateManagerPositions();
}

void PortableWeatherRoutingHost::Impl::Shutdown() {
  if (stopped.exchange(true)) return;
  alive->store(false);
  cancelled.store(true);
  if (cancel_routes) cancel_routes();
  environment_refresh_timer.Stop();
  if (worker.joinable()) worker.join();
  if (frame) {
    SaveSettings();
    frame->Destroy();
    frame = nullptr;
  }
}

PortableWeatherRoutingHost::PortableWeatherRoutingHost(
    wxWindow* parent, wxFileConfig* config, CalculateRoute calculate_route,
    std::function<void()> cancel_routes, const wxString& package_root,
    const wxString& plugin_id, const wxString& surface_resource,
    std::function<wxString()> summary,
    std::function<std::vector<PortableNavigationPosition>()> list_waypoints,
    std::function<std::vector<PortableNavigationRoute>()> list_routes,
    std::function<bool(const wxString&,
                       const std::vector<PortableNavigationPosition>&,
                       wxString*)>
        create_route,
    std::function<bool(PortableNavigationPosition*)> vessel_position,
    std::function<bool(PortableNavigationPosition*)> cursor_position,
    std::function<bool(int64_t*)> displayed_environment_time,
    std::function<bool(double, double, const std::vector<int64_t>&,
                       std::vector<uint8_t>*, wxString*)>
        preflight_environment,
    double latitude, double longitude)
    : m_impl(std::make_unique<Impl>(
          parent, config, std::move(calculate_route), std::move(cancel_routes),
          package_root, plugin_id, surface_resource,
          std::move(summary), std::move(list_waypoints),
          std::move(list_routes),
          std::move(create_route),
          std::move(vessel_position), std::move(cursor_position),
          std::move(displayed_environment_time),
          std::move(preflight_environment), latitude, longitude)) {}
PortableWeatherRoutingHost::~PortableWeatherRoutingHost() = default;
bool PortableWeatherRoutingHost::Show(wxString* error) {
  return m_impl->Show(error);
}
bool PortableWeatherRoutingHost::Render(wxDC& dc,
                                        PlugIn_ViewPort* viewport) {
  return m_impl->Render(dc, viewport);
}
bool PortableWeatherRoutingHost::RenderGL(PlugIn_ViewPort* viewport) {
  return m_impl->RenderGL(viewport);
}
void PortableWeatherRoutingHost::ReportProgress(unsigned percent,
                                                const wxString& message) {
  m_impl->ReportProgress(percent, message);
}
bool PortableWeatherRoutingHost::Cancelled() const {
  return m_impl->Cancelled();
}
void PortableWeatherRoutingHost::Shutdown() { m_impl->Shutdown(); }
