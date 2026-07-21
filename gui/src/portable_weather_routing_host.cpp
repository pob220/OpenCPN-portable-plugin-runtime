#include "portable_weather_routing_host.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include <wx/button.h>
#include <wx/app.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/datetime.h>
#include <wx/filedlg.h>
#include <wx/filepicker.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/gauge.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/wfstream.h>
#include <wx/txtstrm.h>

#include "ocpn_portable_runtime.h"
#include "ocpndc.h"
#include "navutil.h"
#include "portable_polar.h"
#include "viewport.h"

namespace {
constexpr size_t kMaximumRoutePoints = 20000;
constexpr size_t kMaximumInspectionPoints = 200000;
constexpr size_t kMaximumInspectionLines = 10000;
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
bool UtcTime(wxTextCtrl* control, int64_t* value) {
  if (!control || !value) return false;
  wxDateTime parsed;
  const wxChar* end =
      parsed.ParseFormat(control->GetValue(), "%Y-%m-%dT%H:%MZ");
  if (!end || *end != 0 || !parsed.IsValid()) return false;
  parsed.MakeFromTimezone(wxDateTime::UTC);
  *value = parsed.GetTicks();
  return true;
}
bool LoadSurface(const wxString& package_root, wxString* title,
                 wxArrayString* tabs, std::map<wxString, wxString>* labels,
                 wxString* error) {
  wxFileInputStream input(package_root + wxFILE_SEP_PATH + "ui" +
                          wxFILE_SEP_PATH + "iweather-routing.ui.json");
  wxJSONValue value;
  wxJSONReader reader;
  if (!input.IsOk() || reader.Parse(input, &value) != 0 || !value.IsObject() ||
      value["schema"].AsString() != "org.opencpn.portable-ui/0.1" ||
      value["surface"].AsString() != "weather-routing" ||
      !value["title"].IsString() || !value["tabs"].IsArray() ||
      value["tabs"].Size() != 4 || !value["controls"].IsArray()) {
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
                                                "use-currents",
                                                "require-current-data",
                                                "use-waves",
                                                "require-wave-data",
                                                "maximum-latitude",
                                                "upwind-efficiency",
                                                "downwind-efficiency",
                                                "tack-penalty",
                                                "gybe-penalty",
                                                "time-step",
                                                "heading-step",
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
                                                "show-isochrones",
                                                "route-to-cursor",
                                                "boat-at-grib-time",
                                                "export-gpx",
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
  *title = value["title"].AsString();
  return true;
}
struct RoutingOutcome {
  struct InspectionLine {
    int64_t unix_time = 0;
    std::vector<ocpn_portable_route_point> points;
  };
  std::vector<ocpn_portable_route_point> points;
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
  uint8_t comfort_level = 0;
  bool current_metrics_available = false;
  int64_t departure_unix_time = 0;
};

struct DepartureResult {
  RoutingOutcome outcome;
  wxString error;
  int64_t requested_departure_unix_time = 0;
  bool success = false;
};

wxString FormatUtc(int64_t unix_time) {
  return wxDateTime(static_cast<time_t>(unix_time))
      .ToUTC()
      .Format("%d %b %Y %H:%M UTC");
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
}  // namespace

class PortableWeatherRoutingHost::Impl {
public:
  Impl(wxWindow* parent_value, ocpn_portable_runtime* runtime_value,
       std::shared_ptr<std::mutex> runtime_mutex_value,
       wxString package_root_value, std::function<wxString()> summary,
       std::function<std::vector<PortableNavigationPosition>()> waypoints,
       std::function<bool(PortableNavigationPosition*)> vessel,
       std::function<bool(PortableNavigationPosition*)> cursor,
       std::function<bool(int64_t*)> environment_time,
       std::function<bool(double, double, const std::vector<int64_t>&,
                          std::vector<uint8_t>*, wxString*)>
           environment_preflight,
       double latitude, double longitude)
      : parent(parent_value),
        runtime(runtime_value),
        runtime_mutex(std::move(runtime_mutex_value)),
        package_root(std::move(package_root_value)),
        dataset_summary(std::move(summary)),
        list_waypoints(std::move(waypoints)),
        vessel_position(std::move(vessel)),
        cursor_position(std::move(cursor)),
        displayed_environment_time(std::move(environment_time)),
        preflight_environment(std::move(environment_preflight)),
        initial_latitude(latitude),
        initial_longitude(longitude) {}
  ~Impl() { Shutdown(); }
  bool Show(wxString* error);
  bool Render(ocpnDC& dc, const ViewPort& viewport);
  void ReportProgress(unsigned percent, const wxString& message);
  bool Cancelled() const { return cancelled.load(); }
  void Shutdown();

private:
  void CreateFrame();
  wxPanel* CreateRoutePanel(wxNotebook* book);
  wxPanel* CreateSafetyPanel(wxNotebook* book);
  wxPanel* CreateAdvancedPanel(wxNotebook* book);
  wxPanel* CreateResultsPanel(wxNotebook* book);
  void RefreshNavigationPositions(bool initial = false);
  bool ApplyPositionSource(bool start, bool report_error = true);
  void UpdatePositionControls(bool start);
  void RelayoutRoutePanel();
  bool LoadVesselPerformance(const wxString& path, bool report_error = true);
  void LoadSettings();
  void SaveSettings();
  void Start();
  void Finish(std::vector<DepartureResult> results, int64_t nominal_departure);
  void PopulateDepartureResults();
  void SelectDepartureResult(size_t index);
  void ExportGpx();
  const wxString& Label(const wxString& id) const {
    return surface_labels.at(id);
  }

  wxWindow* parent = nullptr;
  ocpn_portable_runtime* runtime = nullptr;
  std::shared_ptr<std::mutex> runtime_mutex;
  wxString package_root;
  wxString surface_title;
  wxArrayString surface_tabs;
  std::map<wxString, wxString> surface_labels;
  bool surface_loaded = false;
  std::function<wxString()> dataset_summary;
  std::function<std::vector<PortableNavigationPosition>()> list_waypoints;
  std::function<bool(PortableNavigationPosition*)> vessel_position;
  std::function<bool(PortableNavigationPosition*)> cursor_position;
  std::function<bool(int64_t*)> displayed_environment_time;
  std::function<bool(double, double, const std::vector<int64_t>&,
                     std::vector<uint8_t>*, wxString*)>
      preflight_environment;
  std::vector<PortableNavigationPosition> waypoints;
  double initial_latitude = 0.0;
  double initial_longitude = 0.0;
  wxFrame* frame = nullptr;
  wxNotebook* notebook = nullptr;
  wxScrolledWindow* route_panel = nullptr;
  wxTextCtrl *start_lat = nullptr, *start_lon = nullptr, *dest_lat = nullptr,
             *dest_lon = nullptr, *departure = nullptr;
  wxChoice *start_source = nullptr, *start_waypoint = nullptr,
           *dest_source = nullptr, *dest_waypoint = nullptr;
  wxButton* refresh_positions = nullptr;
  wxFilePickerCtrl* vessel_performance_file = nullptr;
  wxSpinCtrl *time_step = nullptr, *heading_step = nullptr,
             *max_hours = nullptr, *max_states = nullptr,
             *departure_window = nullptr, *departure_spacing = nullptr,
             *departure_workers = nullptr, *min_wind_angle = nullptr,
             *max_wind_angle = nullptr, *maximum_latitude = nullptr,
             *upwind_efficiency = nullptr, *downwind_efficiency = nullptr,
             *tack_penalty = nullptr, *gybe_penalty = nullptr,
             *maximum_search_angle = nullptr;
  wxCheckBox *avoid_land = nullptr, *limit_true_wind = nullptr,
             *limit_apparent_wind = nullptr, *limit_waves = nullptr,
             *use_currents = nullptr, *require_current_data = nullptr,
             *use_waves = nullptr, *require_wave_data = nullptr,
             *compare_departures = nullptr;
  wxCheckBox *show_isochrones = nullptr, *route_to_cursor = nullptr,
             *boat_at_grib_time = nullptr;
  wxTextCtrl *max_true_wind = nullptr, *max_apparent_wind = nullptr,
             *max_wave = nullptr, *destination_tolerance = nullptr;
  wxStaticText *provider = nullptr, *vessel_performance_status = nullptr,
               *status = nullptr, *metrics = nullptr;
  wxListCtrl* departure_results = nullptr;
  wxGauge* gauge = nullptr;
  wxButton *calculate = nullptr, *cancel = nullptr, *export_gpx = nullptr;
  std::atomic<bool> cancelled{false};
  std::shared_ptr<std::atomic<bool>> alive =
      std::make_shared<std::atomic<bool>>(true);
  std::atomic<unsigned> departure_runs{1};
  std::atomic<unsigned> departures_completed{0};
  std::thread worker;
  std::vector<ocpn_portable_route_point> route;
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
  departure = new wxTextCtrl(
      panel, wxID_ANY, wxDateTime::Now().ToUTC().Format("%Y-%m-%dT%H:%MZ"));
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
  refresh_positions = new wxButton(panel, wxID_ANY, Label("refresh-positions"));
  AddRow(grid, panel, Label("start-source"), start_source);
  AddRow(grid, panel, Label("start-waypoint"), start_waypoint);
  AddRow(grid, panel, Label("start-latitude"), start_lat);
  AddRow(grid, panel, Label("start-longitude"), start_lon);
  AddRow(grid, panel, Label("destination-source"), dest_source);
  AddRow(grid, panel, Label("destination-waypoint"), dest_waypoint);
  AddRow(grid, panel, Label("destination-latitude"), dest_lat);
  AddRow(grid, panel, Label("destination-longitude"), dest_lon);
  AddRow(grid, panel, Label("departure-utc"), departure);
  AddRow(grid, panel, Label("vessel-performance-file"),
         vessel_performance_file);
  vessel_performance_status =
      new wxStaticText(panel, wxID_ANY, Label("vessel-performance-status"));
  provider = new wxStaticText(panel, wxID_ANY, dataset_summary());
  root->Add(grid, 0, wxEXPAND | wxALL, 12);
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
  refresh_positions->Bind(
      wxEVT_BUTTON, [this](wxCommandEvent&) { RefreshNavigationPositions(); });
  vessel_performance_file->Bind(
      wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent&) {
        LoadVesselPerformance(vessel_performance_file->GetPath());
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
  waypoints = list_waypoints ? list_waypoints()
                             : std::vector<PortableNavigationPosition>();
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
        wxString::Format("Loaded %zu OpenCPN waypoint(s)", waypoints.size()));
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
  max_wind_angle->SetValue(160);
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
  tack_penalty->SetValue(0);
  gybe_penalty = new wxSpinCtrl(panel, wxID_ANY);
  gybe_penalty->SetRange(0, 3600);
  gybe_penalty->SetValue(0);
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
  root->Add(new wxStaticText(panel, wxID_ANY,
                             "Safety responses distinguish covered, unsafe, "
                             "missing coverage and unknown. They remain "
                             "advisory and must be checked by the navigator."),
            0, wxEXPAND | wxALL, 12);
  panel->SetSizer(root);
  use_currents->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    require_current_data->Enable(use_currents->GetValue());
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
  limit_apparent_wind->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    max_apparent_wind->Enable(limit_apparent_wind->GetValue());
  });
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
  AddRow(grid, panel, Label("maximum-search-angle"), maximum_search_angle);
  AddRow(grid, panel, Label("destination-tolerance"), destination_tolerance);
  AddRow(grid, panel, Label("maximum-hours"), max_hours);
  AddRow(grid, panel, Label("maximum-states"), max_states);
  AddRow(grid, panel, Label("departure-window"), departure_window);
  AddRow(grid, panel, Label("departure-spacing"), departure_spacing);
  AddRow(grid, panel, Label("departure-workers"), departure_workers);
  auto* root = new wxBoxSizer(wxVERTICAL);
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
  const wxString result_columns[] = {
      "Best",     "Offset",      "Departure",   "ETA",     "Elapsed",
      "Distance", "Avg Speed",   "Avg SOG",     "Max SOG", "Avg Wind",
      "Max Wind", "Avg Current", "Max Current", "Tacks",   "Comfort",
      "States",   "Isochrones",  "State"};
  for (size_t column = 0; column < WXSIZEOF(result_columns); ++column)
    departure_results->InsertColumn(static_cast<int>(column),
                                    result_columns[column]);
  departure_results->SetMinSize(wxSize(-1, panel->FromDIP(210)));
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
  show_isochrones = new wxCheckBox(panel, wxID_ANY, Label("show-isochrones"));
  route_to_cursor = new wxCheckBox(panel, wxID_ANY, Label("route-to-cursor"));
  boat_at_grib_time =
      new wxCheckBox(panel, wxID_ANY, Label("boat-at-grib-time"));
  root->Add(show_isochrones, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(route_to_cursor, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(boat_at_grib_time, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
  for (auto* toggle : {show_isochrones, route_to_cursor, boat_at_grib_time}) {
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
  root->Add(export_gpx, 0, wxALL, 12);
  panel->SetSizer(root);
  return panel;
}

void PortableWeatherRoutingHost::Impl::LoadSettings() {
  const wxString bundled_polar = package_root + wxFILE_SEP_PATH + "resources" +
                                 wxFILE_SEP_PATH +
                                 "Nicholson35_Mk1_cruising_realistic.pol";
  show_isochrones->SetValue(true);
  route_to_cursor->SetValue(false);
  boat_at_grib_time->SetValue(true);
  if (!pConfig) {
    vessel_performance_file->SetPath(bundled_polar);
    LoadVesselPerformance(bundled_polar, false);
    return;
  }
  const wxString old_path = pConfig->GetPath();
  pConfig->SetPath("/PortablePlugins/org.opencpn.iweather-routing/Routing");
  const wxString performance_path =
      pConfig->Read("vesselPerformancePath", bundled_polar);
  vessel_performance_file->SetPath(performance_path);
  LoadVesselPerformance(performance_path, false);
  avoid_land->SetValue(pConfig->ReadBool("avoidUnsafeCharts", true));
  min_wind_angle->SetValue(pConfig->ReadLong("minimumTrueWindAngle", 40));
  max_wind_angle->SetValue(pConfig->ReadLong("maximumTrueWindAngle", 160));
  limit_true_wind->SetValue(pConfig->ReadBool("limitTrueWind", true));
  max_true_wind->SetValue(pConfig->Read("maximumTrueWind", "50"));
  limit_apparent_wind->SetValue(pConfig->ReadBool("limitApparentWind", true));
  max_apparent_wind->SetValue(pConfig->Read("maximumApparentWind", "50"));
  limit_waves->SetValue(pConfig->ReadBool("limitWaves", true));
  max_wave->SetValue(pConfig->Read("maximumWave", "8.0"));
  use_currents->SetValue(pConfig->ReadBool("useCurrents", true));
  require_current_data->SetValue(
      pConfig->ReadBool("requireCurrentData", false));
  use_waves->SetValue(pConfig->ReadBool("useWaves", true));
  require_wave_data->SetValue(pConfig->ReadBool("requireWaveData", true));
  maximum_latitude->SetValue(pConfig->ReadLong("maximumLatitude", 89));
  upwind_efficiency->SetValue(pConfig->ReadLong("upwindEfficiency", 100));
  downwind_efficiency->SetValue(pConfig->ReadLong("downwindEfficiency", 100));
  tack_penalty->SetValue(pConfig->ReadLong("tackPenaltySeconds", 0));
  gybe_penalty->SetValue(pConfig->ReadLong("gybePenaltySeconds", 0));
  time_step->SetValue(pConfig->ReadLong("timeStepSeconds", 3600));
  heading_step->SetValue(pConfig->ReadLong("headingStepDegrees", 15));
  maximum_search_angle->SetValue(
      pConfig->ReadLong("maximumSearchAngleDegrees", 120));
  destination_tolerance->SetValue(
      pConfig->Read("destinationToleranceNm", "1.0"));
  max_hours->SetValue(pConfig->ReadLong("maximumHours", 120));
  max_states->SetValue(pConfig->ReadLong("maximumStates", 80000));
  compare_departures->SetValue(pConfig->ReadBool("compareDepartures", false));
  departure_window->SetValue(pConfig->ReadLong("departureWindowHours", 6));
  departure_spacing->SetValue(pConfig->ReadLong("departureSpacingHours", 1));
  departure_workers->SetValue(pConfig->ReadLong("departureWorkers", 4));
  show_isochrones->SetValue(pConfig->ReadBool("showIsochrones", true));
  route_to_cursor->SetValue(pConfig->ReadBool("routeToCursor", false));
  boat_at_grib_time->SetValue(pConfig->ReadBool("boatAtGribTime", true));
  pConfig->SetPath(old_path);

  require_current_data->Enable(use_currents->GetValue());
  require_wave_data->Enable(use_waves->GetValue());
  limit_waves->Enable(use_waves->GetValue());
  max_wave->Enable(use_waves->GetValue() && limit_waves->GetValue());
  max_true_wind->Enable(limit_true_wind->GetValue());
  max_apparent_wind->Enable(limit_apparent_wind->GetValue());
}

void PortableWeatherRoutingHost::Impl::SaveSettings() {
  if (!pConfig) return;
  const wxString old_path = pConfig->GetPath();
  pConfig->SetPath("/PortablePlugins/org.opencpn.iweather-routing/Routing");
  pConfig->Write("settingsSchema", 3L);
  if (vessel_performance_file)
    pConfig->Write("vesselPerformancePath", vessel_performance_file->GetPath());
  pConfig->Write("avoidUnsafeCharts", avoid_land->GetValue());
  pConfig->Write("minimumTrueWindAngle",
                 static_cast<long>(min_wind_angle->GetValue()));
  pConfig->Write("maximumTrueWindAngle",
                 static_cast<long>(max_wind_angle->GetValue()));
  pConfig->Write("limitTrueWind", limit_true_wind->GetValue());
  pConfig->Write("maximumTrueWind", max_true_wind->GetValue());
  pConfig->Write("limitApparentWind", limit_apparent_wind->GetValue());
  pConfig->Write("maximumApparentWind", max_apparent_wind->GetValue());
  pConfig->Write("limitWaves", limit_waves->GetValue());
  pConfig->Write("maximumWave", max_wave->GetValue());
  pConfig->Write("useCurrents", use_currents->GetValue());
  pConfig->Write("requireCurrentData", require_current_data->GetValue());
  pConfig->Write("useWaves", use_waves->GetValue());
  pConfig->Write("requireWaveData", require_wave_data->GetValue());
  pConfig->Write("maximumLatitude",
                 static_cast<long>(maximum_latitude->GetValue()));
  pConfig->Write("upwindEfficiency",
                 static_cast<long>(upwind_efficiency->GetValue()));
  pConfig->Write("downwindEfficiency",
                 static_cast<long>(downwind_efficiency->GetValue()));
  pConfig->Write("tackPenaltySeconds",
                 static_cast<long>(tack_penalty->GetValue()));
  pConfig->Write("gybePenaltySeconds",
                 static_cast<long>(gybe_penalty->GetValue()));
  pConfig->Write("timeStepSeconds", static_cast<long>(time_step->GetValue()));
  pConfig->Write("headingStepDegrees",
                 static_cast<long>(heading_step->GetValue()));
  pConfig->Write("maximumSearchAngleDegrees",
                 static_cast<long>(maximum_search_angle->GetValue()));
  pConfig->Write("destinationToleranceNm", destination_tolerance->GetValue());
  pConfig->Write("maximumHours", static_cast<long>(max_hours->GetValue()));
  pConfig->Write("maximumStates", static_cast<long>(max_states->GetValue()));
  pConfig->Write("compareDepartures", compare_departures->GetValue());
  pConfig->Write("departureWindowHours",
                 static_cast<long>(departure_window->GetValue()));
  pConfig->Write("departureSpacingHours",
                 static_cast<long>(departure_spacing->GetValue()));
  pConfig->Write("departureWorkers",
                 static_cast<long>(departure_workers->GetValue()));
  pConfig->Write("showIsochrones", show_isochrones->GetValue());
  pConfig->Write("routeToCursor", route_to_cursor->GetValue());
  pConfig->Write("boatAtGribTime", boat_at_grib_time->GetValue());
  pConfig->SetPath(old_path);
  pConfig->Flush();
}

void PortableWeatherRoutingHost::Impl::CreateFrame() {
  frame = new wxFrame(parent, wxID_ANY, surface_title, wxDefaultPosition,
                      wxSize(820, 760),
                      wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT);
  frame->SetMinSize(frame->FromDIP(wxSize(680, 560)));
  auto* root = new wxBoxSizer(wxVERTICAL);
  notebook = new wxNotebook(frame, wxID_ANY);
  notebook->AddPage(CreateRoutePanel(notebook), surface_tabs[0]);
  notebook->AddPage(CreateSafetyPanel(notebook), surface_tabs[1]);
  notebook->AddPage(CreateAdvancedPanel(notebook), surface_tabs[2]);
  notebook->AddPage(CreateResultsPanel(notebook), surface_tabs[3]);
  root->Add(notebook, 1, wxEXPAND | wxALL, 8);
  status = new wxStaticText(frame, wxID_ANY, "Ready");
  gauge = new wxGauge(frame, wxID_ANY, 100);
  root->Add(status, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
  root->Add(gauge, 0, wxEXPAND | wxALL, 12);
  auto* buttons = new wxBoxSizer(wxHORIZONTAL);
  calculate = new wxButton(frame, wxID_ANY, Label("calculate"));
  cancel = new wxButton(frame, wxID_ANY, Label("cancel"));
  cancel->Enable(false);
  buttons->AddStretchSpacer();
  buttons->Add(calculate, 0, wxRIGHT, 8);
  buttons->Add(cancel);
  root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  calculate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Start(); });
  cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    cancelled.store(true);
    status->SetLabel("Cancelling…");
  });
  frame->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
    if (worker.joinable()) {
      cancelled.store(true);
      event.Veto();
    } else {
      SaveSettings();
      frame->Hide();
    }
  });
  frame->SetSizer(root);
  LoadSettings();
  RefreshNavigationPositions(true);
  frame->Layout();
  RelayoutRoutePanel();
}

