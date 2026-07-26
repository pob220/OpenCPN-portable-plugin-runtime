#include "weather_routing_host.h"

#include "window_activation.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
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
#include <wx/choicdlg.h>
#include <wx/collpane.h>
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
#include <wx/slider.h>
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
#include "chart_safety_service.h"
#include "portable_polar.h"
#include "portable_ui_menu.h"
#include "weather_routing_departure_coordinator.h"
#include "weather_routing_display.h"
#include "weather_routing_stability.h"
#include "wind_barb_geometry.h"

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
                   value.IsValid() ? value.Format("%H:%M") : "00:00", position,
                   size, wxTE_PROCESS_ENTER) {
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
  const std::set<wxString> required_controls = {
      "start-latitude",
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
      "require-authoritative-chart-safety",
      "chart-safety-status",
      "minimum-chart-depth",
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
      "reset-advanced",
      "route-metrics",
      "departure-results",
      "candidate-details",
      "route-schedule",
      "validation-diagnostics",
      "show-isochrones",
      "isochrone-display-preset",
      "isochrone-display-settings",
      "show-stability-corridor",
      "stability-corridor-settings",
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
      const int expected = id == "departure-results" ? 7
                           : id == "route-schedule"  ? 9
                                                     : 1;
      if (!columns.IsArray() || columns.Size() != expected) {
        if (error)
          *error =
              "portable weather-routing table has incompatible columns: " + id;
        return false;
      }
      for (int column = 0; column < columns.Size(); ++column) {
        if (!columns[column].IsString() || columns[column].AsString().empty()) {
          if (error)
            *error =
                "portable weather-routing table has an invalid column: " + id;
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
  std::vector<ppm::RoutingPassageLeg> passage_legs;
  uint64_t validation_samples = 0;
};

enum class DepartureCandidateStatus {
  kQueued,
  kRunning,
  kCompleted,
  kFailed,
  kCancelled,
};

struct DepartureResult {
  RoutingOutcome outcome;
  wxString error;
  int64_t requested_departure_unix_time = 0;
  DepartureCandidateStatus status = DepartureCandidateStatus::kQueued;
  unsigned percent = 0;
  wxString stage = "Queued";
  bool success = false;
};

bool CalculateContinuousPassage(
    const PortableWeatherRoutingHost::CalculatePassage& calculate_passage,
    const ocpn_portable_route_request& request,
    const PortablePolarSet& performance,
    const std::vector<PortableNavigationPosition>& gates,
    int64_t departure_offset_seconds, ppm::RoutingOutcome* outcome,
    std::string* failure) {
  if (!calculate_passage || !outcome || gates.size() < 2) {
    if (failure) *failure = "invalid continuous passage calculation request";
    return false;
  }
  ppm::RoutingPassageRequest portable_request;
  portable_request.route.parameters = request;
  portable_request.route.parameters.polars = nullptr;
  portable_request.route.parameters.polar_count = 0;
  portable_request.route.polars.reserve(performance.grids.size());
  for (const auto& grid : performance.grids) {
    portable_request.route.polars.push_back(
        {grid.identity, grid.true_wind_speeds_knots,
         grid.true_wind_angles_degrees, grid.boat_speeds_knots});
  }
  portable_request.gates.reserve(gates.size());
  for (const auto& gate : gates) {
    portable_request.gates.push_back({gate.id.ToStdString(),
                                      gate.name.ToStdString(), gate.latitude,
                                      gate.longitude});
  }
  portable_request.departure_offset_seconds = departure_offset_seconds;
  if (!calculate_passage(std::move(portable_request), outcome, failure))
    return false;
  outcome->departure_unix_time = request.departure_unix_time;
  return true;
}

RoutingOutcome HostOutcome(ppm::RoutingOutcome outcome) {
  RoutingOutcome converted;
  converted.points = std::move(outcome.points);
  converted.route_environment = std::move(outcome.route_environment);
  converted.isochrones.reserve(outcome.isochrones.size());
  for (auto& line : outcome.isochrones)
    converted.isochrones.push_back({line.unix_time, std::move(line.points)});
  converted.traces.reserve(outcome.traces.size());
  for (auto& line : outcome.traces)
    converted.traces.push_back({line.unix_time, std::move(line.points)});
  converted.diagnostic = wxString::FromUTF8(outcome.diagnostic);
  converted.distance_nautical_miles = outcome.distance_nautical_miles;
  converted.duration_seconds = outcome.duration_seconds;
  converted.states_examined = outcome.states_examined;
  converted.average_speed_knots = outcome.average_speed_knots;
  converted.maximum_speed_knots = outcome.maximum_speed_knots;
  converted.average_sog_knots = outcome.average_sog_knots;
  converted.maximum_sog_knots = outcome.maximum_sog_knots;
  converted.average_wind_knots = outcome.average_wind_knots;
  converted.maximum_wind_knots = outcome.maximum_wind_knots;
  converted.average_current_knots = outcome.average_current_knots;
  converted.maximum_current_knots = outcome.maximum_current_knots;
  converted.tacks = outcome.tacks;
  converted.motor_seconds = outcome.motor_seconds;
  converted.estimated_fuel_litres = outcome.estimated_fuel_litres;
  converted.propulsion_transitions = outcome.propulsion_transitions;
  converted.comfort_level = outcome.comfort_level;
  converted.current_metrics_available = outcome.metrics_available & 1;
  converted.fuel_metrics_available = outcome.metrics_available & 2;
  converted.departure_unix_time = outcome.departure_unix_time;
  converted.passage_legs = std::move(outcome.passage_legs);
  converted.validation_samples = outcome.validation_samples;
  return converted;
}

DepartureCandidateStatus HostCandidateStatus(
    ppm::DepartureCandidateState state) {
  switch (state) {
    case ppm::DepartureCandidateState::kCompleted:
      return DepartureCandidateStatus::kCompleted;
    case ppm::DepartureCandidateState::kFailed:
      return DepartureCandidateStatus::kFailed;
    case ppm::DepartureCandidateState::kCancelled:
      return DepartureCandidateStatus::kCancelled;
    case ppm::DepartureCandidateState::kRunning:
      return DepartureCandidateStatus::kRunning;
    case ppm::DepartureCandidateState::kPreflight:
    case ppm::DepartureCandidateState::kQueued:
    default:
      return DepartureCandidateStatus::kQueued;
  }
}

thread_local const ppm::DepartureCandidateProgress* active_departure_progress =
    nullptr;

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

wxString XmlEscape(wxString value) {
  value.Replace("&", "&amp;");
  value.Replace("<", "&lt;");
  value.Replace(">", "&gt;");
  value.Replace("\"", "&quot;");
  value.Replace("'", "&apos;");
  return value;
}

bool IsBetterDeparture(const DepartureResult& candidate,
                       const DepartureResult& incumbent,
                       int64_t nominal_departure) {
  const auto& lhs = candidate.outcome;
  const auto& rhs = incumbent.outcome;
  if (lhs.duration_seconds != rhs.duration_seconds)
    return lhs.duration_seconds < rhs.duration_seconds;
  const int64_t lhs_eta =
      lhs.departure_unix_time + static_cast<int64_t>(lhs.duration_seconds);
  const int64_t rhs_eta =
      rhs.departure_unix_time + static_cast<int64_t>(rhs.duration_seconds);
  if (lhs_eta != rhs_eta) return lhs_eta < rhs_eta;
  if (lhs.motor_seconds != rhs.motor_seconds)
    return lhs.motor_seconds < rhs.motor_seconds;
  if (lhs.estimated_fuel_litres != rhs.estimated_fuel_litres)
    return lhs.estimated_fuel_litres < rhs.estimated_fuel_litres;
  const uint64_t lhs_manoeuvres =
      static_cast<uint64_t>(lhs.tacks) + lhs.propulsion_transitions;
  const uint64_t rhs_manoeuvres =
      static_cast<uint64_t>(rhs.tacks) + rhs.propulsion_transitions;
  if (lhs_manoeuvres != rhs_manoeuvres) return lhs_manoeuvres < rhs_manoeuvres;
  const int64_t lhs_offset =
      std::abs(candidate.requested_departure_unix_time - nominal_departure);
  const int64_t rhs_offset =
      std::abs(incumbent.requested_departure_unix_time - nominal_departure);
  if (lhs_offset != rhs_offset) return lhs_offset < rhs_offset;
  return candidate.requested_departure_unix_time <
         incumbent.requested_departure_unix_time;
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
  double bearing = std::atan2(east, north) * 180.0 / 3.14159265358979323846;
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
                   std::cos(phi1) * std::cos(phi2) * std::sin(dlon / 2.0) *
                       std::sin(dlon / 2.0);
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
      route[lower].latitude +
      (route[upper].latitude - route[lower].latitude) * part;
  const double longitude_delta =
      std::fmod(route[upper].longitude - route[lower].longitude + 540.0,
                360.0) -
      180.0;
  const double longitude =
      std::fmod(route[lower].longitude + longitude_delta * part + 540.0,
                360.0) -
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

double ProjectedRoutePixels(const std::vector<ocpn_portable_route_point>& route,
                            PlugIn_ViewPort* viewport) {
  double result = 0.0;
  std::optional<wxPoint> previous;
  for (const auto& point : route) {
    const wxPoint pixel = Project(viewport, point.latitude, point.longitude);
    if (previous)
      result += std::hypot(static_cast<double>(pixel.x - previous->x),
                           static_cast<double>(pixel.y - previous->y));
    previous = pixel;
  }
  return result;
}

struct IsochronePalette {
  wxColour minor;
  wxColour major;
  wxColour highlighted;
  wxColour route;
  wxColour route_halo;
  wxColour cursor;
  wxColour label_text;
  wxColour label_background;
};

IsochronePalette PaletteForScheme(int colour_scheme) {
  if (colour_scheme == PI_GLOBAL_COLOR_SCHEME_NIGHT) {
    return {wxColour(105, 58, 82),   wxColour(155, 60, 92),
            wxColour(225, 112, 45),  wxColour(195, 42, 92),
            wxColour(35, 12, 24),    wxColour(70, 145, 190),
            wxColour(235, 190, 180), wxColour(45, 15, 25)};
  }
  if (colour_scheme == PI_GLOBAL_COLOR_SCHEME_DUSK) {
    return {wxColour(90, 85, 135),   wxColour(175, 75, 145),
            wxColour(245, 155, 45),  wxColour(220, 42, 145),
            wxColour(55, 35, 55),    wxColour(45, 155, 205),
            wxColour(250, 235, 225), wxColour(55, 40, 58)};
  }
  return {wxColour(85, 105, 150),  wxColour(185, 70, 160),
          wxColour(255, 178, 35),  wxColour(235, 45, 175),
          wxColour(255, 255, 255), wxColour(30, 145, 220),
          wxColour(35, 35, 45),    wxColour(250, 250, 245)};
}

wxColour BlendColour(const wxColour& start, const wxColour& end,
                     double fraction, unsigned char alpha) {
  const double clamped = std::clamp(fraction, 0.0, 1.0);
  const auto channel = [clamped](unsigned char left, unsigned char right) {
    return static_cast<unsigned char>(std::lround(
        static_cast<double>(left) +
        (static_cast<double>(right) - static_cast<double>(left)) * clamped));
  };
  return wxColour(channel(start.Red(), end.Red()),
                  channel(start.Green(), end.Green()),
                  channel(start.Blue(), end.Blue()), alpha);
}

wxColour IsochroneColour(const IsochronePalette& palette,
                         const ppm::IsochroneDisplaySettings& settings,
                         const ppm::IsochroneLineDisplay& style,
                         bool cursor_focus) {
  int opacity = std::clamp(settings.opacity_percent, 10, 100);
  if (cursor_focus && settings.fade_during_cursor_inspection &&
      !style.cursor_selected && !style.highlighted)
    opacity = std::max(8, opacity / 3);
  const unsigned char alpha =
      static_cast<unsigned char>(std::lround(opacity * 2.55));
  wxColour colour = style.major ? palette.major : palette.minor;
  if (settings.colour_mode == ppm::IsochroneColourMode::kElapsedTime)
    colour = BlendColour(wxColour(55, 125, 205), wxColour(195, 60, 160),
                         style.elapsed_fraction, alpha);
  else
    colour.Set(colour.Red(), colour.Green(), colour.Blue(), alpha);
  if (style.highlighted)
    colour = wxColour(palette.highlighted.Red(), palette.highlighted.Green(),
                      palette.highlighted.Blue(), 235);
  if (style.cursor_selected)
    colour = wxColour(palette.cursor.Red(), palette.cursor.Green(),
                      palette.cursor.Blue(), 235);
  return colour;
}

double IsochroneWidth(const ppm::IsochroneDisplaySettings& settings,
                      const ppm::IsochroneLineDisplay& style) {
  double width = std::clamp(settings.line_width, 0.5, 3.0);
  if (style.major) width *= 1.65;
  if (style.highlighted || style.cursor_selected) width *= 2.6;
  return std::clamp(width, 0.5, 6.0);
}

wxString IsochroneTimeLabel(std::int64_t departure, std::int64_t contour_time) {
  const std::uint64_t elapsed = static_cast<std::uint64_t>(
      std::max<std::int64_t>(0, contour_time - departure));
  const std::uint64_t days = elapsed / 86400;
  const std::uint64_t hours = (elapsed % 86400) / 3600;
  if (days && hours)
    return wxString::Format("+%llud %lluh",
                            static_cast<unsigned long long>(days),
                            static_cast<unsigned long long>(hours));
  if (days)
    return wxString::Format("+%llud", static_cast<unsigned long long>(days));
  return wxString::Format("+%lluh", static_cast<unsigned long long>(hours));
}

void DrawIsochroneLabel(wxDC& dc, const wxPoint& anchor, const wxString& text,
                        const IsochronePalette& palette) {
  const wxSize extent = dc.GetTextExtent(text);
  wxRect background(anchor.x + 6, anchor.y - extent.y - 7, extent.x + 8,
                    extent.y + 5);
  dc.SetPen(wxPen(palette.major, 1));
  dc.SetBrush(wxBrush(palette.label_background));
  dc.DrawRoundedRectangle(background, 3);
  dc.SetTextForeground(palette.label_text);
  dc.DrawText(text, background.x + 4, background.y + 2);
}

void DrawGlIsochroneLabel(const wxPoint& anchor, const wxString& text,
                          const IsochronePalette& palette) {
  // A small public-OpenGL stroke font keeps elapsed-time labels available
  // without depending on OpenCPN's private texture-font implementation.
  // Vulkan uses the renderer-neutral wxDC path above.
  const std::string value = text.ToStdString();
  constexpr float scale = 1.35F;
  constexpr float advance = 6.0F * scale;
  const float left = static_cast<float>(anchor.x + 6);
  const float top = static_cast<float>(anchor.y - 13);
  const float width = std::max(10.0F, advance * value.size() + 4.0F);
  glColor4ub(palette.label_background.Red(), palette.label_background.Green(),
             palette.label_background.Blue(), 225);
  glBegin(GL_QUADS);
  glVertex2f(left - 2.0F, top - 2.0F);
  glVertex2f(left + width, top - 2.0F);
  glVertex2f(left + width, top + 11.0F);
  glVertex2f(left - 2.0F, top + 11.0F);
  glEnd();

  auto segment = [](int id, float x, float y) {
    static constexpr float coordinates[7][4] = {
        {0, 0, 4, 0}, {4, 0, 4, 4}, {4, 4, 4, 8}, {0, 8, 4, 8},
        {0, 4, 0, 8}, {0, 0, 0, 4}, {0, 4, 4, 4}};
    glVertex2f(x + coordinates[id][0] * scale, y + coordinates[id][1] * scale);
    glVertex2f(x + coordinates[id][2] * scale, y + coordinates[id][3] * scale);
  };
  static constexpr unsigned char digits[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66,
                                               0x6d, 0x7d, 0x07, 0x7f, 0x6f};
  glColor4ub(palette.label_text.Red(), palette.label_text.Green(),
             palette.label_text.Blue(), 255);
  glLineWidth(1.4F);
  glBegin(GL_LINES);
  float x = left;
  for (const char character : value) {
    if (character >= '0' && character <= '9') {
      const unsigned char mask = digits[character - '0'];
      for (int id = 0; id < 7; ++id)
        if (mask & (1U << id)) segment(id, x, top);
    } else if (character == '+') {
      glVertex2f(x, top + 4 * scale);
      glVertex2f(x + 4 * scale, top + 4 * scale);
      glVertex2f(x + 2 * scale, top + 2 * scale);
      glVertex2f(x + 2 * scale, top + 6 * scale);
    } else if (character == 'h') {
      glVertex2f(x, top);
      glVertex2f(x, top + 8 * scale);
      glVertex2f(x, top + 4 * scale);
      glVertex2f(x + 4 * scale, top + 4 * scale);
      glVertex2f(x + 4 * scale, top + 4 * scale);
      glVertex2f(x + 4 * scale, top + 8 * scale);
    } else if (character == 'd') {
      glVertex2f(x + 4 * scale, top);
      glVertex2f(x + 4 * scale, top + 8 * scale);
      glVertex2f(x, top + 4 * scale);
      glVertex2f(x + 4 * scale, top + 4 * scale);
      glVertex2f(x, top + 4 * scale);
      glVertex2f(x, top + 8 * scale);
      glVertex2f(x, top + 8 * scale);
      glVertex2f(x + 4 * scale, top + 8 * scale);
    }
    x += character == ' ' ? advance * 0.65F : advance;
  }
  glEnd();
}

void DrawRouteWindBarb(wxDC& dc, const wxPoint& origin, double east_knots,
                       double north_knots, const wxColour& colour) {
  const auto geometry = ppm::BuildWindBarbGeometry(east_knots, north_knots);
  if (!geometry.visible) return;
  dc.SetPen(wxPen(colour, 2, wxPENSTYLE_SOLID));
  dc.SetBrush(wxBrush(colour));
  if (geometry.calm) {
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawCircle(origin, static_cast<int>(std::lround(geometry.calm_radius)));
    return;
  }
  const auto point = [&origin](const ppm::WindBarbPoint& value) {
    return wxPoint(origin.x + static_cast<int>(std::lround(value.x)),
                   origin.y + static_cast<int>(std::lround(value.y)));
  };
  for (const auto& line : geometry.lines) {
    const wxPoint start = point(line.start);
    const wxPoint end = point(line.end);
    dc.DrawLine(start.x, start.y, end.x, end.y);
  }
  for (const auto& pennant : geometry.pennants) {
    wxPoint triangle[3] = {point(pennant[0]), point(pennant[1]),
                           point(pennant[2])};
    dc.DrawPolygon(3, triangle);
  }
}

void DrawRouteWindBarbGl(const wxPoint& origin, double east_knots,
                         double north_knots) {
  const auto geometry = ppm::BuildWindBarbGeometry(east_knots, north_knots);
  if (!geometry.visible) return;
  if (geometry.calm) {
    constexpr int kCircleSegments = 16;
    glBegin(GL_LINE_LOOP);
    for (int index = 0; index < kCircleSegments; ++index) {
      const double angle = 2.0 * std::acos(-1.0) * static_cast<double>(index) /
                           static_cast<double>(kCircleSegments);
      glVertex2d(origin.x + std::cos(angle) * geometry.calm_radius,
                 origin.y + std::sin(angle) * geometry.calm_radius);
    }
    glEnd();
    return;
  }

  glBegin(GL_LINES);
  for (const auto& line : geometry.lines) {
    glVertex2d(origin.x + line.start.x, origin.y + line.start.y);
    glVertex2d(origin.x + line.end.x, origin.y + line.end.y);
  }
  glEnd();
  if (!geometry.pennants.empty()) {
    glBegin(GL_TRIANGLES);
    for (const auto& pennant : geometry.pennants) {
      for (const auto& point : pennant)
        glVertex2d(origin.x + point.x, origin.y + point.y);
    }
    glEnd();
  }
}
}  // namespace

class PortableWeatherRoutingHost::Impl {
public:
  Impl(wxWindow* parent_value, wxFileConfig* config_value,
       CalculateRoute route_calculator, CalculatePassage passage_calculator,
       BeginRouteAttempt route_starter, std::function<void()> route_canceller,
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
        calculate_passage(std::move(passage_calculator)),
        begin_route_attempt(std::move(route_starter)),
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
  bool ShowRouteAnalysis(const wxString& route_id, wxString* error);
  bool Render(wxDC& dc, PlugIn_ViewPort* viewport);
  bool RenderGL(PlugIn_ViewPort* viewport);
  void SetColorScheme(int scheme);
  void CursorChanged();
  void ReportProgress(unsigned percent, const wxString& message);
  bool Cancelled() const { return cancelled.load(); }
  void Shutdown();

private:
  void CreateFrame();
  void CreateEditor();
  void ShowEditor(size_t tab = 0);
  void PopulateManagerPositions();
  void PopulateManagerRouting(const wxString& state = "Ready");
  void ClearRoutingResults(const wxString& diagnostic);
  void DeleteRouting();
  void ResetRouting();
  void ResetAdvancedSettings();
  void StartNewRouting();
  void UpdateRoutingActionState();
  void DispatchSurfaceAction(const wxString& action);
  wxPanel* CreateRoutePanel(wxNotebook* book);
  wxPanel* CreateSafetyPanel(wxNotebook* book);
  wxPanel* CreateAdvancedPanel(wxNotebook* book);
  wxPanel* CreateResultsPanel(wxNotebook* book);
  void ApplyIsochronePreset(ppm::IsochronePreset preset);
  void ShowIsochroneDisplaySettings();
  void ShowStabilityCorridorSettings();
  void ResetStabilityCorridor();
  void EnsureStabilityCorridor();
  const ppm::StabilityRouteFamily* SelectedStabilityFamily() const;
  const RoutingOutcome::InspectionLine* FindCursorTrace(
      PlugIn_ViewPort* viewport) const;
  ppm::IsochroneDisplayPlan IsochronePlan(
      PlugIn_ViewPort* viewport,
      const RoutingOutcome::InspectionLine* cursor_trace) const;
  void RefreshEnvironmentSummary();
  void RefreshNavigationPositions(bool initial = false);
  bool LoadOpenCpnRoute(wxString route_id, wxString* error);
  void ChooseOpenCpnRoute();
  void UpdateOpenCpnRouteSelection();
  bool ApplyPositionSource(bool start, bool report_error = true);
  void UpdatePositionControls(bool start);
  void RelayoutRoutePanel();
  PortableDepartureZone SelectedDepartureZone() const;
  bool GetDepartureUnixTime(int64_t* unix_time, wxString* error) const;
  void SetDepartureUnixTime(int64_t unix_time);
  void UpdateDepartureSummary();
  wxString FormatRoutingTime(int64_t unix_time) const;
  void UpdateTimeColumnLabels();
  void UpdateTimeStepControls();
  void SetTimeStepSeconds(long seconds);
  int TimeStepSeconds() const;
  bool LoadVesselPerformance(const wxString& path, bool report_error = true);
  void LoadSettings();
  void SaveSettings();
  void Start();
  void ApplyDepartureProgress(const ppm::DepartureSearchProgress& progress);
  void Finish(std::vector<DepartureResult> results, int64_t nominal_departure,
              std::optional<size_t> best_index);
  void PopulateDepartureResults();
  void SelectDepartureResult(size_t index);
  std::map<size_t, PortableNavigationPosition> SelectedPassageGates() const;
  bool ExportGpx();
  bool SendToOpenCpn();
  const wxString& Label(const wxString& id) const {
    return surface_labels.at(id);
  }

  wxWindow* parent = nullptr;
  wxFileConfig* config = nullptr;
  CalculateRoute calculate_route;
  CalculatePassage calculate_passage;
  BeginRouteAttempt begin_route_attempt;
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
                     const std::vector<PortableNavigationPosition>&, wxString*)>
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
  wxSplitterWindow* manager_splitter = nullptr;
  wxMenu* routing_context_menu = nullptr;
  wxListCtrl *manager_positions = nullptr, *manager_routings = nullptr;
  wxButton *manager_new = nullptr, *manager_compute = nullptr,
           *manager_edit = nullptr, *manager_delete = nullptr,
           *manager_export = nullptr, *manager_send = nullptr,
           *manager_stop = nullptr;
  std::map<wxString, wxMenuItem*> surface_menu_items;
  std::map<wxString, wxMenuItem*> context_menu_items;
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
  wxCheckBox *use_opencpn_route = nullptr, *reverse_opencpn_route = nullptr;
  wxButton* refresh_positions = nullptr;
  wxFilePickerCtrl* vessel_performance_file = nullptr;
  wxSpinCtrl *time_step_hours = nullptr, *time_step_minutes = nullptr,
             *heading_step = nullptr, *refined_heading_step = nullptr,
             *labels_per_cell = nullptr, *max_hours = nullptr,
             *max_states = nullptr, *departure_window = nullptr,
             *departure_spacing = nullptr, *departure_workers = nullptr,
             *min_wind_angle = nullptr, *max_wind_angle = nullptr,
             *maximum_latitude = nullptr, *upwind_efficiency = nullptr,
             *downwind_efficiency = nullptr, *tack_penalty = nullptr,
             *gybe_penalty = nullptr, *maximum_search_angle = nullptr;
  wxCheckBox *avoid_land = nullptr,
             *require_authoritative_chart_safety = nullptr,
             *limit_true_wind = nullptr, *limit_apparent_wind = nullptr,
             *limit_waves = nullptr, *limit_opposing_wind_current = nullptr,
             *use_currents = nullptr, *require_current_data = nullptr,
             *use_waves = nullptr, *require_wave_data = nullptr,
             *compare_departures = nullptr, *adaptive_headings = nullptr,
             *allow_motor_sailing = nullptr, *allow_motor = nullptr,
             *limit_motor_hours = nullptr, *limit_fuel = nullptr;
  wxCheckBox *show_isochrones = nullptr, *show_stability_corridor = nullptr,
             *show_route_wind = nullptr, *route_to_cursor = nullptr,
             *boat_at_grib_time = nullptr;
  wxChoice* isochrone_preset_choice = nullptr;
  wxButton *isochrone_settings_button = nullptr,
           *stability_settings_button = nullptr;
  wxTextCtrl *max_true_wind = nullptr, *max_apparent_wind = nullptr,
             *max_wave = nullptr, *max_opposing_wind_current = nullptr,
             *land_safety_margin = nullptr, *minimum_chart_depth = nullptr,
             *destination_tolerance = nullptr, *spatial_cell = nullptr,
             *motor_threshold = nullptr, *motor_speed = nullptr,
             *motor_sailing_boost = nullptr, *motor_hysteresis = nullptr,
             *maximum_motor_hours = nullptr, *fuel_consumption = nullptr,
             *maximum_fuel = nullptr;
  wxSpinCtrl *minimum_motor_run = nullptr, *mode_change_penalty = nullptr;
  wxStaticText *provider = nullptr, *vessel_performance_status = nullptr,
               *chart_safety_status = nullptr, *status = nullptr,
               *metrics = nullptr, *candidate_details = nullptr;
  wxListCtrl *departure_results = nullptr, *route_schedule = nullptr;
  wxTextCtrl* validation_diagnostics = nullptr;
  wxGauge* gauge = nullptr;
  wxTimer environment_refresh_timer;
  wxButton *new_routing = nullptr, *calculate = nullptr, *cancel = nullptr,
           *export_gpx = nullptr, *send_to_opencpn = nullptr;
  std::atomic<bool> cancelled{false};
  std::shared_ptr<std::atomic<bool>> alive =
      std::make_shared<std::atomic<bool>>(true);
  std::atomic<unsigned> departure_runs{1};
  std::atomic<unsigned> departures_completed{0};
  std::atomic<unsigned> departures_running{0};
  std::atomic<unsigned> departure_progress_floor{0};
  std::thread worker;
  std::thread stability_worker;
  std::atomic<std::uint64_t> stability_generation{0};
  std::atomic<bool> stability_running{false};
  bool routing_exists = true;
  bool calculation_running = false;
  std::vector<ocpn_portable_route_point> route;
  std::vector<ocpn_portable_route_environment_point> route_environment;
  std::vector<std::vector<ocpn_portable_route_point>> alternative_routes;
  std::vector<RoutingOutcome::InspectionLine> isochrones;
  std::vector<RoutingOutcome::InspectionLine> traces;
  ppm::IsochroneDisplaySettings isochrone_display =
      ppm::SettingsForPreset(ppm::IsochronePreset::kNavigation);
  int colour_scheme = PI_GLOBAL_COLOR_SCHEME_DAY;
  bool updating_isochrone_preset = false;
  std::vector<DepartureResult> departure_result_rows;
  ppm::StabilityCorridorOptions stability_options;
  std::optional<ppm::StabilityCorridorResult> stability_corridor;
  std::vector<PortableNavigationPosition> routing_result_gates;
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
  departure_time = new TimeCtrl(departure_editor, wxID_ANY, wxDefaultDateTime,
                                wxDefaultPosition,
                                departure_editor->FromDIP(wxSize(80, -1)));
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
  departure_timezone->SetMinSize(departure_editor->FromDIP(wxSize(190, -1)));
  departure_date->SetToolTip(
      "Departure calendar date in the selected timezone");
  departure_time->SetToolTip("Departure clock time in 24-hour HH:MM format");
  departure_timezone->SetToolTip(
      "Timezone used for departure entry and route schedules");
  departure_fields->Add(departure_date, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                        6);
  departure_fields->Add(departure_time, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                        6);
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
  departure_summary =
      new wxStaticText(departure_editor, wxID_ANY, wxEmptyString);
  departure_editor_root->Add(departure_summary, 0, wxEXPAND | wxTOP, 6);
  departure_editor->SetSizer(departure_editor_root);
  SetDepartureUnixTime(
      ((static_cast<int64_t>(wxDateTime::Now().GetTicks()) + 899) / 900) * 900);
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
  use_opencpn_route->SetToolTip(
      "Select this to route through every waypoint in an OpenCPN route. "
      "Clear it, or choose New routing, to enter an individual start and "
      "destination.");
  reverse_opencpn_route =
      new wxCheckBox(panel, wxID_ANY, Label("reverse-opencpn-route"));
  reverse_opencpn_route->Enable(false);
  route_choice = new wxChoice(panel, wxID_ANY);
  route_choice->Enable(false);
  refresh_positions = new wxButton(panel, wxID_ANY, Label("refresh-positions"));

  compare_departures =
      new wxCheckBox(panel, wxID_ANY, Label("compare-departures"));
  departure_window = new wxSpinCtrl(panel, wxID_ANY);
  departure_window->SetRange(5, 4320);
  departure_window->SetValue(360);
  departure_spacing = new wxSpinCtrl(panel, wxID_ANY);
  departure_spacing->SetRange(5, 720);
  departure_spacing->SetValue(60);

  auto* time_step_panel = new wxPanel(panel);
  auto* time_step_sizer = new wxBoxSizer(wxHORIZONTAL);
  time_step_hours = new wxSpinCtrl(time_step_panel, wxID_ANY);
  time_step_hours->SetRange(0, 6);
  time_step_hours->SetValue(1);
  time_step_hours->SetToolTip("Routing calculation step: 5 minutes to 6 hours");
  time_step_minutes = new wxSpinCtrl(time_step_panel, wxID_ANY);
  time_step_minutes->SetRange(0, 59);
  time_step_minutes->SetValue(0);
  time_step_minutes->SetToolTip(
      "Routing calculation step: 5 minutes to 6 hours");
  time_step_sizer->Add(time_step_hours, 0, wxRIGHT, 5);
  time_step_sizer->Add(new wxStaticText(time_step_panel, wxID_ANY, "h"), 0,
                       wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  time_step_sizer->Add(time_step_minutes, 0, wxRIGHT, 5);
  time_step_sizer->Add(new wxStaticText(time_step_panel, wxID_ANY, "m"), 0,
                       wxALIGN_CENTER_VERTICAL);
  time_step_panel->SetSizer(time_step_sizer);

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
  avoid_land = new wxCheckBox(panel, wxID_ANY, Label("avoid-unsafe"));
  avoid_land->SetValue(true);
  use_currents = new wxCheckBox(panel, wxID_ANY, Label("use-currents"));
  use_currents->SetValue(true);
  use_waves = new wxCheckBox(panel, wxID_ANY, Label("use-waves"));
  use_waves->SetValue(true);

  vessel_performance_status =
      new wxStaticText(panel, wxID_ANY, Label("vessel-performance-status"));
  current_dataset_summary = dataset_summary();
  provider = new wxStaticText(panel, wxID_ANY,
                              "Environment: " + current_dataset_summary);
  auto* route_mode =
      new wxStaticBoxSizer(wxVERTICAL, panel, "OpenCPN passage route");
  route_mode->Add(use_opencpn_route, 0, wxEXPAND | wxALL, 8);
  auto* route_selector = new wxFlexGridSizer(2, 8, 8);
  route_selector->AddGrowableCol(1);
  AddRow(route_selector, panel, Label("opencpn-route"), route_choice);
  route_mode->Add(route_selector, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  route_mode->Add(reverse_opencpn_route, 0,
                  wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  auto* route_mode_help = new wxStaticText(
      panel, wxID_ANY,
      "Clear the option above for a normal start-to-destination routing. "
      "Choosing New routing also returns to that mode.");
  route_mode_help->Wrap(panel->FromDIP(650));
  route_mode->Add(route_mode_help, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,
                  8);

  auto* columns = new wxFlexGridSizer(1, 2, 12, 12);
  columns->AddGrowableCol(0, 1);
  columns->AddGrowableCol(1, 1);

  auto* left = new wxBoxSizer(wxVERTICAL);
  auto* start_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Start");
  auto* start_grid = new wxFlexGridSizer(2, 6, 6);
  start_grid->AddGrowableCol(1);
  AddRow(start_grid, panel, Label("start-source"), start_source);
  AddRow(start_grid, panel, Label("start-waypoint"), start_waypoint);
  AddRow(start_grid, panel, Label("start-latitude"), start_lat);
  AddRow(start_grid, panel, Label("start-longitude"), start_lon);
  AddRow(start_grid, panel, Label("departure-utc"), departure_editor);
  start_box->Add(start_grid, 0, wxEXPAND | wxALL, 8);
  start_box->Add(compare_departures, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
  auto* departure_grid = new wxFlexGridSizer(2, 6, 6);
  departure_grid->AddGrowableCol(1);
  AddRow(departure_grid, panel, Label("departure-window"), departure_window);
  AddRow(departure_grid, panel, Label("departure-spacing"), departure_spacing);
  start_box->Add(departure_grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  left->Add(start_box, 0, wxEXPAND | wxBOTTOM, 8);

  auto* boat_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Boat");
  boat_box->Add(vessel_performance_file, 0, wxEXPAND | wxALL, 8);
  boat_box->Add(vessel_performance_status, 0,
                wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  left->Add(boat_box, 0, wxEXPAND | wxBOTTOM, 8);

  auto* constraints_box =
      new wxStaticBoxSizer(wxVERTICAL, panel, "Constraints");
  auto* constraints_grid = new wxFlexGridSizer(2, 6, 6);
  constraints_grid->AddGrowableCol(1);
  constraints_grid->Add(limit_true_wind, 0, wxALIGN_CENTER_VERTICAL);
  constraints_grid->Add(max_true_wind, 1, wxEXPAND);
  constraints_grid->Add(limit_apparent_wind, 0, wxALIGN_CENTER_VERTICAL);
  constraints_grid->Add(max_apparent_wind, 1, wxEXPAND);
  constraints_grid->Add(limit_waves, 0, wxALIGN_CENTER_VERTICAL);
  constraints_grid->Add(max_wave, 1, wxEXPAND);
  constraints_box->Add(constraints_grid, 0, wxEXPAND | wxALL, 8);
  left->Add(constraints_box, 0, wxEXPAND);

  auto* right = new wxBoxSizer(wxVERTICAL);
  auto* end_box = new wxStaticBoxSizer(wxVERTICAL, panel, "End");
  auto* end_grid = new wxFlexGridSizer(2, 6, 6);
  end_grid->AddGrowableCol(1);
  AddRow(end_grid, panel, Label("destination-source"), dest_source);
  AddRow(end_grid, panel, Label("destination-waypoint"), dest_waypoint);
  AddRow(end_grid, panel, Label("destination-latitude"), dest_lat);
  AddRow(end_grid, panel, Label("destination-longitude"), dest_lon);
  end_box->Add(end_grid, 0, wxEXPAND | wxALL, 8);
  right->Add(end_box, 0, wxEXPAND | wxBOTTOM, 8);

  auto* time_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Time Step");
  time_box->Add(time_step_panel, 0, wxALL, 8);
  right->Add(time_box, 0, wxEXPAND | wxBOTTOM, 8);

  auto* options_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Options");
  options_box->Add(avoid_land, 0, wxALL, 8);
  options_box->Add(use_currents, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
  options_box->Add(use_waves, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
  right->Add(options_box, 0, wxEXPAND | wxBOTTOM, 8);

  auto* source_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Data Source");
  source_box->Add(provider, 0, wxEXPAND | wxALL, 8);
  auto* broker_note = new wxStaticText(
      panel, wxID_ANY,
      "The Wasm route engine consumes environmental values through iGRIB's "
      "typed host-brokered provider service.");
  broker_note->Wrap(panel->FromDIP(360));
  source_box->Add(broker_note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  right->Add(source_box, 0, wxEXPAND);

  columns->Add(left, 1, wxEXPAND);
  columns->Add(right, 1, wxEXPAND);
  root->Add(route_mode, 0, wxEXPAND | wxALL, 8);
  root->Add(columns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  root->Add(refresh_positions, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
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
    UpdateOpenCpnRouteSelection();
  });
  route_choice->Bind(
      wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateOpenCpnRouteSelection(); });
  reverse_opencpn_route->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    UpdateOpenCpnRouteSelection();
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
  time_step_hours->Bind(wxEVT_SPINCTRL,
                        [this](wxSpinEvent&) { UpdateTimeStepControls(); });
  time_step_hours->Bind(wxEVT_TEXT,
                        [this](wxCommandEvent&) { UpdateTimeStepControls(); });
  auto update_departure_search = [this] {
    const bool enabled = compare_departures->GetValue();
    departure_window->Enable(enabled);
    departure_spacing->Enable(enabled);
  };
  compare_departures->Bind(wxEVT_CHECKBOX,
                           [update_departure_search](wxCommandEvent&) {
                             update_departure_search();
                           });
  update_departure_search();
  use_currents->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    if (require_current_data)
      require_current_data->Enable(use_currents->GetValue());
    if (limit_opposing_wind_current)
      limit_opposing_wind_current->Enable(use_currents->GetValue());
    if (max_opposing_wind_current)
      max_opposing_wind_current->Enable(
          use_currents->GetValue() && limit_opposing_wind_current &&
          limit_opposing_wind_current->GetValue());
  });
  use_waves->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    if (require_wave_data) require_wave_data->Enable(use_waves->GetValue());
    limit_waves->Enable(use_waves->GetValue());
    max_wave->Enable(use_waves->GetValue() && limit_waves->GetValue());
  });
  limit_waves->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    max_wave->Enable(use_waves->GetValue() && limit_waves->GetValue());
  });
  limit_true_wind->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    max_true_wind->Enable(limit_true_wind->GetValue());
  });
  limit_apparent_wind->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    max_apparent_wind->Enable(limit_apparent_wind->GetValue());
  });
  panel->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
    RelayoutRoutePanel();
    event.Skip();
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

PortableDepartureZone PortableWeatherRoutingHost::Impl::SelectedDepartureZone()
    const {
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
  return PortableDepartureToUnix(departure_date->GetValue(), clock.GetHour(),
                                 clock.GetMinute(), SelectedDepartureZone(),
                                 unix_time, error);
}

void PortableWeatherRoutingHost::Impl::SetDepartureUnixTime(int64_t unix_time) {
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
    departure_summary->SetLabel(zone.kind == PortableDepartureZoneKind::kUtc
                                    ? "Routing time: " + utc
                                    : "Routing time: " + selected + " = " +
                                          utc);
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
  const wxString zone = PortableDepartureZoneLabel(
      SelectedDepartureZone(),
      static_cast<int64_t>(wxDateTime::Now().GetTicks()));
  auto set_column = [](wxListCtrl* list, int index, const wxString& label) {
    if (!list || index < 0 || index >= list->GetColumnCount()) return;
    wxListItem column;
    column.SetMask(wxLIST_MASK_TEXT);
    column.SetText(label);
    list->SetColumn(index, column);
  };
  set_column(route_schedule, 0, "Time (" + zone + ")");
  set_column(manager_routings, 3, "Start Time (" + zone + ")");
  set_column(manager_routings, 5, "End Time (" + zone + ")");
}

void PortableWeatherRoutingHost::Impl::UpdateTimeStepControls() {
  const int hours = time_step_hours->GetValue();
  if (hours == 0) {
    time_step_minutes->Enable(true);
    time_step_minutes->SetRange(5, 59);
    if (time_step_minutes->GetValue() < 5) time_step_minutes->SetValue(5);
  } else if (hours == 6) {
    time_step_minutes->SetRange(0, 0);
    time_step_minutes->SetValue(0);
    time_step_minutes->Enable(false);
  } else {
    time_step_minutes->Enable(true);
    time_step_minutes->SetRange(0, 59);
  }
}

void PortableWeatherRoutingHost::Impl::SetTimeStepSeconds(long seconds) {
  const PortableRoutingTimeStep step =
      PortableRoutingTimeStepFromSeconds(static_cast<int>(
          std::clamp<long>(seconds, std::numeric_limits<int>::min(),
                           std::numeric_limits<int>::max())));
  time_step_hours->SetValue(step.hours);
  time_step_minutes->SetRange(0, 59);
  time_step_minutes->SetValue(step.minutes);
  UpdateTimeStepControls();
}

int PortableWeatherRoutingHost::Impl::TimeStepSeconds() const {
  return PortableRoutingTimeStepSeconds(time_step_hours->GetValue(),
                                        time_step_minutes->GetValue());
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
  navigation_routes =
      list_routes ? list_routes() : std::vector<PortableNavigationRoute>();
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
  reverse_opencpn_route->Enable(use_opencpn_route &&
                                use_opencpn_route->GetValue() &&
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
    status->SetLabel(
        wxString::Format("Loaded %zu OpenCPN waypoint(s) and %zu route(s)",
                         waypoints.size(), navigation_routes.size()));
  PopulateManagerPositions();
  if (manager_routings && manager_routings->GetItemCount() == 0)
    PopulateManagerRouting("Not computed");
}

void PortableWeatherRoutingHost::Impl::UpdateOpenCpnRouteSelection() {
  if (!use_opencpn_route || !route_choice || !reverse_opencpn_route) return;
  const bool enabled = use_opencpn_route->GetValue();
  route_choice->Enable(enabled && !navigation_routes.empty());
  reverse_opencpn_route->Enable(enabled && !navigation_routes.empty());
  start_source->Enable(!enabled);
  start_waypoint->Enable(!enabled);
  dest_source->Enable(!enabled);
  dest_waypoint->Enable(!enabled);
  start_lat->Enable(!enabled);
  start_lon->Enable(!enabled);
  dest_lat->Enable(!enabled);
  dest_lon->Enable(!enabled);
  if (!enabled) {
    ApplyPositionSource(true, false);
    ApplyPositionSource(false, false);
    if (status)
      status->SetLabel(
          "Start-to-destination mode; choose the two endpoint sources");
    return;
  }
  const int selection = route_choice->GetSelection();
  if (selection == wxNOT_FOUND ||
      static_cast<size_t>(selection) >= navigation_routes.size())
    return;
  const auto& selected = navigation_routes[static_cast<size_t>(selection)];
  if (selected.points.size() < 2) return;
  configured_route_id = selected.id;
  const bool reverse = reverse_opencpn_route->GetValue();
  const auto& first =
      reverse ? selected.points.back() : selected.points.front();
  const auto& last = reverse ? selected.points.front() : selected.points.back();
  start_lat->SetValue(wxString::Format("%.6f", first.latitude));
  start_lon->SetValue(wxString::Format("%.6f", first.longitude));
  dest_lat->SetValue(wxString::Format("%.6f", last.latitude));
  dest_lon->SetValue(wxString::Format("%.6f", last.longitude));
}

bool PortableWeatherRoutingHost::Impl::LoadOpenCpnRoute(wxString route_id,
                                                        wxString* error) {
  // Keep a value copy: menu callers commonly obtain this GUID from
  // navigation_routes, which RefreshNavigationPositions replaces below.
  RefreshNavigationPositions(false);
  const auto selected =
      std::find_if(navigation_routes.begin(), navigation_routes.end(),
                   [&route_id](const PortableNavigationRoute& route) {
                     return route.id == route_id;
                   });
  if (selected == navigation_routes.end()) {
    if (error)
      *error =
          "The selected OpenCPN route is no longer available. Refresh the "
          "route list and try again.";
    return false;
  }
  if (selected->points.size() < 2) {
    if (error) *error = "Weather routing requires at least two route points.";
    return false;
  }
  const int selection =
      static_cast<int>(std::distance(navigation_routes.begin(), selected));
  configured_route_id = selected->id;
  route_choice->SetSelection(selection);
  use_opencpn_route->SetValue(true);
  reverse_opencpn_route->SetValue(false);
  UpdateOpenCpnRouteSelection();
  routing_exists = true;
  ClearRoutingResults("Loaded from OpenCPN; not yet computed.");
  PopulateManagerRouting("Not computed");
  SaveSettings();
  UpdateRoutingActionState();
  if (status)
    status->SetLabel(
        wxString::Format("Loaded OpenCPN route “%s” with %zu waypoint(s)",
                         selected->name, selected->points.size()));
  return true;
}

void PortableWeatherRoutingHost::Impl::ChooseOpenCpnRoute() {
  RefreshNavigationPositions(false);
  if (navigation_routes.empty()) {
    wxMessageBox("OpenCPN has no routes containing at least two waypoints.",
                 "Load OpenCPN route", wxOK | wxICON_INFORMATION, frame);
    return;
  }
  wxArrayString choices;
  for (const auto& route : navigation_routes)
    choices.Add(wxString::Format("%s — %zu waypoint%s", route.name,
                                 route.points.size(),
                                 route.points.size() == 1 ? "" : "s"));
  wxSingleChoiceDialog chooser(
      frame,
      "Choose the route to analyse. Every waypoint will be retained "
      "as an ordered routing gate.",
      "Load OpenCPN route", choices);
  const auto current =
      std::find_if(navigation_routes.begin(), navigation_routes.end(),
                   [this](const PortableNavigationRoute& route) {
                     return route.id == configured_route_id;
                   });
  if (current != navigation_routes.end())
    chooser.SetSelection(
        static_cast<int>(std::distance(navigation_routes.begin(), current)));
  if (chooser.ShowModal() != wxID_OK) return;
  const int selection = chooser.GetSelection();
  if (selection == wxNOT_FOUND ||
      static_cast<size_t>(selection) >= navigation_routes.size())
    return;
  const wxString route_id =
      navigation_routes[static_cast<size_t>(selection)].id;
  wxString error;
  if (!LoadOpenCpnRoute(route_id, &error)) {
    wxMessageBox(error, "Could not load OpenCPN route", wxOK | wxICON_ERROR,
                 frame);
    return;
  }
  ShowEditor(0);
}

wxPanel* PortableWeatherRoutingHost::Impl::CreateSafetyPanel(wxNotebook* book) {
  auto* panel = new wxScrolledWindow(book);
  panel->SetScrollRate(0, 12);
  auto* root = new wxBoxSizer(wxVERTICAL);
  min_wind_angle = new wxSpinCtrl(panel, wxID_ANY);
  min_wind_angle->SetRange(0, 180);
  min_wind_angle->SetValue(40);
  max_wind_angle = new wxSpinCtrl(panel, wxID_ANY);
  max_wind_angle->SetRange(0, 180);
  max_wind_angle->SetValue(180);
  land_safety_margin = new wxTextCtrl(panel, wxID_ANY, "0.4");
  maximum_latitude = new wxSpinCtrl(panel, wxID_ANY);
  maximum_latitude->SetRange(1, 90);
  maximum_latitude->SetValue(89);
  maximum_search_angle = new wxSpinCtrl(panel, wxID_ANY);
  maximum_search_angle->SetRange(30, 180);
  maximum_search_angle->SetValue(120);
  destination_tolerance = new wxTextCtrl(panel, wxID_ANY, "1.0");
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
  auto* columns = new wxFlexGridSizer(1, 2, 12, 12);
  columns->AddGrowableCol(0, 1);
  columns->AddGrowableCol(1, 1);
  auto* left = new wxBoxSizer(wxVERTICAL);
  auto* right = new wxBoxSizer(wxVERTICAL);

  auto* constraints_box =
      new wxStaticBoxSizer(wxVERTICAL, panel, "Constraints");
  auto* constraints_grid = new wxFlexGridSizer(2, 8, 8);
  constraints_grid->AddGrowableCol(1);
  AddRow(constraints_grid, panel, Label("maximum-latitude"), maximum_latitude);
  AddRow(constraints_grid, panel, Label("maximum-search-angle"),
         maximum_search_angle);
  AddRow(constraints_grid, panel, Label("destination-tolerance"),
         destination_tolerance);
  AddRow(constraints_grid, panel, Label("land-safety-margin"),
         land_safety_margin);
  constraints_box->Add(constraints_grid, 0, wxEXPAND | wxALL, 8);
  left->Add(constraints_box, 0, wxEXPAND | wxBOTTOM, 8);

  auto* courses_box = new wxStaticBoxSizer(wxVERTICAL, panel,
                                           "Courses (relative to true wind)");
  auto* courses_grid = new wxFlexGridSizer(2, 8, 8);
  courses_grid->AddGrowableCol(1);
  AddRow(courses_grid, panel, Label("minimum-wind-angle"), min_wind_angle);
  AddRow(courses_grid, panel, Label("maximum-wind-angle"), max_wind_angle);
  courses_box->Add(courses_grid, 0, wxEXPAND | wxALL, 8);
  left->Add(courses_box, 0, wxEXPAND | wxBOTTOM, 8);

  auto* manoeuvres_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Options");
  auto* manoeuvres_grid = new wxFlexGridSizer(2, 8, 8);
  manoeuvres_grid->AddGrowableCol(1);
  AddRow(manoeuvres_grid, panel, Label("tack-penalty"), tack_penalty);
  AddRow(manoeuvres_grid, panel, Label("gybe-penalty"), gybe_penalty);
  manoeuvres_box->Add(manoeuvres_grid, 0, wxEXPAND | wxALL, 8);
  left->Add(manoeuvres_box, 0, wxEXPAND);

  auto* propulsion_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Motoring");
  propulsion_box->Add(allow_motor_sailing, 0, wxALL, 8);
  propulsion_box->Add(allow_motor, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
  auto* propulsion_grid = new wxFlexGridSizer(2, 8, 8);
  propulsion_grid->AddGrowableCol(1);
  AddRow(propulsion_grid, panel, Label("motor-threshold"), motor_threshold);
  AddRow(propulsion_grid, panel, Label("motor-speed"), motor_speed);
  AddRow(propulsion_grid, panel, Label("motor-sailing-boost"),
         motor_sailing_boost);
  AddRow(propulsion_grid, panel, Label("motor-hysteresis"), motor_hysteresis);
  AddRow(propulsion_grid, panel, Label("minimum-motor-run"), minimum_motor_run);
  AddRow(propulsion_grid, panel, Label("mode-change-penalty"),
         mode_change_penalty);
  propulsion_grid->Add(limit_motor_hours, 0, wxALIGN_CENTER_VERTICAL);
  propulsion_grid->Add(maximum_motor_hours, 1, wxEXPAND);
  AddRow(propulsion_grid, panel, Label("fuel-consumption"), fuel_consumption);
  propulsion_grid->Add(limit_fuel, 0, wxALIGN_CENTER_VERTICAL);
  propulsion_grid->Add(maximum_fuel, 1, wxEXPAND);
  propulsion_box->Add(propulsion_grid, 0,
                      wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  right->Add(propulsion_box, 0, wxEXPAND | wxBOTTOM, 8);

  auto* efficiency_box =
      new wxStaticBoxSizer(wxVERTICAL, panel, "Polar Efficiency");
  auto* efficiency_grid = new wxFlexGridSizer(2, 8, 8);
  efficiency_grid->AddGrowableCol(1);
  AddRow(efficiency_grid, panel, Label("upwind-efficiency"), upwind_efficiency);
  AddRow(efficiency_grid, panel, Label("downwind-efficiency"),
         downwind_efficiency);
  efficiency_box->Add(efficiency_grid, 0, wxEXPAND | wxALL, 8);
  right->Add(efficiency_box, 0, wxEXPAND);

  columns->Add(left, 1, wxEXPAND);
  columns->Add(right, 1, wxEXPAND);
  root->Add(columns, 0, wxEXPAND | wxALL, 8);
  auto* reset = new wxButton(panel, wxID_ANY, Label("reset-advanced"));
  reset->Bind(wxEVT_BUTTON,
              [this](wxCommandEvent&) { ResetAdvancedSettings(); });
  root->Add(reset, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, 8);
  panel->SetSizer(root);
  auto update_propulsion = [this] {
    const bool enabled =
        allow_motor_sailing->GetValue() || allow_motor->GetValue();
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
      wxEVT_CHECKBOX,
      [update_propulsion](wxCommandEvent&) { update_propulsion(); });
  allow_motor->Bind(wxEVT_CHECKBOX, [update_propulsion](wxCommandEvent&) {
    update_propulsion();
  });
  limit_motor_hours->Bind(wxEVT_CHECKBOX, [update_propulsion](wxCommandEvent&) {
    update_propulsion();
  });
  limit_fuel->Bind(wxEVT_CHECKBOX, [update_propulsion](wxCommandEvent&) {
    update_propulsion();
  });
  update_propulsion();
  panel->FitInside();
  return panel;
}

wxPanel* PortableWeatherRoutingHost::Impl::CreateAdvancedPanel(
    wxNotebook* book) {
  auto* panel = new wxScrolledWindow(book);
  panel->SetScrollRate(0, panel->FromDIP(12));

  require_authoritative_chart_safety = new wxCheckBox(
      panel, wxID_ANY, Label("require-authoritative-chart-safety"));
  require_authoritative_chart_safety->SetValue(true);
  chart_safety_status =
      new wxStaticText(panel, wxID_ANY, ppm::ChartSafetyService().Summary());
  minimum_chart_depth = new wxTextCtrl(panel, wxID_ANY, "2.0");
  require_current_data =
      new wxCheckBox(panel, wxID_ANY, Label("require-current-data"));
  require_current_data->SetValue(false);
  require_wave_data =
      new wxCheckBox(panel, wxID_ANY, Label("require-wave-data"));
  require_wave_data->SetValue(true);
  limit_opposing_wind_current =
      new wxCheckBox(panel, wxID_ANY, Label("maximum-opposing-wind-current"));
  limit_opposing_wind_current->SetValue(false);
  max_opposing_wind_current = new wxTextCtrl(panel, wxID_ANY, "0.0");
  max_opposing_wind_current->Enable(false);

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
  max_hours = new wxSpinCtrl(panel, wxID_ANY);
  max_hours->SetRange(6, 720);
  max_hours->SetValue(120);
  max_states = new wxSpinCtrl(panel, wxID_ANY);
  max_states->SetRange(1000, 500000);
  max_states->SetValue(80000);
  departure_workers = new wxSpinCtrl(panel, wxID_ANY);
  departure_workers->SetRange(0, 8);
  departure_workers->SetValue(0);

  auto* root = new wxBoxSizer(wxVERTICAL);
  auto* columns = new wxFlexGridSizer(1, 2, 12, 12);
  columns->AddGrowableCol(0, 1);
  columns->AddGrowableCol(1, 1);
  auto* left = new wxBoxSizer(wxVERTICAL);
  auto* right = new wxBoxSizer(wxVERTICAL);

  auto* chart_box = new wxStaticBoxSizer(wxVERTICAL, panel, "Chart safety");
  chart_box->Add(require_authoritative_chart_safety, 0, wxALL, 8);
  auto* chart_grid = new wxFlexGridSizer(2, 8, 8);
  chart_grid->AddGrowableCol(1);
  AddRow(chart_grid, panel, Label("minimum-chart-depth"), minimum_chart_depth);
  chart_box->Add(chart_grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  chart_box->Add(chart_safety_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,
                 8);
  left->Add(chart_box, 0, wxEXPAND | wxBOTTOM, 8);

  auto* environment_box =
      new wxStaticBoxSizer(wxVERTICAL, panel, "Environmental coverage");
  environment_box->Add(require_current_data, 0, wxALL, 8);
  environment_box->Add(require_wave_data, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
  auto* environment_grid = new wxFlexGridSizer(2, 8, 8);
  environment_grid->AddGrowableCol(1);
  environment_grid->Add(limit_opposing_wind_current, 0,
                        wxALIGN_CENTER_VERTICAL);
  environment_grid->Add(max_opposing_wind_current, 1, wxEXPAND);
  environment_box->Add(environment_grid, 0,
                       wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  left->Add(environment_box, 0, wxEXPAND);

  auto* solver_box =
      new wxStaticBoxSizer(wxVERTICAL, panel, "Portable route engine");
  solver_box->Add(adaptive_headings, 0, wxALL, 8);
  auto* solver_grid = new wxFlexGridSizer(2, 8, 8);
  solver_grid->AddGrowableCol(1);
  AddRow(solver_grid, panel, Label("heading-step"), heading_step);
  AddRow(solver_grid, panel, Label("refined-heading-step"),
         refined_heading_step);
  AddRow(solver_grid, panel, Label("spatial-cell"), spatial_cell);
  AddRow(solver_grid, panel, Label("labels-per-cell"), labels_per_cell);
  AddRow(solver_grid, panel, Label("maximum-hours"), max_hours);
  AddRow(solver_grid, panel, Label("maximum-states"), max_states);
  AddRow(solver_grid, panel, Label("departure-workers"), departure_workers);
  solver_box->Add(solver_grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  right->Add(solver_box, 0, wxEXPAND);

  columns->Add(left, 1, wxEXPAND);
  columns->Add(right, 1, wxEXPAND);
  root->Add(columns, 0, wxEXPAND | wxALL, 8);
  auto* note = new wxStaticText(
      panel, wxID_ANY,
      "Authoritative validation fails closed when chart or depth evidence is "
      "missing. Solver resource limits and departure workers bound CPU and "
      "memory use without weakening final route replay.");
  note->Wrap(panel->FromDIP(720));
  root->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  panel->SetSizer(root);

  adaptive_headings->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    refined_heading_step->Enable(adaptive_headings->GetValue());
  });
  limit_opposing_wind_current->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    max_opposing_wind_current->Enable(limit_opposing_wind_current->GetValue() &&
                                      use_currents->GetValue());
  });
  panel->FitInside();
  return panel;
}

wxPanel* PortableWeatherRoutingHost::Impl::CreateResultsPanel(
    wxNotebook* book) {
  auto* panel = new wxScrolledWindow(book);
  panel->SetScrollRate(0, panel->FromDIP(12));
  auto* root = new wxBoxSizer(wxVERTICAL);
  metrics = new wxStaticText(panel, wxID_ANY, "No route calculated");
  root->Add(metrics, 0, wxEXPAND | wxALL, 12);
  departure_results =
      new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
  departure_results->SetName(Label("departure-results"));
  auto table_columns = [this](const wxString& id) {
    wxArrayString result;
    for (int index = 0; index < surface_definition["controls"].Size();
         ++index) {
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
  auto* details_pane =
      new wxCollapsiblePane(panel, wxID_ANY, Label("candidate-details"));
  candidate_details = new wxStaticText(
      details_pane->GetPane(), wxID_ANY,
      "Select a completed departure candidate to inspect its detailed "
      "performance and validation metrics.");
  candidate_details->Wrap(panel->FromDIP(720));
  auto* details_sizer = new wxBoxSizer(wxVERTICAL);
  details_sizer->Add(candidate_details, 1, wxEXPAND | wxALL, 8);
  details_pane->GetPane()->SetSizer(details_sizer);
  details_pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED,
                     [panel](wxCollapsiblePaneEvent&) { panel->Layout(); });
  root->Add(details_pane, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
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
  root->Add(route_schedule, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(new wxStaticText(panel, wxID_ANY, Label("validation-diagnostics")),
            0, wxEXPAND | wxLEFT | wxRIGHT, 12);
  validation_diagnostics = new wxTextCtrl(
      panel, wxID_ANY, "No route validation result", wxDefaultPosition,
      wxSize(-1, panel->FromDIP(70)), wxTE_MULTILINE | wxTE_READONLY);
  root->Add(validation_diagnostics, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,
            12);
  show_isochrones = new wxCheckBox(panel, wxID_ANY, Label("show-isochrones"));
  isochrone_preset_choice = new wxChoice(
      panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
      wxArrayString{"Navigation", "Analysis", "Minimal", "Custom"});
  isochrone_settings_button =
      new wxButton(panel, wxID_ANY, Label("isochrone-display-settings"));
  show_stability_corridor =
      new wxCheckBox(panel, wxID_ANY, Label("show-stability-corridor"));
  stability_settings_button =
      new wxButton(panel, wxID_ANY, Label("stability-corridor-settings"));
  show_route_wind = new wxCheckBox(panel, wxID_ANY, Label("show-route-wind"));
  route_to_cursor = new wxCheckBox(panel, wxID_ANY, Label("route-to-cursor"));
  boat_at_grib_time =
      new wxCheckBox(panel, wxID_ANY, Label("boat-at-grib-time"));
  show_isochrones->SetToolTip(
      "Show or hide the retained equal-time outer fronts for the selected "
      "routing; recalculation is not required.");
  route_to_cursor->SetToolTip(
      "Toggle after calculation, then move the chart cursor near a displayed "
      "front to inspect its exact route from the passage origin.");
  root->Add(show_isochrones, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  auto* display_style = new wxBoxSizer(wxHORIZONTAL);
  display_style->Add(
      new wxStaticText(panel, wxID_ANY, Label("isochrone-display-preset")), 0,
      wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  display_style->Add(isochrone_preset_choice, 0, wxRIGHT, 8);
  display_style->Add(isochrone_settings_button, 0);
  root->Add(display_style, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  auto* stability_row = new wxBoxSizer(wxHORIZONTAL);
  stability_row->Add(show_stability_corridor, 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  stability_row->Add(stability_settings_button, 0);
  root->Add(stability_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(show_route_wind, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(route_to_cursor, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(boat_at_grib_time, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  for (auto* toggle :
       {show_isochrones, show_route_wind, route_to_cursor, boat_at_grib_time}) {
    toggle->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
      SaveSettings();
      if (parent) parent->Refresh();
    });
  }
  show_stability_corridor->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    SaveSettings();
    if (show_stability_corridor->GetValue()) EnsureStabilityCorridor();
    if (parent) parent->Refresh(false);
  });
  stability_settings_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    ShowStabilityCorridorSettings();
  });
  isochrone_preset_choice->SetToolTip(
      "Navigation is a quiet chart view; Analysis shows every retained "
      "front; Minimal keeps only major, selected-time and final contours.");
  isochrone_preset_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    if (updating_isochrone_preset) return;
    const int selection = isochrone_preset_choice->GetSelection();
    if (selection < 0 || selection > 3) return;
    ApplyIsochronePreset(static_cast<ppm::IsochronePreset>(selection));
    SaveSettings();
    if (parent) parent->Refresh(false);
  });
  isochrone_settings_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    ShowIsochroneDisplaySettings();
  });
  root->Add(
      new wxStaticText(
          panel, wxID_ANY,
          "Save the selected result in OpenCPN or export it as GPX before "
          "choosing New routing. Route overlays and exported GPX are planning "
          "outputs, not safe or authoritative navigation routes."),
      0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  export_gpx = new wxButton(panel, wxID_ANY, Label("export-gpx"));
  export_gpx->Enable(false);
  export_gpx->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ExportGpx(); });
  send_to_opencpn = new wxButton(panel, wxID_ANY, Label("send-to-opencpn"));
  send_to_opencpn->Enable(false);
  send_to_opencpn->Bind(wxEVT_BUTTON,
                        [this](wxCommandEvent&) { SendToOpenCpn(); });
  auto* output_actions = new wxBoxSizer(wxHORIZONTAL);
  output_actions->Add(export_gpx, 0, wxRIGHT, 8);
  output_actions->Add(send_to_opencpn, 0);
  root->Add(output_actions, 0, wxALL, 12);
  panel->SetSizer(root);
  panel->FitInside();
  return panel;
}

void PortableWeatherRoutingHost::Impl::ApplyIsochronePreset(
    ppm::IsochronePreset preset) {
  if (preset == ppm::IsochronePreset::kCustom)
    isochrone_display.preset = preset;
  else
    isochrone_display = ppm::SettingsForPreset(preset);
  if (isochrone_preset_choice) {
    updating_isochrone_preset = true;
    isochrone_preset_choice->SetSelection(static_cast<int>(preset));
    updating_isochrone_preset = false;
  }
}

void PortableWeatherRoutingHost::Impl::ShowIsochroneDisplaySettings() {
  wxDialog dialog(
      editor ? static_cast<wxWindow*>(editor) : static_cast<wxWindow*>(frame),
      wxID_ANY, "Isochrone display settings", wxDefaultPosition, wxDefaultSize,
      wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto working = isochrone_display;
  auto* root = new wxBoxSizer(wxVERTICAL);
  auto* grid = new wxFlexGridSizer(2, 8, 8);
  grid->AddGrowableCol(1);

  const std::vector<int> interval_values{0,   -1,  30,  60,  120,
                                         180, 360, 720, 1440};
  wxArrayString interval_labels;
  interval_labels.Add("Automatic — passage length and chart scale");
  interval_labels.Add("Every retained solver layer");
  interval_labels.Add("30 minutes");
  interval_labels.Add("1 hour");
  interval_labels.Add("2 hours");
  interval_labels.Add("3 hours");
  interval_labels.Add("6 hours");
  interval_labels.Add("12 hours");
  interval_labels.Add("24 hours");
  interval_labels.Add("Custom");
  auto* interval = new wxChoice(&dialog, wxID_ANY, wxDefaultPosition,
                                wxDefaultSize, interval_labels);
  auto* custom_interval = new wxSpinCtrl(
      &dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
      wxSP_ARROW_KEYS, 15, 2880, std::max(15, working.interval_minutes));
  int interval_selection = static_cast<int>(interval_values.size());
  for (std::size_t index = 0; index < interval_values.size(); ++index)
    if (working.interval_minutes == interval_values[index])
      interval_selection = static_cast<int>(index);
  interval->SetSelection(interval_selection);
  custom_interval->Enable(interval_selection ==
                          static_cast<int>(interval_values.size()));
  interval->Bind(wxEVT_CHOICE, [interval, custom_interval,
                                custom_index = static_cast<int>(
                                    interval_values.size())](wxCommandEvent&) {
    custom_interval->Enable(interval->GetSelection() == custom_index);
  });
  AddRow(grid, &dialog, "Displayed interval", interval);
  AddRow(grid, &dialog, "Custom interval (minutes)", custom_interval);

  const std::vector<int> major_values{0, 360, 720, 1440};
  wxArrayString major_labels;
  major_labels.Add("Automatic — 6, 12 or 24 hours");
  major_labels.Add("6 hours");
  major_labels.Add("12 hours");
  major_labels.Add("24 hours");
  major_labels.Add("Custom");
  auto* major = new wxChoice(&dialog, wxID_ANY, wxDefaultPosition,
                             wxDefaultSize, major_labels);
  auto* custom_major = new wxSpinCtrl(
      &dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
      wxSP_ARROW_KEYS, 60, 10080, std::max(60, working.major_interval_minutes));
  int major_selection = static_cast<int>(major_values.size());
  for (std::size_t index = 0; index < major_values.size(); ++index)
    if (working.major_interval_minutes == major_values[index])
      major_selection = static_cast<int>(index);
  major->SetSelection(major_selection);
  custom_major->Enable(major_selection ==
                       static_cast<int>(major_values.size()));
  major->Bind(
      wxEVT_CHOICE,
      [major, custom_major,
       custom_index = static_cast<int>(major_values.size())](wxCommandEvent&) {
        custom_major->Enable(major->GetSelection() == custom_index);
      });
  AddRow(grid, &dialog, "Major contour interval", major);
  AddRow(grid, &dialog, "Custom major interval (minutes)", custom_major);

  auto* width = new wxSpinCtrlDouble(
      &dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
      wxSP_ARROW_KEYS, 0.5, 3.0, working.line_width, 0.25);
  width->SetDigits(2);
  AddRow(grid, &dialog, "Minor line width", width);
  auto* opacity = new wxSlider(&dialog, wxID_ANY, working.opacity_percent, 10,
                               100, wxDefaultPosition, wxSize(240, -1),
                               wxSL_HORIZONTAL | wxSL_VALUE_LABEL);
  AddRow(grid, &dialog, "Minor line opacity (%)", opacity);

  auto* colour_mode =
      new wxChoice(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   wxArrayString{"Uniform", "Elapsed-time sequence"});
  colour_mode->SetSelection(static_cast<int>(working.colour_mode));
  AddRow(grid, &dialog, "Contour colours", colour_mode);
  root->Add(grid, 0, wxEXPAND | wxALL, 12);

  auto* highlight =
      new wxCheckBox(&dialog, wxID_ANY,
                     "Highlight the contour nearest the displayed GRIB time");
  highlight->SetValue(working.highlight_environment_time);
  auto* labels =
      new wxCheckBox(&dialog, wxID_ANY,
                     "Label major and selected contours at the winning route");
  labels->SetValue(working.show_time_labels);
  auto* fade = new wxCheckBox(
      &dialog, wxID_ANY,
      "Fade unrelated contours while inspecting a route to the cursor");
  fade->SetValue(working.fade_during_cursor_inspection);
  auto* points = new wxCheckBox(&dialog, wxID_ANY,
                                "Show retained front points (diagnostic)");
  points->SetValue(working.show_front_points);
  for (auto* control : {highlight, labels, fade, points})
    root->Add(control, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

  auto* note = new wxStaticText(
      &dialog, wxID_ANY,
      "These are display-only settings. They do not alter routing resolution "
      "or the calculated result.");
  note->Wrap(dialog.FromDIP(520));
  root->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  auto* buttons = dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL);
  if (buttons) root->Add(buttons, 0, wxEXPAND | wxALL, 12);
  dialog.SetSizerAndFit(root);
  dialog.SetMinSize(dialog.FromDIP(wxSize(580, 500)));
  if (dialog.ShowModal() != wxID_OK) return;

  const int selected_interval = interval->GetSelection();
  working.interval_minutes =
      selected_interval >= 0 &&
              selected_interval < static_cast<int>(interval_values.size())
          ? interval_values[static_cast<std::size_t>(selected_interval)]
          : custom_interval->GetValue();
  const int selected_major = major->GetSelection();
  working.major_interval_minutes =
      selected_major >= 0 &&
              selected_major < static_cast<int>(major_values.size())
          ? major_values[static_cast<std::size_t>(selected_major)]
          : custom_major->GetValue();
  working.line_width = width->GetValue();
  working.opacity_percent = opacity->GetValue();
  working.colour_mode =
      colour_mode->GetSelection() ==
              static_cast<int>(ppm::IsochroneColourMode::kElapsedTime)
          ? ppm::IsochroneColourMode::kElapsedTime
          : ppm::IsochroneColourMode::kUniform;
  working.highlight_environment_time = highlight->GetValue();
  working.show_time_labels = labels->GetValue();
  working.fade_during_cursor_inspection = fade->GetValue();
  working.show_front_points = points->GetValue();
  working.preset = ppm::IsochronePreset::kCustom;
  isochrone_display = working;
  ApplyIsochronePreset(ppm::IsochronePreset::kCustom);
  SaveSettings();
  if (parent) parent->Refresh(false);
}

void PortableWeatherRoutingHost::Impl::ShowStabilityCorridorSettings() {
  wxDialog dialog(
      editor ? static_cast<wxWindow*>(editor) : static_cast<wxWindow*>(frame),
      wxID_ANY, "Departure stability corridor settings", wxDefaultPosition,
      wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* root = new wxBoxSizer(wxVERTICAL);
  auto* grid = new wxFlexGridSizer(2, 8, 8);
  grid->AddGrowableCol(1);
  auto make_number = [&dialog](double minimum, double maximum, double value,
                               double increment, int digits) {
    auto* control = new wxSpinCtrlDouble(
        &dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
        wxSP_ARROW_KEYS, minimum, maximum, value, increment);
    control->SetDigits(digits);
    return control;
  };
  auto* penalty = make_number(
      0.0, 1440.0, stability_options.maximum_elapsed_penalty_minutes, 15.0, 0);
  auto* cluster = make_number(
      0.25, 25.0, stability_options.cluster_distance_nautical_miles, 0.25, 2);
  auto* resolution = make_number(
      0.1, 5.0, stability_options.grid_resolution_nautical_miles, 0.1, 2);
  auto* influence = make_number(
      0.1, 10.0, stability_options.route_influence_nautical_miles, 0.1, 2);
  auto* outer = make_number(10.0, 100.0,
                            stability_options.outer_agreement * 100.0, 5.0, 0);
  auto* inner = make_number(10.0, 100.0,
                            stability_options.inner_agreement * 100.0, 5.0, 0);
  AddRow(grid, &dialog, "Maximum elapsed-time penalty (minutes)", penalty);
  AddRow(grid, &dialog, "Route-family separation (NM)", cluster);
  AddRow(grid, &dialog, "Agreement grid resolution (NM)", resolution);
  AddRow(grid, &dialog, "Route influence radius (NM)", influence);
  AddRow(grid, &dialog, "Outer agreement threshold (%)", outer);
  AddRow(grid, &dialog, "Inner agreement threshold (%)", inner);
  root->Add(grid, 0, wxEXPAND | wxALL, 12);
  auto* note = new wxStaticText(
      &dialog, wxID_ANY,
      "Only complete, independently validated routes within the elapsed-time "
      "penalty are compared. Similar routes are grouped into families, and "
      "agreement cells are accepted only after batched chart-safety checks. "
      "This analysis is calculated on demand and cached for the current "
      "departure result set.");
  note->Wrap(dialog.FromDIP(560));
  root->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  auto* buttons = dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL);
  if (buttons) root->Add(buttons, 0, wxEXPAND | wxALL, 12);
  dialog.SetSizerAndFit(root);
  if (dialog.ShowModal() != wxID_OK) return;
  if (inner->GetValue() < outer->GetValue()) {
    wxMessageBox(
        "The inner agreement threshold must be at least the outer "
        "agreement threshold.",
        "Invalid stability corridor settings", wxOK | wxICON_WARNING, &dialog);
    return;
  }
  stability_options.maximum_elapsed_penalty_minutes = penalty->GetValue();
  stability_options.cluster_distance_nautical_miles = cluster->GetValue();
  stability_options.grid_resolution_nautical_miles = resolution->GetValue();
  stability_options.route_influence_nautical_miles = influence->GetValue();
  stability_options.outer_agreement = outer->GetValue() / 100.0;
  stability_options.inner_agreement = inner->GetValue() / 100.0;
  ResetStabilityCorridor();
  SaveSettings();
  if (show_stability_corridor && show_stability_corridor->GetValue())
    EnsureStabilityCorridor();
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
  ApplyIsochronePreset(ppm::IsochronePreset::kNavigation);
  if (!config) {
    vessel_performance_file->SetPath(bundled_polar);
    LoadVesselPerformance(bundled_polar, false);
    UpdateDepartureSummary();
    UpdateTimeColumnLabels();
    return;
  }
  const wxString old_path = config->GetPath();
  config->SetPath("/PortablePlugins/" + plugin_id + "/Routing");
  routing_exists = config->ReadBool("routingExists", true);
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
      departure_timezone->SetSelection(static_cast<int>(
          std::distance(departure_time_zones.begin(), configured)));
  }
  if (have_initial_departure) SetDepartureUnixTime(initial_departure);
  avoid_land->SetValue(config->ReadBool("avoidUnsafeCharts", true));
  require_authoritative_chart_safety->SetValue(
      config->ReadBool("requireAuthoritativeChartSafety", true));
  use_opencpn_route->SetValue(config->ReadBool("useOpenCpnRoute", false));
  reverse_opencpn_route->SetValue(
      config->ReadBool("reverseOpenCpnRoute", false));
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
  minimum_chart_depth->SetValue(config->Read("minimumChartDepthM", "2.0"));
  use_currents->SetValue(config->ReadBool("useCurrents", true));
  require_current_data->SetValue(config->ReadBool("requireCurrentData", false));
  use_waves->SetValue(config->ReadBool("useWaves", true));
  require_wave_data->SetValue(config->ReadBool("requireWaveData", true));
  maximum_latitude->SetValue(config->ReadLong("maximumLatitude", 89));
  upwind_efficiency->SetValue(config->ReadLong("upwindEfficiency", 100));
  downwind_efficiency->SetValue(config->ReadLong("downwindEfficiency", 100));
  tack_penalty->SetValue(config->ReadLong("tackPenaltySeconds", 300));
  gybe_penalty->SetValue(config->ReadLong("gybePenaltySeconds", 300));
  allow_motor_sailing->SetValue(config->ReadBool("allowMotorSailing", false));
  allow_motor->SetValue(config->ReadBool("allowMotor", false));
  motor_threshold->SetValue(config->Read("motorThresholdKnots", "3.0"));
  motor_speed->SetValue(config->Read("motorSpeedKnots", "5.5"));
  motor_sailing_boost->SetValue(config->Read("motorSailingBoostKnots", "1.5"));
  motor_hysteresis->SetValue(
      config->Read("motorCrossoverHysteresisKnots", "0.2"));
  minimum_motor_run->SetValue(config->ReadLong("minimumMotorRunSeconds", 1800));
  mode_change_penalty->SetValue(
      config->ReadLong("modeChangePenaltySeconds", 120));
  limit_motor_hours->SetValue(config->ReadBool("limitMotorHours", false));
  maximum_motor_hours->SetValue(config->Read("maximumMotorHours", "24.0"));
  fuel_consumption->SetValue(
      config->Read("fuelConsumptionLitresPerHour", "2.5"));
  limit_fuel->SetValue(config->ReadBool("limitFuel", false));
  maximum_fuel->SetValue(config->Read("maximumFuelLitres", "100.0"));
  SetTimeStepSeconds(config->ReadLong("timeStepSeconds", 3600));
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
  const long departure_window_minutes =
      config->HasEntry("departureWindowMinutes")
          ? config->ReadLong("departureWindowMinutes", 360)
          : config->ReadLong("departureWindowHours", 6) * 60;
  const long departure_spacing_minutes =
      config->HasEntry("departureSpacingMinutes")
          ? config->ReadLong("departureSpacingMinutes", 60)
          : config->ReadLong("departureSpacingHours", 1) * 60;
  departure_window->SetValue(departure_window_minutes);
  departure_spacing->SetValue(departure_spacing_minutes);
  departure_workers->SetValue(config->ReadLong("departureWorkers", 0));
  show_isochrones->SetValue(config->ReadBool("showIsochrones", true));
  show_stability_corridor->SetValue(
      config->ReadBool("showStabilityCorridor", true));
  stability_options.maximum_elapsed_penalty_minutes =
      config->ReadDouble("stabilityMaximumElapsedPenaltyMinutes", 120.0);
  stability_options.cluster_distance_nautical_miles =
      config->ReadDouble("stabilityClusterDistanceNm", 2.5);
  stability_options.grid_resolution_nautical_miles =
      config->ReadDouble("stabilityGridResolutionNm", 0.5);
  stability_options.route_influence_nautical_miles =
      config->ReadDouble("stabilityRouteInfluenceNm", 0.75);
  stability_options.outer_agreement =
      config->ReadDouble("stabilityOuterAgreement", 0.4);
  stability_options.inner_agreement =
      config->ReadDouble("stabilityInnerAgreement", 0.7);
  show_route_wind->SetValue(config->ReadBool("showRouteWind", true));
  route_to_cursor->SetValue(config->ReadBool("routeToCursor", false));
  boat_at_grib_time->SetValue(config->ReadBool("boatAtGribTime", true));
  const long stored_preset =
      config->ReadLong("isochroneDisplayPreset",
                       static_cast<long>(ppm::IsochronePreset::kNavigation));
  const auto preset =
      stored_preset >= static_cast<long>(ppm::IsochronePreset::kNavigation) &&
              stored_preset <= static_cast<long>(ppm::IsochronePreset::kCustom)
          ? static_cast<ppm::IsochronePreset>(stored_preset)
          : ppm::IsochronePreset::kNavigation;
  ApplyIsochronePreset(preset);
  if (preset == ppm::IsochronePreset::kCustom) {
    isochrone_display.interval_minutes = static_cast<int>(
        config->ReadLong("isochroneDisplayIntervalMinutes", 0));
    isochrone_display.major_interval_minutes =
        static_cast<int>(config->ReadLong("isochroneMajorIntervalMinutes", 0));
    double width = 1.0;
    config->Read("isochroneLineWidth", &width, 1.0);
    isochrone_display.line_width = std::clamp(width, 0.5, 3.0);
    isochrone_display.opacity_percent = std::clamp(
        static_cast<int>(config->ReadLong("isochroneOpacityPercent", 36)), 10,
        100);
    isochrone_display.colour_mode =
        config->ReadLong("isochroneColourMode", 0) ==
                static_cast<long>(ppm::IsochroneColourMode::kElapsedTime)
            ? ppm::IsochroneColourMode::kElapsedTime
            : ppm::IsochroneColourMode::kUniform;
    isochrone_display.highlight_environment_time =
        config->ReadBool("isochroneHighlightEnvironmentTime", true);
    isochrone_display.show_time_labels =
        config->ReadBool("isochroneShowTimeLabels", true);
    isochrone_display.fade_during_cursor_inspection =
        config->ReadBool("isochroneFadeDuringCursorInspection", true);
    isochrone_display.show_front_points =
        config->ReadBool("isochroneShowFrontPoints", false);
  }
  if (frame) {
    const wxSize size = frame->GetSize();
    const long width = config->ReadLong("frameWidth", size.x);
    const long height = config->ReadLong("frameHeight", size.y);
    if (width >= frame->GetMinSize().x && height >= frame->GetMinSize().y)
      frame->SetSize(wxSize(width, height));
  }
  if (editor) {
    const wxSize size = editor->GetSize();
    const long width = config->ReadLong("editorWidth", size.x);
    const long height = config->ReadLong("editorHeight", size.y);
    if (width >= editor->GetMinSize().x && height >= editor->GetMinSize().y)
      editor->SetSize(wxSize(width, height));
  }
  if (manager_splitter)
    manager_splitter->SetSashPosition(
        config->ReadLong("managerSashPosition", frame->FromDIP(285)));
  if (notebook) {
    const long selected_tab = config->ReadLong("selectedTab", 0);
    if (selected_tab >= 0 &&
        selected_tab < static_cast<long>(notebook->GetPageCount()))
      notebook->SetSelection(static_cast<int>(selected_tab));
  }
  config->SetPath(old_path);

  require_current_data->Enable(use_currents->GetValue());
  require_wave_data->Enable(use_waves->GetValue());
  limit_waves->Enable(use_waves->GetValue());
  max_wave->Enable(use_waves->GetValue() && limit_waves->GetValue());
  max_true_wind->Enable(limit_true_wind->GetValue());
  max_apparent_wind->Enable(limit_apparent_wind->GetValue());
  limit_opposing_wind_current->Enable(use_currents->GetValue());
  max_opposing_wind_current->Enable(use_currents->GetValue() &&
                                    limit_opposing_wind_current->GetValue());
  departure_window->Enable(compare_departures->GetValue());
  departure_spacing->Enable(compare_departures->GetValue());
  refined_heading_step->Enable(adaptive_headings->GetValue());
  route_choice->Enable(use_opencpn_route->GetValue() &&
                       !navigation_routes.empty());
  reverse_opencpn_route->Enable(use_opencpn_route->GetValue() &&
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
  config->Write("settingsSchema", 9L);
  config->Write("routingExists", routing_exists);
  if (vessel_performance_file)
    config->Write("vesselPerformancePath", vessel_performance_file->GetPath());
  config->Write("departureTimeZone",
                PortableDepartureZoneSetting(SelectedDepartureZone()));
  config->Write("avoidUnsafeCharts", avoid_land->GetValue());
  config->Write("requireAuthoritativeChartSafety",
                require_authoritative_chart_safety->GetValue());
  config->Write("useOpenCpnRoute", use_opencpn_route->GetValue());
  config->Write("reverseOpenCpnRoute", reverse_opencpn_route->GetValue());
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
  config->Write("minimumChartDepthM", minimum_chart_depth->GetValue());
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
  config->Write("motorSailingBoostKnots", motor_sailing_boost->GetValue());
  config->Write("motorCrossoverHysteresisKnots", motor_hysteresis->GetValue());
  config->Write("minimumMotorRunSeconds",
                static_cast<long>(minimum_motor_run->GetValue()));
  config->Write("modeChangePenaltySeconds",
                static_cast<long>(mode_change_penalty->GetValue()));
  config->Write("limitMotorHours", limit_motor_hours->GetValue());
  config->Write("maximumMotorHours", maximum_motor_hours->GetValue());
  config->Write("fuelConsumptionLitresPerHour", fuel_consumption->GetValue());
  config->Write("limitFuel", limit_fuel->GetValue());
  config->Write("maximumFuelLitres", maximum_fuel->GetValue());
  config->Write("timeStepSeconds", static_cast<long>(TimeStepSeconds()));
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
  config->Write("departureWindowMinutes",
                static_cast<long>(departure_window->GetValue()));
  config->Write("departureSpacingMinutes",
                static_cast<long>(departure_spacing->GetValue()));
  config->Write("departureWorkers",
                static_cast<long>(departure_workers->GetValue()));
  config->Write("showIsochrones", show_isochrones->GetValue());
  config->Write("showStabilityCorridor", show_stability_corridor->GetValue());
  config->Write("stabilityMaximumElapsedPenaltyMinutes",
                stability_options.maximum_elapsed_penalty_minutes);
  config->Write("stabilityClusterDistanceNm",
                stability_options.cluster_distance_nautical_miles);
  config->Write("stabilityGridResolutionNm",
                stability_options.grid_resolution_nautical_miles);
  config->Write("stabilityRouteInfluenceNm",
                stability_options.route_influence_nautical_miles);
  config->Write("stabilityOuterAgreement", stability_options.outer_agreement);
  config->Write("stabilityInnerAgreement", stability_options.inner_agreement);
  config->Write("showRouteWind", show_route_wind->GetValue());
  config->Write("routeToCursor", route_to_cursor->GetValue());
  config->Write("boatAtGribTime", boat_at_grib_time->GetValue());
  config->Write("isochroneDisplayPreset",
                static_cast<long>(isochrone_display.preset));
  config->Write("isochroneDisplayIntervalMinutes",
                static_cast<long>(isochrone_display.interval_minutes));
  config->Write("isochroneMajorIntervalMinutes",
                static_cast<long>(isochrone_display.major_interval_minutes));
  config->Write("isochroneLineWidth", isochrone_display.line_width);
  config->Write("isochroneOpacityPercent",
                static_cast<long>(isochrone_display.opacity_percent));
  config->Write("isochroneColourMode",
                static_cast<long>(isochrone_display.colour_mode));
  config->Write("isochroneHighlightEnvironmentTime",
                isochrone_display.highlight_environment_time);
  config->Write("isochroneShowTimeLabels", isochrone_display.show_time_labels);
  config->Write("isochroneFadeDuringCursorInspection",
                isochrone_display.fade_during_cursor_inspection);
  config->Write("isochroneShowFrontPoints",
                isochrone_display.show_front_points);
  if (frame) {
    config->Write("frameWidth", static_cast<long>(frame->GetSize().x));
    config->Write("frameHeight", static_cast<long>(frame->GetSize().y));
  }
  if (editor) {
    config->Write("editorWidth", static_cast<long>(editor->GetSize().x));
    config->Write("editorHeight", static_cast<long>(editor->GetSize().y));
  }
  if (manager_splitter)
    config->Write("managerSashPosition",
                  static_cast<long>(manager_splitter->GetSashPosition()));
  if (notebook)
    config->Write("selectedTab", static_cast<long>(notebook->GetSelection()));
  config->SetPath(old_path);
  config->Flush();
}

void PortableWeatherRoutingHost::Impl::CreateEditor() {
  editor = new wxDialog(frame, wxID_ANY,
                        "Weather Routing Configuration — iWeatherRouting",
                        wxDefaultPosition, wxSize(1080, 820),
                        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  editor->SetMinSize(editor->FromDIP(wxSize(820, 600)));
  auto* root = new wxBoxSizer(wxVERTICAL);
  notebook = new wxNotebook(editor, wxID_ANY);
  notebook->AddPage(CreateRoutePanel(notebook), surface_tabs[0]);
  notebook->AddPage(CreateSafetyPanel(notebook), surface_tabs[1]);
  notebook->AddPage(CreateAdvancedPanel(notebook), surface_tabs[2]);
  notebook->AddPage(CreateResultsPanel(notebook), surface_tabs[3]);
  root->Add(notebook, 1, wxEXPAND | wxALL, 8);
  auto* buttons = new wxBoxSizer(wxHORIZONTAL);
  new_routing = new wxButton(editor, wxID_ANY, "New routing…");
  calculate = new wxButton(editor, wxID_ANY, Label("calculate"));
  cancel = new wxButton(editor, wxID_ANY, Label("cancel"));
  cancel->Enable(false);
  auto* close = new wxButton(editor, wxID_CLOSE, "OK");
  buttons->Add(new_routing, 0, wxRIGHT, 8);
  buttons->AddStretchSpacer();
  buttons->Add(calculate, 0, wxRIGHT, 8);
  buttons->Add(cancel, 0, wxRIGHT, 8);
  buttons->Add(close);
  root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  new_routing->Bind(wxEVT_BUTTON,
                    [this](wxCommandEvent&) { StartNewRouting(); });
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
  ppm::ShowAndActivateWindow(editor);
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
  if (!routing_exists) {
    for (int column = 0; column < manager_routings->GetColumnCount(); ++column)
      manager_routings->SetColumnWidth(column, wxLIST_AUTOSIZE_USEHEADER);
    return;
  }
  auto position_name = [this](bool start) {
    if (use_opencpn_route && use_opencpn_route->GetValue() && route_choice &&
        route_choice->GetSelection() != wxNOT_FOUND &&
        static_cast<size_t>(route_choice->GetSelection()) <
            navigation_routes.size()) {
      const auto& selected =
          navigation_routes[static_cast<size_t>(route_choice->GetSelection())];
      if (selected.points.size() >= 2) {
        const bool reverse =
            reverse_opencpn_route && reverse_opencpn_route->GetValue();
        if (start)
          return reverse ? selected.points.back().name
                         : selected.points.front().name;
        return reverse ? selected.points.front().name
                       : selected.points.back().name;
      }
    }
    wxChoice* source = start ? start_source : dest_source;
    wxChoice* waypoint = start ? start_waypoint : dest_waypoint;
    if (source->GetSelection() == kOpenCpnWaypoint &&
        waypoint->GetSelection() != wxNOT_FOUND)
      return waypoint->GetStringSelection()
          .BeforeFirst(wxUniChar(0x2014))
          .Trim();
    return source->GetStringSelection();
  };
  auto start_type = [this] {
    if (use_opencpn_route && use_opencpn_route->GetValue())
      return wxString("OpenCPN Route");
    switch (start_source->GetSelection()) {
      case kVesselPosition:
        return wxString("From Boat");
      case kOpenCpnWaypoint:
        return wxString("From Waypoint");
      case kChartCursor:
        return wxString("From Cursor");
      case kManualCoordinates:
        return wxString("Manual");
      default:
        return wxString("Unknown");
    }
  };
  const long row = manager_routings->InsertItem(0, "\u2713");
  manager_routings->SetItem(row, 1, start_type());
  manager_routings->SetItem(row, 2, position_name(true));
  int64_t configured_departure = 0;
  wxString departure_error;
  manager_routings->SetItem(
      row, 3,
      GetDepartureUnixTime(&configured_departure, &departure_error)
          ? FormatRoutingTime(configured_departure)
          : "Invalid departure");
  manager_routings->SetItem(row, 4, position_name(false));
  if (selected_departure_result != std::numeric_limits<size_t>::max() &&
      selected_departure_result < departure_result_rows.size() &&
      departure_result_rows[selected_departure_result].success) {
    const auto& outcome =
        departure_result_rows[selected_departure_result].outcome;
    manager_routings->SetItem(
        row, 5,
        FormatRoutingTime(outcome.departure_unix_time +
                          static_cast<int64_t>(outcome.duration_seconds)));
    manager_routings->SetItem(row, 6, FormatElapsed(outcome.duration_seconds));
    manager_routings->SetItem(
        row, 7, wxString::Format("%.1f NM", outcome.distance_nautical_miles));
  } else {
    for (int column = 5; column <= 7; ++column)
      manager_routings->SetItem(row, column, "N/A");
  }
  manager_routings->SetItem(row, 8, state);
  manager_routings->SetItemState(row, wxLIST_STATE_SELECTED,
                                 wxLIST_STATE_SELECTED);
  for (int column = 0; column < manager_routings->GetColumnCount(); ++column)
    manager_routings->SetColumnWidth(column, wxLIST_AUTOSIZE_USEHEADER);
}

void PortableWeatherRoutingHost::Impl::ClearRoutingResults(
    const wxString& diagnostic) {
  ResetStabilityCorridor();
  route.clear();
  route_environment.clear();
  alternative_routes.clear();
  isochrones.clear();
  traces.clear();
  departure_result_rows.clear();
  routing_result_gates.clear();
  selected_departure_result = std::numeric_limits<size_t>::max();
  best_departure_result = std::numeric_limits<size_t>::max();
  nominal_departure_unix_time = 0;
  if (departure_results) departure_results->DeleteAllItems();
  if (route_schedule) route_schedule->DeleteAllItems();
  if (validation_diagnostics) validation_diagnostics->SetValue(diagnostic);
  if (metrics) metrics->SetLabel("No route calculated");
  if (gauge) gauge->SetValue(0);
  if (parent) parent->Refresh();
}

void PortableWeatherRoutingHost::Impl::UpdateRoutingActionState() {
  const bool idle_routing = routing_exists && !calculation_running;
  const bool have_result = idle_routing && !route.empty();
  auto enable_menu = [this](const wxString& action, bool enabled) {
    const auto item = surface_menu_items.find(action);
    if (item != surface_menu_items.end()) item->second->Enable(enabled);
    const auto context = context_menu_items.find(action);
    if (context != context_menu_items.end()) context->second->Enable(enabled);
  };
  enable_menu("new-routing", !calculation_running);
  enable_menu("load-opencpn-route", !calculation_running);
  enable_menu("edit-routing", idle_routing);
  enable_menu("delete-routing", idle_routing);
  enable_menu("compute-routing", idle_routing);
  enable_menu("stop-routing", calculation_running);
  enable_menu("reset-routing", idle_routing);
  enable_menu("export-gpx", have_result);
  enable_menu("send-to-opencpn", have_result);
  enable_menu("show-results", routing_exists);
  if (manager_compute) manager_compute->Enable(idle_routing);
  if (manager_new) manager_new->Enable(!calculation_running);
  if (manager_edit) manager_edit->Enable(idle_routing);
  if (manager_delete) manager_delete->Enable(idle_routing);
  if (manager_stop) manager_stop->Enable(calculation_running);
  if (manager_export) manager_export->Enable(have_result);
  if (manager_send) manager_send->Enable(have_result);
  if (new_routing) new_routing->Enable(!calculation_running);
  if (calculate) calculate->Enable(idle_routing);
  if (cancel) cancel->Enable(calculation_running);
  if (export_gpx) export_gpx->Enable(have_result);
  if (send_to_opencpn) send_to_opencpn->Enable(have_result);
  if (vessel_performance_file)
    vessel_performance_file->Enable(!calculation_running);
}

void PortableWeatherRoutingHost::Impl::DeleteRouting() {
  if (!routing_exists || calculation_running) return;
  if (wxMessageBox(
          "Delete the selected portable routing and its calculated results?\n\n"
          "This does not delete any route previously sent to OpenCPN.",
          "Delete routing", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION,
          frame) != wxYES)
    return;
  routing_exists = false;
  ClearRoutingResults("No routing is selected.");
  PopulateManagerRouting();
  if (editor) editor->Hide();
  if (status) status->SetLabel("Routing deleted");
  SaveSettings();
  UpdateRoutingActionState();
}

void PortableWeatherRoutingHost::Impl::ResetRouting() {
  if (!routing_exists || calculation_running) return;
  ClearRoutingResults(
      "Routing results were reset. The configuration is ready to compute.");
  PopulateManagerRouting("Not computed");
  if (status) status->SetLabel("Routing reset; configuration retained");
  UpdateRoutingActionState();
}

void PortableWeatherRoutingHost::Impl::ResetAdvancedSettings() {
  if (calculation_running) return;
  maximum_latitude->SetValue(89);
  maximum_search_angle->SetValue(120);
  destination_tolerance->SetValue("1.0");
  land_safety_margin->SetValue("0.4");
  min_wind_angle->SetValue(40);
  max_wind_angle->SetValue(180);
  tack_penalty->SetValue(300);
  gybe_penalty->SetValue(300);
  upwind_efficiency->SetValue(100);
  downwind_efficiency->SetValue(100);
  allow_motor_sailing->SetValue(false);
  allow_motor->SetValue(false);
  motor_threshold->SetValue("3.0");
  motor_speed->SetValue("5.5");
  motor_sailing_boost->SetValue("1.5");
  motor_hysteresis->SetValue("0.2");
  minimum_motor_run->SetValue(1800);
  mode_change_penalty->SetValue(120);
  limit_motor_hours->SetValue(false);
  maximum_motor_hours->SetValue("24.0");
  fuel_consumption->SetValue("2.5");
  limit_fuel->SetValue(false);
  maximum_fuel->SetValue("100.0");
  for (auto* control :
       {motor_threshold, motor_speed, motor_sailing_boost, motor_hysteresis,
        maximum_motor_hours, fuel_consumption, maximum_fuel})
    control->Enable(false);
  minimum_motor_run->Enable(false);
  mode_change_penalty->Enable(false);
  limit_motor_hours->Enable(false);
  limit_fuel->Enable(false);
  if (status)
    status->SetLabel("Advanced parameters reset to portable defaults");
  SaveSettings();
}

void PortableWeatherRoutingHost::Impl::StartNewRouting() {
  if (calculation_running) return;
  if (!route.empty()) {
    wxMessageDialog save_prompt(
        editor ? static_cast<wxWindow*>(editor) : static_cast<wxWindow*>(frame),
        "Starting a new routing removes the current calculated result from "
        "iWeatherRouting.\n\n"
        "Save the selected result as an OpenCPN route first?\n\n"
        "Choose No to continue without saving, or Cancel to keep the current "
        "result. You can also export GPX from the Results tab.",
        "Start new routing", wxYES_NO | wxCANCEL | wxICON_QUESTION);
    save_prompt.SetYesNoCancelLabels("Save in OpenCPN", "Start without saving",
                                     "Cancel");
    const int answer = save_prompt.ShowModal();
    if (answer == wxID_CANCEL) return;
    if (answer == wxID_YES && !SendToOpenCpn()) return;
  }

  routing_exists = true;
  use_opencpn_route->SetValue(false);
  reverse_opencpn_route->SetValue(false);
  configured_route_id.clear();
  UpdateOpenCpnRouteSelection();
  ClearRoutingResults(
      "New start-to-destination routing has not been computed.");
  SetDepartureUnixTime(
      ((static_cast<int64_t>(wxDateTime::Now().GetTicks()) + 899) / 900) * 900);
  ShowEditor(0);
  PopulateManagerRouting("Not computed");
  if (status)
    status->SetLabel(
        "New routing ready; choose a start and destination, or select an "
        "OpenCPN route");
  SaveSettings();
  UpdateRoutingActionState();
}

void PortableWeatherRoutingHost::Impl::DispatchSurfaceAction(
    const wxString& action) {
  if (action == "close") {
    frame->Close();
  } else if (action == "refresh-positions") {
    RefreshNavigationPositions();
  } else if (action == "load-opencpn-route") {
    ChooseOpenCpnRoute();
  } else if (action == "new-routing") {
    StartNewRouting();
  } else if (action == "edit-routing" || action == "show-configuration") {
    if (routing_exists) ShowEditor(0);
  } else if (action == "show-results") {
    if (routing_exists) ShowEditor(3);
  } else if (action == "delete-routing") {
    DeleteRouting();
  } else if (action == "compute-routing") {
    Start();
  } else if (action == "stop-routing") {
    cancelled.store(true);
    if (cancel_routes) cancel_routes();
    if (status) status->SetLabel("Cancelling…");
  } else if (action == "reset-routing") {
    ResetRouting();
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
    if (action == "show-stability-corridor" && toggle && toggle->GetValue())
      EnsureStabilityCorridor();
    if (parent) parent->Refresh();
  }
}

void PortableWeatherRoutingHost::Impl::CreateFrame() {
  frame = new wxFrame(parent, wxID_ANY, surface_title, wxDefaultPosition,
                      wxSize(1120, 620),
                      wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT);
  frame->SetMinSize(frame->FromDIP(wxSize(780, 460)));

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
      frame->Bind(
          wxEVT_MENU,
          [this, action](wxCommandEvent&) { DispatchSurfaceAction(action); },
          item->GetId());
    }
    menu_bar->Append(menu, menu_definition.label);
  }
  frame->SetMenuBar(menu_bar);

  auto* root = new wxBoxSizer(wxVERTICAL);
  manager_splitter = new wxSplitterWindow(frame, wxID_ANY, wxDefaultPosition,
                                          wxDefaultSize, wxSP_3D);
  manager_splitter->SetSashGravity(0.28);
  manager_splitter->SetMinimumPaneSize(frame->FromDIP(180));
  auto* positions_panel = new wxPanel(manager_splitter);
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

  auto* routings_panel = new wxPanel(manager_splitter);
  auto* routings_root = new wxStaticBoxSizer(
      wxVERTICAL, routings_panel,
      surface_definition["manager"]["routings"]["label"].AsString());
  manager_routings =
      new wxListCtrl(routings_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
  for (int index = 0;
       index < surface_definition["manager"]["routings"]["columns"].Size();
       ++index)
    manager_routings->InsertColumn(
        index,
        surface_definition["manager"]["routings"]["columns"][index].AsString());
  routing_context_menu = new wxMenu;
  auto append_context_action = [this](const wxString& action) {
    wxString label = action;
    for (const auto& menu : surface_menus) {
      for (const auto& item : menu.items) {
        if (!item.separator && item.id == action) {
          label = item.label;
          break;
        }
      }
    }
    auto* item = routing_context_menu->Append(wxID_ANY, label);
    context_menu_items[action] = item;
    frame->Bind(
        wxEVT_MENU,
        [this, action](wxCommandEvent&) { DispatchSurfaceAction(action); },
        item->GetId());
  };
  append_context_action("new-routing");
  append_context_action("load-opencpn-route");
  append_context_action("edit-routing");
  routing_context_menu->AppendSeparator();
  append_context_action("compute-routing");
  append_context_action("stop-routing");
  append_context_action("reset-routing");
  routing_context_menu->AppendSeparator();
  append_context_action("delete-routing");
  routing_context_menu->AppendSeparator();
  append_context_action("export-gpx");
  append_context_action("send-to-opencpn");
  manager_routings->Bind(wxEVT_LIST_ITEM_ACTIVATED,
                         [this](wxListEvent&) { ShowEditor(0); });
  manager_routings->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent&) {
    manager_routings->PopupMenu(routing_context_menu);
  });
  routings_root->Add(manager_routings, 1, wxEXPAND | wxALL, 5);
  auto* actions = new wxBoxSizer(wxHORIZONTAL);
  for (int index = 0; index < surface_definition["manager"]["actions"].Size();
       ++index) {
    wxJSONValue action_definition =
        surface_definition["manager"]["actions"][index];
    const wxString action = action_definition["id"].AsString();
    auto* button = new wxButton(routings_panel, wxID_ANY,
                                action_definition["label"].AsString());
    button->Bind(wxEVT_BUTTON, [this, action](wxCommandEvent&) {
      DispatchSurfaceAction(action);
    });
    actions->Add(button, 0, wxRIGHT, 6);
    if (action == "compute-routing") manager_compute = button;
    if (action == "new-routing") manager_new = button;
    if (action == "edit-routing") manager_edit = button;
    if (action == "delete-routing") manager_delete = button;
    if (action == "export-gpx") manager_export = button;
    if (action == "send-to-opencpn") manager_send = button;
  }
  manager_stop = new wxButton(routings_panel, wxID_ANY, "&Stop");
  manager_stop->Enable(false);
  manager_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    DispatchSurfaceAction("stop-routing");
  });
  actions->Add(manager_stop, 0);
  routings_root->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
  status = new wxStaticText(routings_panel, wxID_ANY, "Ready");
  gauge = new wxGauge(routings_panel, wxID_ANY, 100);
  routings_root->Add(status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
  routings_root->Add(gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
  routings_panel->SetSizer(routings_root);
  manager_splitter->SplitVertically(positions_panel, routings_panel,
                                    frame->FromDIP(285));
  root->Add(manager_splitter, 1, wxEXPAND | wxALL, 5);
  frame->SetSizer(root);

  CreateEditor();
  environment_refresh_timer.SetOwner(frame);
  frame->Bind(
      wxEVT_TIMER, [this](wxTimerEvent&) { RefreshEnvironmentSummary(); },
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
    if (item != surface_menu_items.end())
      item->second->Check(toggle->GetValue());
  }
  if (!routing_exists) status->SetLabel("No routing configured; choose New");
  UpdateRoutingActionState();
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
  if (stopped.load() || !calculate_route || !calculate_passage) {
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
  ppm::ShowAndActivateWindow(frame);
  return true;
}

bool PortableWeatherRoutingHost::Impl::ShowRouteAnalysis(
    const wxString& route_id, wxString* error) {
  if (!Show(error)) return false;
  if (!LoadOpenCpnRoute(route_id, error)) return false;
  ShowEditor(0);
  return true;
}

void PortableWeatherRoutingHost::Impl::Start() {
  if (worker.joinable()) return;
  if (!routing_exists) {
    if (status) status->SetLabel("Choose New before calculating a routing");
    return;
  }
  const bool use_route = use_opencpn_route && use_opencpn_route->GetValue();
  if (!use_route && (!ApplyPositionSource(true) || !ApplyPositionSource(false)))
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
    const int selection =
        route_choice ? route_choice->GetSelection() : wxNOT_FOUND;
    if (selection == wxNOT_FOUND || selection < 0 ||
        static_cast<size_t>(selection) >= navigation_routes.size() ||
        navigation_routes[static_cast<size_t>(selection)].points.size() < 2) {
      status->SetLabel(
          "Select an OpenCPN route containing at least two valid waypoints");
      return;
    }
    routing_gates = navigation_routes[static_cast<size_t>(selection)].points;
    if (reverse_opencpn_route && reverse_opencpn_route->GetValue())
      std::reverse(routing_gates.begin(), routing_gates.end());
    request.start_latitude = routing_gates.front().latitude;
    request.start_longitude = routing_gates.front().longitude;
    request.destination_latitude = routing_gates.back().latitude;
    request.destination_longitude = routing_gates.back().longitude;
  } else {
    routing_gates = {
        {"manual:start", "Start", request.start_latitude,
         request.start_longitude},
        {"manual:destination", "Destination", request.destination_latitude,
         request.destination_longitude}};
  }
  const auto selected_performance = vessel_performance;
  if (!selected_performance || selected_performance->grids.empty()) {
    status->SetLabel("Load a valid OpenCPN boat .xml or polar .pol file first");
    return;
  }
  request.time_step_seconds = TimeStepSeconds();
  if (request.time_step_seconds == 0) {
    status->SetLabel("Time step must be between 5 minutes and 6 hours");
    return;
  }
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
  // Retain bounded inspection geometry independently of its current display
  // state. This makes both overlays true post-calculation toggles. A
  // single-candidate route captures every completed solver layer; departure
  // comparisons initially retain a coarser view for each candidate and
  // refresh the winner at full density after deterministic selection.
  request.inspection_interval_seconds = 1U;
  request.include_traces = true;
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
  if (!Number(land_safety_margin, &request.land_safety_margin_nautical_miles) ||
      request.land_safety_margin_nautical_miles < 0.0 ||
      request.land_safety_margin_nautical_miles > 20.0) {
    status->SetLabel("Land safety margin must be between 0 and 20 NM");
    return;
  }
  if (!Number(minimum_chart_depth, &request.minimum_chart_depth_metres) ||
      request.minimum_chart_depth_metres < 0.0 ||
      request.minimum_chart_depth_metres > 100.0) {
    status->SetLabel("Minimum charted depth must be between 0 and 100 m");
    return;
  }
  request.require_authoritative_chart_safety =
      require_authoritative_chart_safety->GetValue();
  if (use_currents->GetValue() && limit_opposing_wind_current->GetValue() &&
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
    if (!Number(motor_threshold, &request.motor_below_sailing_speed_knots) ||
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
        (!Number(motor_sailing_boost, &request.motor_sailing_boost_knots) ||
         request.motor_sailing_boost_knots < 0.0 ||
         request.motor_sailing_boost_knots > 30.0)) {
      status->SetLabel(
          "Motor-sailing speed increase must be between 0 and 30 kt");
      return;
    }
    if (!Number(motor_hysteresis, &request.motor_crossover_hysteresis_knots) ||
        request.motor_crossover_hysteresis_knots < 0.0 ||
        request.motor_crossover_hysteresis_knots > 10.0) {
      status->SetLabel(
          "Propulsion crossover hysteresis must be between 0 and 10 kt");
      return;
    }
    request.minimum_motor_run_seconds = minimum_motor_run->GetValue();
    request.mode_change_penalty_seconds = mode_change_penalty->GetValue();
    if (!Number(fuel_consumption, &request.fuel_consumption_litres_per_hour) ||
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
  if (use_currents->GetValue() && limit_opposing_wind_current->GetValue())
    request.limits_available |= 8;
  const std::vector<int64_t> departure_offsets =
      compare_departures->GetValue()
          ? PortableDepartureOffsetsSeconds(departure_window->GetValue(),
                                            departure_spacing->GetValue())
          : std::vector<int64_t>{0};
  if (departure_offsets.empty()) {
    status->SetLabel(wxString::Format(
        "The selected range and spacing produce more than %zu departure "
        "candidates; increase the spacing or reduce the range",
        kPortableMaximumDepartureCandidates));
    return;
  }
  const unsigned run_count = static_cast<unsigned>(departure_offsets.size());
  if (run_count > 1) request.inspection_interval_seconds = 2U * 60U * 60U;
  const unsigned parallel_worker_limit = departure_workers->GetValue();
  wxString begin_error;
  if (!begin_route_attempt || !begin_route_attempt(&begin_error)) {
    status->SetLabel("Could not start routing: " +
                     (begin_error.empty()
                          ? wxString("routing runtime is unavailable")
                          : begin_error));
    return;
  }
  SaveSettings();
  cancelled.store(false);
  calculation_running = true;
  ClearRoutingResults("Calculation in progress…");
  routing_result_gates = routing_gates;
  UpdateRoutingActionState();
  status->SetLabel("Checking iGRIB coverage for requested departures…");
  PopulateManagerRouting("Calculating");
  departure_runs.store(run_count);
  departures_completed.store(0);
  departures_running.store(0);
  departure_progress_floor.store(0);
  nominal_departure_unix_time = request.departure_unix_time;
  departure_result_rows.assign(run_count, DepartureResult{});
  for (size_t index = 0; index < departure_offsets.size(); ++index) {
    departure_result_rows[index].requested_departure_unix_time =
        request.departure_unix_time + departure_offsets[index];
    departure_result_rows[index].stage = "Queued";
  }
  PopulateDepartureResults();
  worker = std::thread([this, request, departure_offsets, parallel_worker_limit,
                        selected_performance,
                        routing_gates = std::move(routing_gates)] {
    ppm::DepartureSearchPlan plan;
    plan.nominal_departure_unix_time = request.departure_unix_time;
    plan.offsets_seconds = departure_offsets;
    plan.requested_maximum_workers = parallel_worker_limit;
    ppm::WeatherRoutingDepartureCoordinator coordinator;
    auto search = coordinator.Run(
        plan,
        [this, request](const std::vector<int64_t>& departure_times,
                        std::vector<uint8_t>* availability,
                        std::string* error) {
          wxString preflight_error;
          const bool okay = preflight_environment(
              request.start_latitude, request.start_longitude, departure_times,
              availability, &preflight_error);
          if (error) *error = preflight_error.ToStdString();
          return okay;
        },
        [this, request, selected_performance, &routing_gates](
            size_t candidate_index, int64_t departure, int64_t offset,
            const ppm::DepartureCandidateProgress& report,
            ppm::RoutingOutcome* outcome, std::string* error) {
          (void)candidate_index;
          auto candidate_request = request;
          candidate_request.departure_unix_time = departure;
          struct ProgressScope {
            explicit ProgressScope(
                const ppm::DepartureCandidateProgress* progress)
                : previous(active_departure_progress) {
              active_departure_progress = progress;
            }
            ~ProgressScope() { active_departure_progress = previous; }
            const ppm::DepartureCandidateProgress* previous;
          } progress_scope(&report);
          return CalculateContinuousPassage(
              calculate_passage, candidate_request, *selected_performance,
              routing_gates, offset, outcome, error);
        },
        [this](const ppm::DepartureSearchProgress& progress) {
          const auto live = alive;
          wxTheApp->CallAfter([this, live, progress] {
            if (!live->load() || stopped.load()) return;
            ApplyDepartureProgress(progress);
          });
        },
        cancelled);

    std::vector<DepartureResult> results(search.candidates.size());
    for (size_t index = 0; index < search.candidates.size(); ++index) {
      auto& destination = results[index];
      auto& source = search.candidates[index];
      destination.requested_departure_unix_time =
          source.summary.departure_unix_time;
      destination.status = HostCandidateStatus(source.summary.state);
      destination.percent = source.summary.percent;
      destination.stage = wxString::FromUTF8(source.summary.stage);
      destination.error = wxString::FromUTF8(source.summary.diagnostic);
      destination.success = source.success;
      if (source.success)
        destination.outcome = HostOutcome(std::move(source.outcome));
    }

    if (!cancelled.load() && search.best_index &&
        results.size() > *search.best_index) {
      const size_t inspection_index = *search.best_index;
      if (results[inspection_index].success) {
        auto inspection_request = request;
        inspection_request.departure_unix_time =
            results[inspection_index].requested_departure_unix_time;
        inspection_request.inspection_interval_seconds = 1U;
        inspection_request.include_traces = true;
        ppm::RoutingOutcome inspected_passage;
        std::string inspection_error;
        if (CalculateContinuousPassage(calculate_passage, inspection_request,
                                       *selected_performance, routing_gates,
                                       inspection_request.departure_unix_time -
                                           request.departure_unix_time,
                                       &inspected_passage, &inspection_error) &&
            inspected_passage.points.size() >= 2 &&
            inspected_passage.route_environment.size() ==
                inspected_passage.points.size() &&
            inspected_passage.duration_seconds > 0 &&
            inspected_passage.validation_samples > 0) {
          results[inspection_index].outcome =
              HostOutcome(std::move(inspected_passage));
        } else {
          results[inspection_index].outcome.diagnostic +=
              " Selected-route isochrone refresh was unavailable: " +
              (inspection_error.empty()
                   ? wxString("the refreshed passage was invalid")
                   : wxString::FromUTF8(inspection_error));
        }
      }
    }

    const auto live = alive;
    wxTheApp->CallAfter([this, live, results = std::move(results),
                         nominal_departure = request.departure_unix_time,
                         best_index = search.best_index]() mutable {
      if (!live->load()) return;
      Finish(std::move(results), nominal_departure, best_index);
    });
  });
}

void PortableWeatherRoutingHost::Impl::ApplyDepartureProgress(
    const ppm::DepartureSearchProgress& progress) {
  if (!calculation_running) return;
  departures_completed.store(progress.completed);
  departures_running.store(progress.running);
  departure_runs.store(static_cast<unsigned>(progress.candidates.size()));
  if (departure_result_rows.size() != progress.candidates.size())
    departure_result_rows.resize(progress.candidates.size());
  unsigned aggregate = 0;
  wxString active;
  for (size_t index = 0; index < progress.candidates.size(); ++index) {
    const auto& source = progress.candidates[index];
    auto& destination = departure_result_rows[index];
    destination.requested_departure_unix_time = source.departure_unix_time;
    destination.status = HostCandidateStatus(source.state);
    destination.percent = source.percent;
    destination.stage = wxString::FromUTF8(source.stage);
    destination.error = wxString::FromUTF8(source.diagnostic);
    const bool terminal =
        source.state == ppm::DepartureCandidateState::kCompleted ||
        source.state == ppm::DepartureCandidateState::kFailed ||
        source.state == ppm::DepartureCandidateState::kCancelled;
    aggregate += terminal ? 100U : source.percent;
    if (source.state == ppm::DepartureCandidateState::kRunning) {
      if (!active.empty()) active += " · ";
      active += FormatOffset(source.offset_seconds) + " " + destination.stage;
    }
  }
  PopulateDepartureResults();
  const unsigned count =
      std::max(1U, static_cast<unsigned>(progress.candidates.size()));
  gauge->SetValue(std::min(99U, aggregate / count));
  status->SetLabel(
      wxString::Format("Departure optimisation — %u/%u complete · %u active "
                       "· %u queued%s%s",
                       progress.completed, count, progress.running,
                       progress.queued, active.empty() ? "" : " — ", active));
}

void PortableWeatherRoutingHost::Impl::Finish(
    std::vector<DepartureResult> results, int64_t nominal_departure,
    std::optional<size_t> best_index) {
  if (worker.joinable()) worker.join();
  calculation_running = false;
  departure_result_rows = std::move(results);
  nominal_departure_unix_time = nominal_departure;
  best_departure_result = best_index &&
                                  *best_index < departure_result_rows.size() &&
                                  departure_result_rows[*best_index].success
                              ? *best_index
                              : std::numeric_limits<size_t>::max();
  wxString last_error = "No departure produced a route";
  for (size_t index = 0; index < departure_result_rows.size(); ++index) {
    const auto& result = departure_result_rows[index];
    if (!result.success) {
      if (!result.error.empty()) last_error = result.error;
      continue;
    }
    if (best_departure_result == std::numeric_limits<size_t>::max()) {
      best_departure_result = index;
    }
  }
  PopulateDepartureResults();
  if (best_departure_result == std::numeric_limits<size_t>::max()) {
    UpdateRoutingActionState();
    status->SetLabel("Failed: " + last_error);
    metrics->SetLabel("No successful departure route");
    PopulateManagerRouting("Failed");
    if (validation_diagnostics) validation_diagnostics->SetValue(last_error);
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
          row, 6,
          wxString::Format(
              "Complete · %zu leg%s · %llu validation samples",
              outcome.passage_legs.size(),
              outcome.passage_legs.size() == 1 ? "" : "s",
              static_cast<unsigned long long>(outcome.validation_samples)));
    } else {
      for (int column = 3; column <= 5; ++column)
        departure_results->SetItem(row, column, "N/A");
      const wxString state =
          result.status == DepartureCandidateStatus::kQueued      ? "Queued"
          : result.status == DepartureCandidateStatus::kRunning   ? "Running"
          : result.status == DepartureCandidateStatus::kCompleted ? "Complete"
          : result.status == DepartureCandidateStatus::kCancelled ? "Cancelled"
                                                                  : "Failed";
      const wxString progress =
          result.percent > 0 && result.percent < 100
              ? wxString::Format(" · %u%%", result.percent)
              : wxString();
      departure_results->SetItem(
          row, 6,
          state +
              (result.stage.empty() || result.stage == state
                   ? wxString()
                   : " · " + result.stage) +
              progress +
              (result.error.empty() ? wxString() : " · " + result.error));
    }
  }
  for (int column = 0; column < 7; ++column)
    departure_results->SetColumnWidth(column, wxLIST_AUTOSIZE_USEHEADER);
}

void PortableWeatherRoutingHost::Impl::ResetStabilityCorridor() {
  stability_generation.fetch_add(1);
  stability_corridor.reset();
}

const ppm::StabilityRouteFamily*
PortableWeatherRoutingHost::Impl::SelectedStabilityFamily() const {
  if (!stability_corridor || !stability_corridor->success ||
      selected_departure_result == std::numeric_limits<size_t>::max())
    return nullptr;
  const auto family_id =
      ppm::FindStabilityFamily(*stability_corridor, selected_departure_result);
  if (!family_id) return nullptr;
  const auto family = std::find_if(
      stability_corridor->families.begin(), stability_corridor->families.end(),
      [family_id](const ppm::StabilityRouteFamily& candidate) {
        return candidate.id == *family_id;
      });
  return family == stability_corridor->families.end() ? nullptr : &*family;
}

void PortableWeatherRoutingHost::Impl::EnsureStabilityCorridor() {
  if (stopped.load() || calculation_running || stability_corridor ||
      stability_running.load())
    return;
  if (departure_result_rows.size() < 3) {
    if (!departure_result_rows.empty() && status)
      status->SetLabel(
          "Stability corridor unavailable: at least three validated "
          "departure candidates are required");
    return;
  }
  if (stability_worker.joinable()) stability_worker.join();

  std::vector<ppm::StabilityRoute> routes;
  routes.reserve(departure_result_rows.size());
  for (size_t index = 0; index < departure_result_rows.size(); ++index) {
    const auto& candidate = departure_result_rows[index];
    ppm::StabilityRoute route_candidate;
    route_candidate.id = std::to_string(index);
    route_candidate.departure_unix_time =
        candidate.requested_departure_unix_time;
    route_candidate.duration_seconds = candidate.outcome.duration_seconds;
    route_candidate.complete = candidate.success;
    route_candidate.independently_validated =
        candidate.success && candidate.outcome.validation_samples > 0;
    route_candidate.points.reserve(candidate.outcome.points.size());
    for (const auto& point : candidate.outcome.points)
      route_candidate.points.push_back({point.latitude, point.longitude});
    routes.push_back(std::move(route_candidate));
  }

  ppm::ChartSafetyServiceOptions chart_options;
  Number(land_safety_margin, &chart_options.safety_margin_nautical_miles);
  Number(minimum_chart_depth, &chart_options.minimum_depth_metres);
  chart_options.require_authoritative =
      require_authoritative_chart_safety->GetValue();
  const auto options = stability_options;
  const std::uint64_t generation = stability_generation.load();
  stability_running.store(true);
  if (status)
    status->SetLabel(
        "Calculating validated departure-route stability corridor…");
  stability_worker = std::thread([this, generation, options, chart_options,
                                  routes = std::move(routes)]() mutable {
    const auto safety = [chart_options](
                            const std::vector<ppm::StabilityCell>& cells) {
      constexpr size_t kSegmentsPerCell = 6;
      std::vector<ocpn_portable_geo_segment> segments;
      segments.reserve(cells.size() * kSegmentsPerCell);
      for (const auto& cell : cells) {
        const ocpn_portable_geo_point southwest{cell.minimum_latitude,
                                                cell.minimum_longitude};
        const ocpn_portable_geo_point southeast{cell.minimum_latitude,
                                                cell.maximum_longitude};
        const ocpn_portable_geo_point northwest{cell.maximum_latitude,
                                                cell.minimum_longitude};
        const ocpn_portable_geo_point northeast{cell.maximum_latitude,
                                                cell.maximum_longitude};
        segments.push_back({southwest, southeast});
        segments.push_back({southeast, northeast});
        segments.push_back({northeast, northwest});
        segments.push_back({northwest, southwest});
        segments.push_back({southwest, northeast});
        segments.push_back({southeast, northwest});
      }
      const auto checked =
          ppm::ChartSafetyService().QuerySemantic(segments, chart_options);
      std::vector<ppm::StabilityCellSafety> result(
          cells.size(), ppm::StabilityCellSafety::kMissing);
      if (checked.size() != segments.size()) return result;
      for (size_t cell = 0; cell < cells.size(); ++cell) {
        bool unsafe = false;
        bool error = false;
        bool missing = false;
        for (size_t segment = 0; segment < kSegmentsPerCell; ++segment) {
          const auto state = checked[cell * kSegmentsPerCell + segment].state;
          unsafe = unsafe || state == 1U;
          missing = missing || state == 2U;
          error = error || state == 3U;
        }
        result[cell] = unsafe    ? ppm::StabilityCellSafety::kUnsafe
                       : error   ? ppm::StabilityCellSafety::kError
                       : missing ? ppm::StabilityCellSafety::kMissing
                                 : ppm::StabilityCellSafety::kSafe;
      }
      return result;
    };
    ppm::StabilityCorridorResult corridor;
    try {
      corridor = ppm::BuildStabilityCorridor(routes, options, safety);
    } catch (const std::exception& error) {
      corridor.failure =
          std::string("stability analysis failed safely: ") + error.what();
    } catch (...) {
      corridor.failure =
          "stability analysis failed safely with an unknown exception";
    }
    const auto live = alive;
    wxTheApp->CallAfter([this, live, generation,
                         corridor = std::move(corridor)]() mutable {
      if (!live->load() || stopped.load()) return;
      stability_running.store(false);
      if (generation != stability_generation.load()) {
        if (show_stability_corridor && show_stability_corridor->GetValue())
          EnsureStabilityCorridor();
        return;
      }
      stability_corridor = std::move(corridor);
      if (stability_corridor->success) {
        const auto* family = SelectedStabilityFamily();
        status->SetLabel(
            family
                ? wxString::Format("Stability corridor ready — %zu comparable "
                                   "routes · %.1f NM median / %.1f NM maximum "
                                   "family width",
                                   family->route_indices.size(),
                                   family->median_width_nautical_miles,
                                   family->maximum_width_nautical_miles)
                : "Stability corridor ready; the selected departure "
                  "belongs to a different route family");
      } else {
        status->SetLabel(wxString::Format(
            "Stability corridor unavailable: %s · %zu unsafe and "
            "%zu unresolved chart cell%s omitted",
            wxString::FromUTF8(stability_corridor->failure),
            stability_corridor->unsafe_cells_excluded,
            stability_corridor->unresolved_cells_excluded,
            stability_corridor->unresolved_cells_excluded == 1 ? "" : "s"));
      }
      if (parent) parent->Refresh(false);
      RequestRefresh(GetOCPNCanvasWindow());
    });
  });
}

void PortableWeatherRoutingHost::Impl::SelectDepartureResult(size_t index) {
  if (index >= departure_result_rows.size() ||
      !departure_result_rows[index].success)
    return;
  selected_departure_result = index;
  const auto& selected = departure_result_rows[index].outcome;
  if (candidate_details) {
    const wxString comfort = selected.comfort_level == 1   ? "Good"
                             : selected.comfort_level == 2 ? "Bumpy"
                             : selected.comfort_level == 3 ? "Difficult"
                                                           : "N/A";
    candidate_details->SetLabel(wxString::Format(
        "Average boat speed %.1f kt · average/max SOG %.1f/%.1f kt · "
        "average/max wind %.1f/%.1f kt\n"
        "Average/max current %s/%s · tacks/gybes %u · propulsion %s · "
        "fuel %s · mode changes %s · comfort %s\n"
        "%u states examined · %zu retained isochrones · %zu passage leg%s · "
        "%llu independent validation samples",
        selected.average_speed_knots, selected.average_sog_knots,
        selected.maximum_sog_knots, selected.average_wind_knots,
        selected.maximum_wind_knots,
        selected.current_metrics_available
            ? wxString::Format("%.1f kt", selected.average_current_knots)
            : wxString("N/A"),
        selected.current_metrics_available
            ? wxString::Format("%.1f kt", selected.maximum_current_knots)
            : wxString("N/A"),
        selected.tacks,
        selected.fuel_metrics_available ? FormatElapsed(selected.motor_seconds)
                                        : wxString("N/A"),
        selected.fuel_metrics_available
            ? wxString::Format("%.1f L", selected.estimated_fuel_litres)
            : wxString("N/A"),
        selected.fuel_metrics_available
            ? wxString::Format("%u", selected.propulsion_transitions)
            : wxString("N/A"),
        comfort, selected.states_examined, selected.isochrones.size(),
        selected.passage_legs.size(),
        selected.passage_legs.size() == 1 ? "" : "s",
        static_cast<unsigned long long>(selected.validation_samples)));
    candidate_details->Wrap(candidate_details->GetParent()->FromDIP(700));
    candidate_details->GetParent()->Layout();
  }
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
        FormatElapsed(selected.motor_seconds), selected.estimated_fuel_litres,
        selected.propulsion_transitions);
  }
  metrics->SetLabel(wxString::Format(
      "%s%s · %zu passage leg%s · %zu points · %zu isochrones · %.1f NM · "
      "%.1f hours · %u states · %llu validation samples · departure %s%s",
      index == best_departure_result ? "Best passage · "
                                     : "Selected passage · ",
      FormatElapsed(selected.duration_seconds), selected.passage_legs.size(),
      selected.passage_legs.size() == 1 ? "" : "s", route.size(),
      isochrones.size(), selected.distance_nautical_miles,
      selected.duration_seconds / 3600.0, selected.states_examined,
      static_cast<unsigned long long>(selected.validation_samples),
      FormatRoutingTime(selected.departure_unix_time), propulsion_metrics));
  UpdateRoutingActionState();
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
    const size_t stride = std::max<size_t>(
        1, (route.size() + kMaximumScheduleRows - 1) / kMaximumScheduleRows);
    for (size_t point_index = 0; point_index < route.size();
         point_index += stride) {
      const auto& point = route[point_index];
      const auto& next =
          route[std::min(point_index + stride, route.size() - 1)];
      const int64_t seconds =
          std::max<int64_t>(1, next.unix_time - point.unix_time);
      const double course = InitialBearingDegrees(
          point.latitude, point.longitude, next.latitude, next.longitude);
      const double sog =
          next.unix_time > point.unix_time
              ? GreatCircleNauticalMiles(point.latitude, point.longitude,
                                         next.latitude, next.longitude) *
                    3600.0 / static_cast<double>(seconds)
              : 0.0;
      const long row = route_schedule->InsertItem(
          route_schedule->GetItemCount(), FormatRoutingTime(point.unix_time));
      route_schedule->SetItem(row, 1, wxString::Format("%.5f", point.latitude));
      route_schedule->SetItem(row, 2,
                              wxString::Format("%.5f", point.longitude));
      route_schedule->SetItem(row, 3, wxString::Format("%03.0f°", course));
      route_schedule->SetItem(row, 4, wxString::Format("%.1f kt", sog));
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
          const double current_speed = std::hypot(environment.current_u_knots,
                                                  environment.current_v_knots);
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
      route_schedule->SetItem(row, 1, wxString::Format("%.5f", point.latitude));
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
  if (show_stability_corridor && show_stability_corridor->GetValue())
    EnsureStabilityCorridor();
}

void PortableWeatherRoutingHost::Impl::ReportProgress(unsigned percent,
                                                      const wxString& message) {
  if (active_departure_progress) {
    (*active_departure_progress)(percent, message.ToStdString());
    return;
  }
  if (!frame || stopped.load()) return;
  const unsigned runs = std::max(1u, departure_runs.load());
  const unsigned completed = std::min(departures_completed.load(), runs);
  const unsigned running =
      std::min(departures_running.load(), runs - completed);
  const unsigned queued = runs - completed - running;
  unsigned overall =
      runs > 1
          ? std::min(99u, (completed * 100 + std::min(percent, 99u)) / runs)
          : percent;
  unsigned floor = departure_progress_floor.load();
  while (overall > floor &&
         !departure_progress_floor.compare_exchange_weak(floor, overall)) {
  }
  overall = std::max(overall, floor);
  const wxString labelled =
      runs > 1 ? wxString::Format(
                     "Departure optimisation — %u/%u complete · %u active · "
                     "%u queued — %s",
                     completed, runs, running, queued, message)
               : message;
  const auto live = alive;
  wxTheApp->CallAfter([this, live, overall, labelled] {
    if (!live->load() || !frame || stopped.load()) return;
    gauge->SetValue(overall);
    status->SetLabel(labelled);
  });
}

const RoutingOutcome::InspectionLine*
PortableWeatherRoutingHost::Impl::FindCursorTrace(
    PlugIn_ViewPort* viewport) const {
  if (!viewport || !route_to_cursor || !route_to_cursor->GetValue() ||
      !cursor_position)
    return nullptr;
  PortableNavigationPosition cursor;
  if (!cursor_position(&cursor)) return nullptr;
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
  return closest;
}

ppm::IsochroneDisplayPlan PortableWeatherRoutingHost::Impl::IsochronePlan(
    PlugIn_ViewPort* viewport,
    const RoutingOutcome::InspectionLine* cursor_trace) const {
  std::vector<std::int64_t> line_times;
  line_times.reserve(isochrones.size());
  for (const auto& contour : isochrones)
    line_times.push_back(contour.unix_time);
  std::optional<std::int64_t> environment_time;
  std::int64_t displayed_time = 0;
  if (displayed_environment_time && displayed_environment_time(&displayed_time))
    environment_time = displayed_time;
  const std::optional<std::int64_t> cursor_time =
      cursor_trace ? std::optional<std::int64_t>(cursor_trace->unix_time)
                   : std::nullopt;
  return ppm::BuildIsochroneDisplayPlan(
      line_times, route.empty() ? 0 : route.front().unix_time,
      route.empty() ? 0 : route.back().unix_time, isochrone_display,
      environment_time, cursor_time, ProjectedRoutePixels(route, viewport));
}

void PortableWeatherRoutingHost::Impl::SetColorScheme(int scheme) {
  colour_scheme = scheme;
  if (parent) parent->Refresh(false);
}

bool PortableWeatherRoutingHost::Impl::Render(wxDC& dc,
                                              PlugIn_ViewPort* viewport) {
  if (!viewport || route.size() < 2) return false;
  const IsochronePalette palette = PaletteForScheme(colour_scheme);
  const RoutingOutcome::InspectionLine* cursor_trace =
      FindCursorTrace(viewport);
  const ppm::IsochroneDisplayPlan display_plan =
      IsochronePlan(viewport, cursor_trace);
  std::vector<std::int64_t> label_times;
  const ppm::StabilityRenderPlan stability_plan =
      show_stability_corridor && show_stability_corridor->GetValue() &&
              stability_corridor
          ? ppm::BuildStabilityRenderPlan(
                *stability_corridor, selected_departure_result,
                // OpenCPN presents this renderer-neutral wxDC overlay in
                // both software and Vulkan canvases.
                ppm::StabilityRenderBackend::kSoftware)
          : ppm::StabilityRenderPlan{};
  const ppm::StabilityRouteFamily* stability_family = stability_plan.family;
  if (stability_family) {
    dc.SetPen(*wxTRANSPARENT_PEN);
    auto draw_cells = [&dc, viewport](
                          const std::vector<ppm::StabilityCell>& cells,
                          const wxColour& colour) {
      dc.SetBrush(wxBrush(colour));
      for (const auto& cell : cells) {
        wxPoint polygon[4] = {
            Project(viewport, cell.minimum_latitude, cell.minimum_longitude),
            Project(viewport, cell.minimum_latitude, cell.maximum_longitude),
            Project(viewport, cell.maximum_latitude, cell.maximum_longitude),
            Project(viewport, cell.maximum_latitude, cell.minimum_longitude)};
        dc.DrawPolygon(4, polygon);
      }
    };
    draw_cells(*stability_plan.outer_cells, wxColour(200, 80, 180, 42));
    draw_cells(*stability_plan.inner_cells, wxColour(170, 55, 205, 82));
    if (stability_plan.representative_route_index <
        departure_result_rows.size()) {
      const auto& representative =
          departure_result_rows[stability_plan.representative_route_index]
              .outcome.points;
      std::vector<wxPoint> line;
      line.reserve(representative.size());
      for (const auto& point : representative)
        line.push_back(Project(viewport, point.latitude, point.longitude));
      if (line.size() >= 2) {
        dc.SetPen(wxPen(wxColour(125, 35, 160, 210), 2, wxPENSTYLE_SHORT_DASH));
        dc.DrawLines(static_cast<int>(line.size()), line.data());
      }
    }
  }
  if (show_isochrones && show_isochrones->GetValue()) {
    for (std::size_t index = 0;
         index < isochrones.size() && index < display_plan.lines.size();
         ++index) {
      const auto& contour = isochrones[index];
      const auto& style = display_plan.lines[index];
      if (!style.draw) continue;
      if (contour.points.size() < 2) continue;
      const wxColour colour = IsochroneColour(palette, isochrone_display, style,
                                              cursor_trace != nullptr);
      const int width = std::max(1, static_cast<int>(std::lround(IsochroneWidth(
                                        isochrone_display, style))));
      dc.SetPen(wxPen(colour, width));
      dc.SetBrush(wxBrush(colour));
      std::vector<wxPoint> line;
      line.reserve(contour.points.size());
      for (const auto& point : contour.points)
        line.push_back(Project(viewport, point.latitude, point.longitude));
      dc.DrawLines(static_cast<int>(line.size()), line.data());
      if (isochrone_display.show_front_points)
        for (const auto& point : line) dc.DrawCircle(point, 2);
      if (style.label) label_times.push_back(contour.unix_time);
    }
  }
  dc.SetPen(wxPen(palette.major, 2));
  for (const auto& alternative : alternative_routes) {
    if (alternative.size() < 2) continue;
    std::vector<wxPoint> comparison;
    comparison.reserve(alternative.size());
    for (const auto& point : alternative)
      comparison.push_back(Project(viewport, point.latitude, point.longitude));
    dc.DrawLines(static_cast<int>(comparison.size()), comparison.data());
  }
  std::vector<wxPoint> points;
  points.reserve(route.size());
  for (const auto& point : route)
    points.push_back(Project(viewport, point.latitude, point.longitude));
  dc.SetPen(wxPen(palette.route_halo, 7));
  dc.DrawLines(static_cast<int>(points.size()), points.data());
  dc.SetPen(wxPen(palette.route, 4));
  dc.DrawLines(static_cast<int>(points.size()), points.data());
  dc.SetPen(wxPen(palette.route_halo, 2));
  dc.SetBrush(wxBrush(palette.route));
  for (const auto& [index, gate] : SelectedPassageGates()) {
    (void)index;
    dc.DrawCircle(Project(viewport, gate.latitude, gate.longitude), 6);
  }

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
                        environment.wind_v_knots, palette.major);
      previous = origin;
    }
  }

  for (const std::int64_t label_time : label_times) {
    double latitude = 0.0;
    double longitude = 0.0;
    if (InterpolateRoutePosition(route, label_time, &latitude, &longitude))
      DrawIsochroneLabel(
          dc, Project(viewport, latitude, longitude),
          IsochroneTimeLabel(route.front().unix_time, label_time), palette);
  }

  if (cursor_trace) {
    std::vector<wxPoint> inspection;
    inspection.reserve(cursor_trace->points.size());
    for (const auto& point : cursor_trace->points)
      inspection.push_back(Project(viewport, point.latitude, point.longitude));
    dc.SetPen(wxPen(palette.route_halo, 6, wxPENSTYLE_SHORT_DASH));
    dc.DrawLines(static_cast<int>(inspection.size()), inspection.data());
    dc.SetPen(wxPen(palette.cursor, 3, wxPENSTYLE_SHORT_DASH));
    dc.DrawLines(static_cast<int>(inspection.size()), inspection.data());
    dc.SetPen(wxPen(palette.route_halo, 2));
    dc.SetBrush(wxBrush(palette.cursor));
    dc.DrawCircle(inspection.back(), 6);
  }

  if (boat_at_grib_time && boat_at_grib_time->GetValue() &&
      displayed_environment_time) {
    int64_t display_time = 0;
    double boat_latitude = 0.0;
    double boat_longitude = 0.0;
    if (displayed_environment_time(&display_time) &&
        InterpolateRoutePosition(route, display_time, &boat_latitude,
                                 &boat_longitude)) {
      const wxPoint boat = Project(viewport, boat_latitude, boat_longitude);
      dc.SetPen(wxPen(palette.route_halo, 3));
      dc.SetBrush(wxBrush(palette.route));
      dc.DrawCircle(boat, 8);
      dc.SetPen(wxPen(palette.major, 1));
      dc.DrawCircle(boat, 4);
    }
  }
  return true;
}

