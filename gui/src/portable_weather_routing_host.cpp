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
#include <wx/datetime.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/gauge.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/wfstream.h>
#include <wx/txtstrm.h>

#include "ocpn_portable_runtime.h"
#include "ocpndc.h"
#include "viewport.h"

namespace {
constexpr size_t kMaximumRoutePoints = 20000;
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
                                                "start-longitude",
                                                "destination-latitude",
                                                "destination-longitude",
                                                "departure-utc",
                                                "polar-reference-speed",
                                                "environment-provider",
                                                "avoid-unsafe",
                                                "maximum-wind",
                                                "maximum-wave",
                                                "time-step",
                                                "heading-step",
                                                "maximum-hours",
                                                "maximum-states",
                                                "compare-departures",
                                                "departure-window",
                                                "departure-spacing",
                                                "route-metrics",
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
  std::vector<ocpn_portable_route_point> points;
  wxString diagnostic;
  double distance_nautical_miles = 0.0;
  uint64_t duration_seconds = 0;
  uint32_t states_examined = 0;
  int64_t departure_unix_time = 0;
};

struct DepartureResult {
  RoutingOutcome outcome;
  wxString error;
  bool success = false;
};
}  // namespace

class PortableWeatherRoutingHost::Impl {
public:
  Impl(wxWindow* parent_value, ocpn_portable_runtime* runtime_value,
       std::shared_ptr<std::mutex> runtime_mutex_value,
       wxString package_root_value, std::function<wxString()> summary,
       double latitude, double longitude)
      : parent(parent_value),
        runtime(runtime_value),
        runtime_mutex(std::move(runtime_mutex_value)),
        package_root(std::move(package_root_value)),
        dataset_summary(std::move(summary)),
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
  void Start();
  void Finish(bool success, wxString message, RoutingOutcome selected,
              std::vector<std::vector<ocpn_portable_route_point>> alternatives,
              unsigned successful_departures);
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
  double initial_latitude = 0.0;
  double initial_longitude = 0.0;
  wxFrame* frame = nullptr;
  wxTextCtrl *start_lat = nullptr, *start_lon = nullptr, *dest_lat = nullptr,
             *dest_lon = nullptr, *departure = nullptr, *boat_speed = nullptr;
  wxSpinCtrl *time_step = nullptr, *heading_step = nullptr,
             *max_hours = nullptr, *max_states = nullptr,
             *departure_window = nullptr, *departure_spacing = nullptr;
  wxCheckBox *avoid_land = nullptr, *limit_wind = nullptr,
             *limit_waves = nullptr, *compare_departures = nullptr;
  wxTextCtrl *max_wind = nullptr, *max_wave = nullptr;
  wxStaticText *provider = nullptr, *status = nullptr, *metrics = nullptr;
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
  std::atomic<bool> stopped{false};
};