bool PortableWeatherRoutingHost::Impl::Show(wxString* error) {
  if (stopped.load() || !runtime) {
    if (error) *error = "routing host is unavailable";
    return false;
  }
  if (!surface_loaded) {
    if (!LoadSurface(package_root, &surface_title, &surface_tabs,
                     &surface_labels, error))
      return false;
    surface_loaded = true;
  }
  if (!frame)
    CreateFrame();
  else
    RefreshNavigationPositions(false);
  provider->SetLabel("Environment: " + dataset_summary());
  RelayoutRoutePanel();
  frame->Layout();
  frame->Show();
  frame->Raise();
  return true;
}

void PortableWeatherRoutingHost::Impl::Start() {
  if (worker.joinable()) return;
  if (!ApplyPositionSource(true) || !ApplyPositionSource(false)) return;
  ocpn_portable_route_request request{};
  if (!Number(start_lat, &request.start_latitude) ||
      !Number(start_lon, &request.start_longitude) ||
      !Number(dest_lat, &request.destination_latitude) ||
      !Number(dest_lon, &request.destination_longitude) ||
      !UtcTime(departure, &request.departure_unix_time)) {
    status->SetLabel(
        "Enter valid positions and departure as YYYY-MM-DDTHH:MMZ");
    return;
  }
  const auto selected_performance = vessel_performance;
  if (!selected_performance || selected_performance->grids.empty()) {
    status->SetLabel("Load a valid OpenCPN boat .xml or polar .pol file first");
    return;
  }
  request.time_step_seconds = time_step->GetValue();
  request.heading_step_degrees = heading_step->GetValue();
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
  vessel_performance_file->Enable(false);
  export_gpx->Enable(false);
  route.clear();
  alternative_routes.clear();
  isochrones.clear();
  traces.clear();
  departure_result_rows.clear();
  selected_departure_result = std::numeric_limits<size_t>::max();
  best_departure_result = std::numeric_limits<size_t>::max();
  if (departure_results) departure_results->DeleteAllItems();
  gauge->SetValue(0);
  status->SetLabel("Checking iGRIB coverage for requested departures…");
  departure_runs.store(run_count);
  departures_completed.store(0);
  worker = std::thread([this, request, run_count, departure_step_seconds,
                        departure_times = std::move(departure_times),
                        parallel_worker_limit, selected_performance] {
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
            FormatUtc(departure_times[run]);
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
          std::vector<ocpn_portable_polar_grid> polar_views;
          polar_views.reserve(selected_performance->grids.size());
          for (const auto& grid : selected_performance->grids) {
            polar_views.push_back({grid.identity.data(), grid.identity.size(),
                                   grid.true_wind_speeds_knots.data(),
                                   grid.true_wind_speeds_knots.size(),
                                   grid.true_wind_angles_degrees.data(),
                                   grid.true_wind_angles_degrees.size(),
                                   grid.boat_speeds_knots.data(),
                                   grid.boat_speeds_knots.size()});
          }
          candidate_request.polars = polar_views.data();
          candidate_request.polar_count = polar_views.size();
          std::vector<ocpn_portable_route_point> points(kMaximumRoutePoints);
          std::vector<ocpn_portable_route_point> isochrone_points(
              kMaximumInspectionPoints);
          std::vector<ocpn_portable_route_line> isochrone_lines(
              kMaximumInspectionLines);
          std::vector<ocpn_portable_route_point> trace_points(
              kMaximumInspectionPoints);
          std::vector<ocpn_portable_route_line> trace_lines(
              kMaximumInspectionLines);
          std::vector<char> diagnostic(4096);
          char error[4096] = {};
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
          result.diagnostic = diagnostic.data();
          result.diagnostic_capacity = diagnostic.size();

          ocpn_portable_runtime* replica = nullptr;
          {
            // The compiled Component is shared, while each calculation gets
            // an independent Store. The primary Store is never re-entered.
            std::lock_guard<std::mutex> lock(*runtime_mutex);
            replica = ocpn_portable_runtime_clone_compute(runtime, error,
                                                          sizeof(error));
          }
          if (!replica) {
            departure_result.error = "Could not create Wasm compute replica: " +
                                     wxString::FromUTF8(error);
            departures_completed.fetch_add(1);
            continue;
          }
          const bool ok = ocpn_portable_runtime_calculate_route(
                              replica, &candidate_request, &result, error,
                              sizeof(error)) == 0;
          ocpn_portable_runtime_destroy(replica);
          if (!ok) {
            departure_result.error = wxString::FromUTF8(error);
            departures_completed.fetch_add(1);
            continue;
          }
          if (result.point_count > points.size()) {
            departure_result.error =
                "Portable route exceeded the host point limit";
            departures_completed.fetch_add(1);
            continue;
          }
          points.resize(result.point_count);
          auto copy_inspection =
              [](const std::vector<ocpn_portable_route_point>& source_points,
                 const std::vector<ocpn_portable_route_line>& source_lines,
                 size_t line_count) {
                std::vector<RoutingOutcome::InspectionLine> copied;
                copied.reserve(line_count);
                for (size_t index = 0; index < line_count; ++index) {
                  const auto& span = source_lines[index];
                  if (span.point_offset > source_points.size() ||
                      span.point_count >
                          source_points.size() - span.point_offset)
                    continue;
                  copied.push_back(
                      {span.unix_time,
                       std::vector<ocpn_portable_route_point>(
                           source_points.begin() + span.point_offset,
                           source_points.begin() + span.point_offset +
                               span.point_count)});
                }
                return copied;
              };
          RoutingOutcome outcome;
          outcome.points = std::move(points);
          outcome.isochrones = copy_inspection(
              isochrone_points, isochrone_lines, result.isochrone_count);
          outcome.traces =
              copy_inspection(trace_points, trace_lines, result.trace_count);
          outcome.diagnostic =
              wxString::FromUTF8(diagnostic.data(), result.diagnostic_len);
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
          outcome.comfort_level = result.comfort_level;
          outcome.current_metrics_available = result.metrics_available & 1;
          outcome.departure_unix_time = candidate_request.departure_unix_time;
          departure_result.outcome = std::move(outcome);
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
    departure_results->SetItem(row, 2, FormatUtc(departure));
    if (result.success) {
      const auto& outcome = result.outcome;
      departure_results->SetItem(
          row, 3,
          FormatUtc(outcome.departure_unix_time +
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
      const wxString comfort = outcome.comfort_level == 1   ? "Good"
                               : outcome.comfort_level == 2 ? "Bumpy"
                               : outcome.comfort_level == 3 ? "Difficult"
                                                            : "N/A";
      departure_results->SetItem(row, 14, comfort);
      departure_results->SetItem(
          row, 15, wxString::Format("%u", outcome.states_examined));
      departure_results->SetItem(
          row, 16, wxString::Format("%zu", outcome.isochrones.size()));
      departure_results->SetItem(row, 17, "Complete");
    } else {
      for (int column = 3; column <= 16; ++column)
        departure_results->SetItem(row, column, "N/A");
      departure_results->SetItem(
          row, 17, result.error.empty() ? "Failed" : result.error);
    }
  }
  for (int column = 0; column < 18; ++column)
    departure_results->SetColumnWidth(column, wxLIST_AUTOSIZE_USEHEADER);
}

void PortableWeatherRoutingHost::Impl::SelectDepartureResult(size_t index) {
  if (index >= departure_result_rows.size() ||
      !departure_result_rows[index].success)
    return;
  selected_departure_result = index;
  const auto& selected = departure_result_rows[index].outcome;
  route = selected.points;
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
  metrics->SetLabel(wxString::Format(
      "%s%s · %zu points · %zu isochrones · %.1f NM · %.1f hours · %u "
      "states · departure %s",
      index == best_departure_result ? "Best passage · "
                                     : "Selected passage · ",
      FormatElapsed(selected.duration_seconds), route.size(), isochrones.size(),
      selected.distance_nautical_miles, selected.duration_seconds / 3600.0,
      selected.states_examined, FormatUtc(selected.departure_unix_time)));
  export_gpx->Enable(!route.empty());
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
  if (parent) parent->Refresh();
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

bool PortableWeatherRoutingHost::Impl::Render(ocpnDC& dc,
                                              const ViewPort& viewport) {
  if (route.size() < 2) return false;
  ViewPort projection = viewport;
  if (show_isochrones && show_isochrones->GetValue()) {
    dc.SetPen(wxPen(wxColour(225, 105, 195, 175), 2));
    for (const auto& contour : isochrones) {
      if (contour.points.size() < 2) continue;
      std::vector<wxPoint> line;
      line.reserve(contour.points.size());
      for (const auto& point : contour.points)
        line.push_back(
            projection.GetPixFromLL(point.latitude, point.longitude));
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
          projection.GetPixFromLL(point.latitude, point.longitude));
    dc.DrawLines(static_cast<int>(comparison.size()), comparison.data());
  }
  std::vector<wxPoint> points;
  points.reserve(route.size());
  for (const auto& point : route)
    points.push_back(projection.GetPixFromLL(point.latitude, point.longitude));
  dc.SetPen(wxPen(wxColour(255, 255, 255, 220), 7));
  dc.DrawLines(static_cast<int>(points.size()), points.data());
  dc.SetPen(wxPen(wxColour(235, 45, 175), 4));
  dc.DrawLines(static_cast<int>(points.size()), points.data());

  if (route_to_cursor && route_to_cursor->GetValue() && cursor_position) {
    PortableNavigationPosition cursor;
    if (cursor_position(&cursor)) {
      const wxPoint cursor_pixel =
          projection.GetPixFromLL(cursor.latitude, cursor.longitude);
      const RoutingOutcome::InspectionLine* closest = nullptr;
      long closest_squared = 32L * 32L;
      for (const auto& trace : traces) {
        if (trace.points.size() < 2) continue;
        const auto& endpoint = trace.points.back();
        const wxPoint pixel =
            projection.GetPixFromLL(endpoint.latitude, endpoint.longitude);
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
              projection.GetPixFromLL(point.latitude, point.longitude));
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
          projection.GetPixFromLL(boat_latitude, boat_longitude);
      dc.SetPen(wxPen(wxColour(255, 255, 255), 3));
      dc.SetBrush(wxBrush(wxColour(235, 45, 175)));
      dc.DrawCircle(boat, 8);
      dc.SetPen(wxPen(wxColour(80, 20, 80), 1));
      dc.DrawCircle(boat, 4);
    }
  }
  return true;
}

void PortableWeatherRoutingHost::Impl::ExportGpx() {
  if (route.empty()) return;
  wxFileDialog dialog(frame, "Export iWeatherRouting route", wxEmptyString,
                      "iweather-routing.gpx", "GPX files (*.gpx)|*.gpx",
                      wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
  if (dialog.ShowModal() != wxID_OK) return;
  wxFileOutputStream file(dialog.GetPath());
  if (!file.IsOk()) return;
  wxTextOutputStream out(file);
  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" "
         "creator=\"iWeatherRouting\" "
         "xmlns=\"http://www.topografix.com/GPX/1/"
         "1\"><rte><name>iWeatherRouting advisory route</name>\n";
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

void PortableWeatherRoutingHost::Impl::Shutdown() {
  if (stopped.exchange(true)) return;
  alive->store(false);
  cancelled.store(true);
  if (worker.joinable()) worker.join();
  if (frame) {
    SaveSettings();
    frame->Destroy();
    frame = nullptr;
  }
}

PortableWeatherRoutingHost::PortableWeatherRoutingHost(
    wxWindow* parent, ocpn_portable_runtime* runtime,
    std::shared_ptr<std::mutex> runtime_mutex, const wxString& package_root,
    std::function<wxString()> summary,
    std::function<std::vector<PortableNavigationPosition>()> list_waypoints,
    std::function<bool(PortableNavigationPosition*)> vessel_position,
    std::function<bool(PortableNavigationPosition*)> cursor_position,
    std::function<bool(int64_t*)> displayed_environment_time,
    std::function<bool(double, double, const std::vector<int64_t>&,
                       std::vector<uint8_t>*, wxString*)>
        preflight_environment,
    double latitude, double longitude)
    : m_impl(std::make_unique<Impl>(
          parent, runtime, std::move(runtime_mutex), package_root,
          std::move(summary), std::move(list_waypoints),
          std::move(vessel_position), std::move(cursor_position),
          std::move(displayed_environment_time),
          std::move(preflight_environment), latitude, longitude)) {}
PortableWeatherRoutingHost::~PortableWeatherRoutingHost() = default;
bool PortableWeatherRoutingHost::Show(wxString* error) {
  return m_impl->Show(error);
}
bool PortableWeatherRoutingHost::Render(ocpnDC& dc, const ViewPort& viewport) {
  return m_impl->Render(dc, viewport);
}
void PortableWeatherRoutingHost::ReportProgress(unsigned percent,
                                                const wxString& message) {
  m_impl->ReportProgress(percent, message);
}
bool PortableWeatherRoutingHost::Cancelled() const {
  return m_impl->Cancelled();
}
void PortableWeatherRoutingHost::Shutdown() { m_impl->Shutdown(); }