bool PortableWeatherRoutingHost::Impl::RenderGL(PlugIn_ViewPort* viewport) {
  if (!viewport || route.size() < 2) return false;
  const IsochronePalette palette = PaletteForScheme(colour_scheme);
  const RoutingOutcome::InspectionLine* cursor_trace =
      FindCursorTrace(viewport);
  const ppm::IsochroneDisplayPlan display_plan =
      IsochronePlan(viewport, cursor_trace);
  std::vector<std::int64_t> label_times;
  auto vertex = [viewport](double latitude, double longitude) {
    const wxPoint pixel = Project(viewport, latitude, longitude);
    glVertex2i(pixel.x, pixel.y);
  };
  auto line = [&vertex](const std::vector<ocpn_portable_route_point>& points,
                        GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha,
                        GLfloat width) {
    if (points.size() < 2) return;
    glColor4ub(red, green, blue, alpha);
    glLineWidth(width);
    glBegin(GL_LINE_STRIP);
    for (const auto& point : points) vertex(point.latitude, point.longitude);
    glEnd();
  };

  glPushAttrib(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT | GL_LINE_BIT |
               GL_CURRENT_BIT | GL_POINT_BIT | GL_POLYGON_BIT);
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  const ppm::StabilityRenderPlan stability_plan =
      show_stability_corridor && show_stability_corridor->GetValue() &&
              stability_corridor
          ? ppm::BuildStabilityRenderPlan(*stability_corridor,
                                          selected_departure_result,
                                          ppm::StabilityRenderBackend::kOpenGL)
          : ppm::StabilityRenderPlan{};
  const ppm::StabilityRouteFamily* stability_family = stability_plan.family;
  if (stability_family) {
    auto draw_cells = [&vertex](const std::vector<ppm::StabilityCell>& cells,
                                GLubyte red, GLubyte green, GLubyte blue,
                                GLubyte alpha) {
      glColor4ub(red, green, blue, alpha);
      glBegin(GL_QUADS);
      for (const auto& cell : cells) {
        vertex(cell.minimum_latitude, cell.minimum_longitude);
        vertex(cell.minimum_latitude, cell.maximum_longitude);
        vertex(cell.maximum_latitude, cell.maximum_longitude);
        vertex(cell.maximum_latitude, cell.minimum_longitude);
      }
      glEnd();
    };
    draw_cells(*stability_plan.outer_cells, 200, 80, 180, 42);
    draw_cells(*stability_plan.inner_cells, 170, 55, 205, 82);
    if (stability_plan.representative_route_index <
        departure_result_rows.size()) {
      glEnable(GL_LINE_STIPPLE);
      glLineStipple(2, 0xF0F0);
      line(departure_result_rows[stability_plan.representative_route_index]
               .outcome.points,
           125, 35, 160, 210, 2.0F);
      glDisable(GL_LINE_STIPPLE);
    }
  }
  if (show_isochrones && show_isochrones->GetValue()) {
    for (std::size_t index = 0;
         index < isochrones.size() && index < display_plan.lines.size();
         ++index) {
      const auto& contour = isochrones[index];
      const auto& style = display_plan.lines[index];
      if (!style.draw || contour.points.size() < 2) continue;
      const wxColour colour = IsochroneColour(palette, isochrone_display, style,
                                              cursor_trace != nullptr);
      line(contour.points, colour.Red(), colour.Green(), colour.Blue(),
           colour.Alpha(),
           static_cast<GLfloat>(IsochroneWidth(isochrone_display, style)));
      if (isochrone_display.show_front_points) {
        glColor4ub(colour.Red(), colour.Green(), colour.Blue(), colour.Alpha());
        glPointSize(3.0F);
        glBegin(GL_POINTS);
        for (const auto& point : contour.points)
          vertex(point.latitude, point.longitude);
        glEnd();
      }
      if (style.label) label_times.push_back(contour.unix_time);
    }
  }
  for (const auto& alternative : alternative_routes)
    line(alternative, palette.major.Red(), palette.major.Green(),
         palette.major.Blue(), 210, 2.0F);
  line(route, palette.route_halo.Red(), palette.route_halo.Green(),
       palette.route_halo.Blue(), 220, 7.0F);
  line(route, palette.route.Red(), palette.route.Green(), palette.route.Blue(),
       255, 4.0F);
  for (const auto& [index, gate] : SelectedPassageGates()) {
    (void)index;
    const wxPoint centre = Project(viewport, gate.latitude, gate.longitude);
    glColor4ub(palette.route.Red(), palette.route.Green(), palette.route.Blue(),
               255);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2i(centre.x, centre.y);
    for (int angle_index = 0; angle_index <= 16; ++angle_index) {
      const double angle = static_cast<double>(angle_index) * 2.0 *
                           3.14159265358979323846 / 16.0;
      glVertex2i(centre.x + static_cast<int>(std::lround(std::cos(angle) * 6)),
                 centre.y + static_cast<int>(std::lround(std::sin(angle) * 6)));
    }
    glEnd();
  }

  if (show_route_wind && show_route_wind->GetValue()) {
    wxPoint previous(std::numeric_limits<int>::min(),
                     std::numeric_limits<int>::min());
    glColor4ub(palette.major.Red(), palette.major.Green(), palette.major.Blue(),
               255);
    glLineWidth(2.0F);
    for (const auto& environment : route_environment) {
      const wxPoint origin =
          Project(viewport, environment.latitude, environment.longitude);
      const long dx = static_cast<long>(origin.x) - previous.x;
      const long dy = static_cast<long>(origin.y) - previous.y;
      if (previous.x != std::numeric_limits<int>::min() &&
          dx * dx + dy * dy < 45L * 45L)
        continue;
      DrawRouteWindBarbGl(origin, environment.wind_u_knots,
                          environment.wind_v_knots);
      previous = origin;
    }
  }

  for (const std::int64_t label_time : label_times) {
    double latitude = 0.0;
    double longitude = 0.0;
    if (InterpolateRoutePosition(route, label_time, &latitude, &longitude))
      DrawGlIsochroneLabel(
          Project(viewport, latitude, longitude),
          IsochroneTimeLabel(route.front().unix_time, label_time), palette);
  }

  if (cursor_trace) {
    glEnable(GL_LINE_STIPPLE);
    glLineStipple(2, 0xF0F0);
    line(cursor_trace->points, palette.route_halo.Red(),
         palette.route_halo.Green(), palette.route_halo.Blue(), 230, 6.0F);
    line(cursor_trace->points, palette.cursor.Red(), palette.cursor.Green(),
         palette.cursor.Blue(), 255, 3.0F);
    glDisable(GL_LINE_STIPPLE);
    const auto& endpoint = cursor_trace->points.back();
    const wxPoint centre =
        Project(viewport, endpoint.latitude, endpoint.longitude);
    glColor4ub(palette.cursor.Red(), palette.cursor.Green(),
               palette.cursor.Blue(), 255);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2i(centre.x, centre.y);
    for (int index = 0; index <= 16; ++index) {
      const double angle =
          static_cast<double>(index) * 2.0 * 3.14159265358979323846 / 16.0;
      glVertex2i(centre.x + static_cast<int>(std::lround(std::cos(angle) * 6)),
                 centre.y + static_cast<int>(std::lround(std::sin(angle) * 6)));
    }
    glEnd();
  }

  if (boat_at_grib_time && boat_at_grib_time->GetValue() &&
      displayed_environment_time) {
    std::int64_t display_time = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    if (displayed_environment_time(&display_time) &&
        InterpolateRoutePosition(route, display_time, &latitude, &longitude)) {
      const wxPoint centre = Project(viewport, latitude, longitude);
      glColor4ub(palette.route.Red(), palette.route.Green(),
                 palette.route.Blue(), 255);
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

void PortableWeatherRoutingHost::Impl::CursorChanged() {
  if (route_to_cursor && route_to_cursor->GetValue() && !traces.empty() &&
      parent)
    parent->Refresh(false);
}

std::map<size_t, PortableNavigationPosition>
PortableWeatherRoutingHost::Impl::SelectedPassageGates() const {
  std::map<size_t, PortableNavigationPosition> result;
  if (selected_departure_result >= departure_result_rows.size() ||
      routing_result_gates.empty())
    return result;
  const auto& selected = departure_result_rows[selected_departure_result];
  if (!selected.success || selected.outcome.points.empty()) return result;
  result.emplace(0, routing_result_gates.front());
  for (const auto& leg : selected.outcome.passage_legs) {
    if (leg.end_gate_index >= routing_result_gates.size() ||
        leg.point_count == 0)
      continue;
    const size_t point_index = leg.point_offset + leg.point_count - 1;
    if (point_index < selected.outcome.points.size())
      result[point_index] = routing_result_gates[leg.end_gate_index];
  }
  return result;
}

bool PortableWeatherRoutingHost::Impl::ExportGpx() {
  if (route.empty()) return false;
  wxString filename = plugin_id.AfterLast('.') + "-route.gpx";
  wxFileDialog dialog(frame, "Export portable weather route", wxEmptyString,
                      filename, "GPX files (*.gpx)|*.gpx",
                      wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
  if (dialog.ShowModal() != wxID_OK) return false;
  wxFileOutputStream file(dialog.GetPath());
  if (!file.IsOk()) {
    status->SetLabel("Could not create the selected GPX file");
    return false;
  }
  wxTextOutputStream out(file);
  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" "
         "creator=\"OpenCPN portable routing runtime\" "
         "xmlns=\"http://www.topografix.com/GPX/1/"
         "1\"><rte><name>Portable weather-routing passage</name>\n";
  const auto passage_gates = SelectedPassageGates();
  for (size_t index = 0; index < route.size(); ++index) {
    const auto& point = route[index];
    const auto gate = passage_gates.find(index);
    const wxString name =
        gate == passage_gates.end()
            ? wxString()
            : "<name>" + XmlEscape(gate->second.name) + "</name>";
    out << wxString::Format(
        "<rtept lat=\"%.8f\" lon=\"%.8f\">%s<time>%s</time></rtept>\n",
        point.latitude, point.longitude, name,
        wxDateTime(static_cast<time_t>(point.unix_time))
                .ToUTC()
                .FormatISOCombined('T') +
            "Z");
  }
  out << "</rte></gpx>\n";
  status->SetLabel("GPX route exported");
  return true;
}

bool PortableWeatherRoutingHost::Impl::SendToOpenCpn() {
  if (route.size() < 2 || !create_route) return false;
  constexpr size_t kMaximumOpenCpnRoutePoints = 2000;
  const auto passage_gates = SelectedPassageGates();
  const size_t retained_gate_count =
      std::min(passage_gates.size(), kMaximumOpenCpnRoutePoints);
  const size_t available_samples =
      std::max<size_t>(1, kMaximumOpenCpnRoutePoints - retained_gate_count);
  const size_t stride = std::max<size_t>(
      1, (route.size() + available_samples - 1) / available_samples);
  std::set<size_t> retained_indices;
  for (size_t index = 0; index < route.size(); index += stride)
    retained_indices.insert(index);
  retained_indices.insert(route.size() - 1);
  for (const auto& [index, gate] : passage_gates) {
    (void)gate;
    retained_indices.insert(index);
  }
  std::vector<PortableNavigationPosition> points;
  points.reserve(retained_indices.size());
  for (const size_t index : retained_indices) {
    const auto& point = route[index];
    const auto gate = passage_gates.find(index);
    points.push_back({gate == passage_gates.end()
                          ? plugin_id + wxString::Format(":%zu", index)
                          : gate->second.id,
                      gate == passage_gates.end()
                          ? wxString::Format("Route point %zu", index)
                          : gate->second.name,
                      point.latitude, point.longitude});
  }
  if (passage_gates.empty()) {
    points.front().name = "Route start";
    points.back().name = "Route destination";
  }
  wxString error;
  const wxString name =
      surface_title + " " + FormatRoutingTime(route.front().unix_time);
  if (!create_route(name, points, &error)) {
    status->SetLabel("Could not add route to OpenCPN: " + error);
    return false;
  }
  status->SetLabel(
      "Selected planning route added to OpenCPN; review it before use");
  RefreshNavigationPositions(false);
  PopulateManagerPositions();
  return true;
}

void PortableWeatherRoutingHost::Impl::Shutdown() {
  if (stopped.exchange(true)) return;
  alive->store(false);
  cancelled.store(true);
  stability_generation.fetch_add(1);
  if (cancel_routes) cancel_routes();
  environment_refresh_timer.Stop();
  if (worker.joinable()) worker.join();
  if (stability_worker.joinable()) stability_worker.join();
  if (frame) {
    SaveSettings();
    delete routing_context_menu;
    routing_context_menu = nullptr;
    context_menu_items.clear();
    frame->Destroy();
    frame = nullptr;
  }
}

PortableWeatherRoutingHost::PortableWeatherRoutingHost(
    wxWindow* parent, wxFileConfig* config, CalculateRoute calculate_route,
    CalculatePassage calculate_passage, BeginRouteAttempt begin_route_attempt,
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
          parent, config, std::move(calculate_route),
          std::move(calculate_passage), std::move(begin_route_attempt),
          std::move(cancel_routes), package_root, plugin_id, surface_resource,
          std::move(summary), std::move(list_waypoints), std::move(list_routes),
          std::move(create_route), std::move(vessel_position),
          std::move(cursor_position), std::move(displayed_environment_time),
          std::move(preflight_environment), latitude, longitude)) {}
PortableWeatherRoutingHost::~PortableWeatherRoutingHost() = default;
bool PortableWeatherRoutingHost::Show(wxString* error) {
  return m_impl->Show(error);
}
bool PortableWeatherRoutingHost::ShowRouteAnalysis(const wxString& route_id,
                                                   wxString* error) {
  return m_impl->ShowRouteAnalysis(route_id, error);
}
bool PortableWeatherRoutingHost::Render(wxDC& dc, PlugIn_ViewPort* viewport) {
  return m_impl->Render(dc, viewport);
}
bool PortableWeatherRoutingHost::RenderGL(PlugIn_ViewPort* viewport) {
  return m_impl->RenderGL(viewport);
}
void PortableWeatherRoutingHost::SetColorScheme(int scheme) {
  m_impl->SetColorScheme(scheme);
}
void PortableWeatherRoutingHost::CursorChanged() { m_impl->CursorChanged(); }
void PortableWeatherRoutingHost::ReportProgress(unsigned percent,
                                                const wxString& message) {
  m_impl->ReportProgress(percent, message);
}
bool PortableWeatherRoutingHost::Cancelled() const {
  return m_impl->Cancelled();
}
void PortableWeatherRoutingHost::Shutdown() { m_impl->Shutdown(); }