wxPanel* PortableWeatherRoutingHost::Impl::CreateRoutePanel(wxNotebook* book) {
  auto* panel = new wxPanel(book);
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
  boat_speed = new wxTextCtrl(panel, wxID_ANY, "7.0");
  AddRow(grid, panel, Label("start-latitude"), start_lat);
  AddRow(grid, panel, Label("start-longitude"), start_lon);
  AddRow(grid, panel, Label("destination-latitude"), dest_lat);
  AddRow(grid, panel, Label("destination-longitude"), dest_lon);
  AddRow(grid, panel, Label("departure-utc"), departure);
  AddRow(grid, panel, Label("polar-reference-speed"), boat_speed);
  provider = new wxStaticText(panel, wxID_ANY, dataset_summary());
  root->Add(grid, 0, wxEXPAND | wxALL, 12);
  root->Add(new wxStaticLine(panel), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
  root->Add(provider, 0, wxEXPAND | wxALL, 12);
  root->Add(
      new wxStaticText(panel, wxID_ANY,
                       "The Wasm route engine consumes environmental values "
                       "through iGRIB's typed host-brokered provider service."),
      0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  panel->SetSizer(root);
  return panel;
}

wxPanel* PortableWeatherRoutingHost::Impl::CreateSafetyPanel(wxNotebook* book) {
  auto* panel = new wxPanel(book);
  auto* root = new wxBoxSizer(wxVERTICAL);
  avoid_land = new wxCheckBox(panel, wxID_ANY, Label("avoid-unsafe"));
  avoid_land->SetValue(true);
  limit_wind = new wxCheckBox(panel, wxID_ANY, Label("maximum-wind"));
  limit_wind->SetValue(true);
  max_wind = new wxTextCtrl(panel, wxID_ANY, "35");
  limit_waves = new wxCheckBox(panel, wxID_ANY, Label("maximum-wave"));
  limit_waves->SetValue(true);
  max_wave = new wxTextCtrl(panel, wxID_ANY, "4.0");
  auto* grid = new wxFlexGridSizer(2, 8, 8);
  grid->AddGrowableCol(1);
  grid->Add(limit_wind, 0, wxALIGN_CENTER_VERTICAL);
  grid->Add(max_wind, 1, wxEXPAND);
  grid->Add(limit_waves, 0, wxALIGN_CENTER_VERTICAL);
  grid->Add(max_wave, 1, wxEXPAND);
  root->Add(avoid_land, 0, wxALL, 12);
  root->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
  root->Add(new wxStaticText(panel, wxID_ANY,
                             "Safety responses distinguish covered, unsafe, "
                             "missing coverage and unknown. They remain "
                             "advisory and must be checked by the navigator."),
            0, wxEXPAND | wxALL, 12);
  panel->SetSizer(root);
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
  AddRow(grid, panel, Label("time-step"), time_step);
  AddRow(grid, panel, Label("heading-step"), heading_step);
  AddRow(grid, panel, Label("maximum-hours"), max_hours);
  AddRow(grid, panel, Label("maximum-states"), max_states);
  AddRow(grid, panel, Label("departure-window"), departure_window);
  AddRow(grid, panel, Label("departure-spacing"), departure_spacing);
  auto* root = new wxBoxSizer(wxVERTICAL);
  root->Add(compare_departures, 0, wxLEFT | wxRIGHT | wxTOP, 12);
  root->Add(grid, 0, wxEXPAND | wxALL, 12);
  root->Add(new wxStaticText(
                panel, wxID_ANY,
                "Independent Wasm searches run in parallel (maximum four). "
                "The earliest safe arrival is selected; other successful "
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

void PortableWeatherRoutingHost::Impl::CreateFrame() {
  frame = new wxFrame(parent, wxID_ANY, surface_title, wxDefaultPosition,
                      wxSize(720, 570), wxDEFAULT_FRAME_STYLE);
  auto* root = new wxBoxSizer(wxVERTICAL);
  auto* book = new wxNotebook(frame, wxID_ANY);
  book->AddPage(CreateRoutePanel(book), surface_tabs[0]);
  book->AddPage(CreateSafetyPanel(book), surface_tabs[1]);
  book->AddPage(CreateAdvancedPanel(book), surface_tabs[2]);
  book->AddPage(CreateResultsPanel(book), surface_tabs[3]);
  root->Add(book, 1, wxEXPAND | wxALL, 8);
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
    } else
      frame->Hide();
  });
  frame->SetSizer(root);
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
  if (!frame) CreateFrame();
  provider->SetLabel(dataset_summary());
  frame->Show();
  frame->Raise();
  return true;
}

void PortableWeatherRoutingHost::Impl::Start() {
  if (worker.joinable()) return;
  ocpn_portable_route_request request{};
  if (!Number(start_lat, &request.start_latitude) ||
      !Number(start_lon, &request.start_longitude) ||
      !Number(dest_lat, &request.destination_latitude) ||
      !Number(dest_lon, &request.destination_longitude) ||
      !Number(boat_speed, &request.boat_speed_knots) ||
      !UtcTime(departure, &request.departure_unix_time)) {
    status->SetLabel(
        "Enter valid positions, speed and departure as YYYY-MM-DDTHH:MMZ");
    return;
  }
  request.time_step_seconds = time_step->GetValue();
  request.heading_step_degrees = heading_step->GetValue();
  request.max_hours = max_hours->GetValue();
  request.max_states = max_states->GetValue();
  request.avoid_unsafe_charts = avoid_land->GetValue();
  if (limit_wind->GetValue() && Number(max_wind, &request.max_wind_knots))
    request.limits_available |= 1;
  if (limit_waves->GetValue() && Number(max_wave, &request.max_wave_metres))
    request.limits_available |= 2;
  const unsigned run_count =
      compare_departures->GetValue()
          ? static_cast<unsigned>(departure_window->GetValue() /
                                      departure_spacing->GetValue() +
                                  1)
          : 1;
  const int64_t departure_step_seconds =
      static_cast<int64_t>(departure_spacing->GetValue()) * 3600;
  cancelled.store(false);
  calculate->Enable(false);
  cancel->Enable(true);
  export_gpx->Enable(false);
  route.clear();
  alternative_routes.clear();
  gauge->SetValue(0);
  status->SetLabel("Starting portable route engine…");
  departure_runs.store(run_count);
  departures_completed.store(0);
  worker = std::thread([this, request, run_count, departure_step_seconds] {
    std::vector<DepartureResult> results(run_count);
    std::atomic<unsigned> next_departure{0};
    const unsigned parallelism = std::min(4u, run_count);
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
          auto candidate_request = request;
          candidate_request.departure_unix_time +=
              static_cast<int64_t>(run) * departure_step_seconds;
          std::vector<ocpn_portable_route_point> points(kMaximumRoutePoints);
          std::vector<char> diagnostic(4096);
          char error[4096] = {};
          ocpn_portable_route_result result{
              points.data(),     points.size(),     0, 0.0, 0, 0,
              diagnostic.data(), diagnostic.size(), 0};

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
          departure_result.outcome = {
              std::move(points),
              wxString::FromUTF8(diagnostic.data(), result.diagnostic_len),
              result.distance_nautical_miles,
              result.duration_seconds,
              result.states_examined,
              candidate_request.departure_unix_time};
          departure_result.success = true;
          departures_completed.fetch_add(1);
        }
      });
    }
    for (auto& route_worker : workers) route_worker.join();

    RoutingOutcome best;
    std::vector<std::vector<ocpn_portable_route_point>> alternatives;
    wxString last_error = "No departure produced a route";
    unsigned successes = 0;
    int64_t best_arrival = std::numeric_limits<int64_t>::max();
    for (auto& departure_result : results) {
      if (!departure_result.success) {
        if (!departure_result.error.empty())
          last_error = departure_result.error;
        continue;
      }
      ++successes;
      const int64_t arrival =
          departure_result.outcome.departure_unix_time +
          static_cast<int64_t>(departure_result.outcome.duration_seconds);
      alternatives.push_back(departure_result.outcome.points);
      if (arrival < best_arrival) {
        best_arrival = arrival;
        best = std::move(departure_result.outcome);
      }
    }
    const bool success = !best.points.empty();
    const wxString message = success ? best.diagnostic : last_error;
    const auto live = alive;
    wxTheApp->CallAfter([this, live, success, message, best = std::move(best),
                         alternatives = std::move(alternatives),
                         successes]() mutable {
      if (!live->load()) return;
      Finish(success, message, std::move(best), std::move(alternatives),
             successes);
    });
  });
}

void PortableWeatherRoutingHost::Impl::Finish(
    bool success, wxString message, RoutingOutcome selected,
    std::vector<std::vector<ocpn_portable_route_point>> alternatives,
    unsigned successful_departures) {
  if (worker.joinable()) worker.join();
  calculate->Enable(true);
  cancel->Enable(false);
  if (!success) {
    status->SetLabel("Failed: " + message);
    return;
  }
  route = std::move(selected.points);
  alternative_routes = std::move(alternatives);
  gauge->SetValue(100);
  status->SetLabel(message);
  const wxString selected_departure =
      wxDateTime(static_cast<time_t>(selected.departure_unix_time))
          .ToUTC()
          .Format("%d %b %Y %H:%M UTC");
  metrics->SetLabel(wxString::Format(
      "%zu points · %.1f NM · %.1f hours · %u states · departure %s · "
      "%u successful departure(s)",
      route.size(), selected.distance_nautical_miles,
      selected.duration_seconds / 3600.0, selected.states_examined,
      selected_departure, successful_departures));
  export_gpx->Enable(!route.empty());
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
  dc.SetPen(wxPen(wxColour(235, 45, 175), 4));
  dc.DrawLines(static_cast<int>(points.size()), points.data());
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
    frame->Destroy();
    frame = nullptr;
  }
}

PortableWeatherRoutingHost::PortableWeatherRoutingHost(
    wxWindow* parent, ocpn_portable_runtime* runtime,
    std::shared_ptr<std::mutex> runtime_mutex, const wxString& package_root,
    std::function<wxString()> summary, double latitude, double longitude)
    : m_impl(std::make_unique<Impl>(parent, runtime, std::move(runtime_mutex),
                                    package_root, std::move(summary), latitude,
                                    longitude)) {}
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
