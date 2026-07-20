/***************************************************************************
 * Host-owned iGRIB environmental service implementation.
 ***************************************************************************/

#include "portable_grib_host.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clrpicker.h>
#include <wx/datetime.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/gauge.h>
#include <wx/hyperlink.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/jsonwriter.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/process.h>
#include <wx/scrolwin.h>
#include <wx/secretstore.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/wfstream.h>

#include "model/base_platform.h"
#include "navutil.h"
#include "ocpndc.h"
#include "top_frame.h"
#include "viewport.h"

namespace {

constexpr int kHelperProcessId = wxID_HIGHEST + 711;
constexpr int kProgressTimerId = wxID_HIGHEST + 712;
constexpr int kPlaybackTimerId = wxID_HIGHEST + 713;
constexpr size_t kMaximumResultBytes = 32U * 1024U * 1024U;
constexpr double kPi = 3.14159265358979323846;
constexpr const char* kCopernicusCredentialService =
    "OpenCPN iGRIB Copernicus Marine";
constexpr const char* kCopernicusPasswordEnvironment =
    "OCPN_IGRIB_COPERNICUS_PASSWORD";

struct Sample {
  double latitude = 0.0;
  double longitude = 0.0;
  double value = 0.0;
};

struct LayerDisplaySettings {
  int units = 0;
  bool vectors = false;
  bool overlay = false;
  bool numbers = false;
  bool contours = false;
  int vector_spacing = 44;
  int number_spacing = 70;
  int contour_spacing = 4;
  int vector_style = 0;
  wxColour colour = *wxBLACK;
  int proportional_base_size = 12;
  double proportional_growth_per_knot = 6.0;
};

bool ParseGribTime(const wxString& value, wxDateTime* result) {
  if (value.length() != 14 || value[8] != 'T' || value[13] != 'Z') return false;
  long year = 0, month = 0, day = 0, hour = 0, minute = 0;
  if (!value.Mid(0, 4).ToLong(&year) || !value.Mid(4, 2).ToLong(&month) ||
      !value.Mid(6, 2).ToLong(&day) || !value.Mid(9, 2).ToLong(&hour) ||
      !value.Mid(11, 2).ToLong(&minute) || month < 1 || month > 12 || day < 1 ||
      day > 31 || hour > 23 || minute > 59)
    return false;
  *result = wxDateTime(static_cast<wxDateTime::wxDateTime_t>(day),
                       static_cast<wxDateTime::Month>(month - 1),
                       static_cast<int>(year),
                       static_cast<wxDateTime::wxDateTime_t>(hour),
                       static_cast<wxDateTime::wxDateTime_t>(minute));
  return result->IsValid();
}

wxString FormatGribTime(const wxString& value) {
  wxDateTime time;
  return ParseGribTime(value, &time) ? time.Format("%a %d %b %Y  %H:%M UTC")
                                     : value;
}

double PressureHpa(double value) {
  return std::abs(value) > 2000.0 ? value / 100.0 : value;
}

double TemperatureCelsius(double value) {
  return value > 150.0 ? value - 273.15 : value;
}

wxString HostHelperDirectory() {
#if defined(__WXMSW__)
  return "windows-x86_64";
#elif defined(__WXOSX__)
#if defined(__aarch64__) || defined(__arm64__)
  return "macos-aarch64";
#else
  return "macos-x86_64";
#endif
#elif defined(__aarch64__)
  return "linux-gnu-aarch64";
#else
  return "linux-gnu-x86_64";
#endif
}

bool HelperSupervisionAvailable(wxString* error = nullptr) {
#if defined(__linux__)
  if (wxFileExists("/usr/bin/bwrap") && wxFileExists("/usr/bin/prlimit"))
    return true;
  if (error)
    *error = "native helper execution requires bubblewrap and prlimit on Linux";
#else
  if (error)
    *error = "native helper supervision is not implemented for this host";
#endif
  return false;
}

bool ReadJson(const wxString& path, wxJSONValue* value, wxString* error) {
  wxFileName file(path);
  if (!file.FileExists()) {
    *error = "helper did not publish a result";
    return false;
  }
  if (file.GetSize().GetValue() > kMaximumResultBytes) {
    *error = "helper result exceeded the 32 MiB service limit";
    return false;
  }
  wxFileInputStream input(path);
  wxJSONReader reader;
  if (!input.IsOk() || reader.Parse(input, value) != 0 || !value->IsObject()) {
    *error = "helper returned invalid JSON";
    return false;
  }
  if ((*value)["error"].IsObject()) {
    *error = (*value)["error"]["message"].AsString();
    if (error->empty()) *error = "environmental helper failed";
    return false;
  }
  return true;
}

bool LoadDeclarativeSurface(const wxString& path, wxString* title,
                            wxString* error) {
  wxJSONValue definition;
  if (!ReadJson(path, &definition, error)) return false;
  if (!definition["schema_version"].IsInt() ||
      definition["schema_version"].AsInt() != 1 ||
      definition["surface_id"].AsString() != "igrib.viewer" ||
      definition["kind"].AsString() != "floating-panel" ||
      !definition["title"].IsString() || !definition["controls"].IsArray() ||
      definition["controls"].Size() > 64) {
    *error = "declarative UI definition is incompatible with schema 1";
    return false;
  }
  static const std::set<wxString> allowed_types = {
      "status",    "choice", "button",  "toggle",
      "file-open", "cancel", "progress"};
  std::set<wxString> identifiers;
  for (int i = 0; i < definition["controls"].Size(); ++i) {
    wxJSONValue control = definition["controls"][i];
    const wxString id = control["id"].AsString();
    const wxString type = control["type"].AsString();
    if (!control.IsObject() || id.empty() || id.length() > 64 ||
        !control["label"].IsString() || !allowed_types.count(type) ||
        !identifiers.insert(id).second) {
      *error = "declarative UI contains an invalid or duplicate control";
      return false;
    }
  }
  if (!identifiers.count("timeline") || !identifiers.count("open") ||
      !identifiers.count("generate")) {
    *error = "declarative UI omits a required iGRIB control";
    return false;
  }
  *title = definition["title"].AsString();
  return true;
}

wxString MakeResultPath(const wxString& directory, const wxString& operation) {
  const auto ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return directory + wxFILE_SEP_PATH +
         wxString::Format("%s-%lld.json", operation,
                          static_cast<long long>(ticks));
}

void AddRow(wxFlexGridSizer* grid, wxWindow* parent, const wxString& label,
            wxWindow* control) {
  grid->Add(new wxStaticText(parent, wxID_ANY, label), 0,
            wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  grid->Add(control, 1, wxEXPAND);
}

}  // namespace

class PortableGribHost::Impl : public wxEvtHandler {
public:
  enum class Operation { None, Inspect, DecodeFrame, Generate };

  Impl(wxWindow* parent_value, wxString package_root_value,
       bool credential_access_value)
      : parent(parent_value),
        package_root(std::move(package_root_value)),
        credential_access(credential_access_value),
        progress_timer(this, kProgressTimerId),
        playback_timer(this, kPlaybackTimerId) {
    Bind(wxEVT_END_PROCESS, &Impl::OnProcessEnded, this, kHelperProcessId);
    Bind(wxEVT_TIMER, &Impl::OnProgressTimer, this, kProgressTimerId);
    Bind(wxEVT_TIMER, &Impl::OnPlaybackTimer, this, kPlaybackTimerId);
  }

  ~Impl() override { Shutdown(); }

  bool Show(wxString* error);
  bool Render(ocpnDC& dc, const ViewPort& viewport);
  void SetCursorPosition(double latitude, double longitude);
  void Shutdown();

private:
  void CreateFrame();
  void OpenFile();
  void StartInspect(const wxString& path);
  void StartFrame(size_t index);
  void ShowGenerator();
  void ShowSettings();
  void LoadSettings();
  void SaveSettings();
  void UpdateCursorStatus();
  void Cancel();
  bool Launch(const std::vector<wxString>& arguments, Operation next_operation,
              const wxString& result, const wxString& status,
              const wxSecretValue* secret = nullptr);
  std::vector<wxString> DecoderCommand(const wxString& verb,
                                       const wxString& input,
                                       const wxString& time,
                                       const wxString& result) const;
  std::vector<wxString> GeneratorCommand(const wxString& job,
                                         const wxString& result,
                                         const wxString& output,
                                         const wxString& weather_input,
                                         const wxString& current_input) const;
  void OnProcessEnded(wxProcessEvent& event);
  void OnProgressTimer(wxTimerEvent& event);
  void OnPlaybackTimer(wxTimerEvent& event);
  void HandleInspect(wxJSONValue& value);
  void HandleFrame(wxJSONValue& value);
  void HandleGenerate(wxJSONValue& value);
  void SetBusy(bool busy, const wxString& status);
  wxString DecoderHelper() const;
  wxString GeneratorHelper() const;

  wxWindow* parent = nullptr;
  wxString package_root;
  bool credential_access = false;
  wxString surface_title;
  wxString private_directory;
  wxFrame* frame = nullptr;
  wxStaticText* file_label = nullptr;
  wxChoice* timeline = nullptr;
  wxCheckBox* show_wind = nullptr;
  wxCheckBox* show_pressure = nullptr;
  wxCheckBox* show_waves = nullptr;
  wxCheckBox* show_current = nullptr;
  wxCheckBox* show_temperature = nullptr;
  wxStaticText* wind_value = nullptr;
  wxStaticText* pressure_value = nullptr;
  wxStaticText* wave_value = nullptr;
  wxStaticText* current_value = nullptr;
  wxStaticText* temperature_value = nullptr;
  wxStaticText* data_status = nullptr;
  wxStaticText* cursor_status = nullptr;
  wxGauge* progress = nullptr;
  wxButton* cancel_button = nullptr;
  wxButton* open_button = nullptr;
  wxButton* generate_button = nullptr;
  wxButton* play_button = nullptr;
  wxTimer progress_timer;
  wxTimer playback_timer;
  wxProcess* process = nullptr;
  long process_id = 0;
  Operation operation = Operation::None;
  wxString result_path;
  wxString selected_file;
  wxString generated_output_path;
  std::vector<wxString> times;
  std::map<wxString, std::vector<Sample>> fields;
  std::map<wxString, wxString> field_units;
  double cursor_latitude = 0.0;
  double cursor_longitude = 0.0;
  bool have_cursor = false;
  bool wind_barbs = true;
  bool loop_playback = true;
  LayerDisplaySettings wind_display{
      0, true, false, false, false, 44, 70, 5, 0, wxColour(145, 88, 25)};
  LayerDisplaySettings pressure_display{
      0, false, false, false, true, 44, 70, 4, 0, wxColour(40, 90, 190)};
  LayerDisplaySettings wave_display{
      0, false, true, false, false, 44, 70, 1, 0, wxColour(0, 180, 210)};
  LayerDisplaySettings current_display{
      0, true, true, false, false, 44, 70, 1, 2, wxColour(30, 90, 220)};
  LayerDisplaySettings temperature_display{
      0, false, true, false, false, 44, 70, 2, 0, wxColour(220, 65, 35)};
  int overlay_opacity = 145;
  int playback_interval_ms = 1200;
  bool stopped = false;
};

wxString PortableGribHost::Impl::DecoderHelper() const {
  wxString path = package_root + wxFILE_SEP_PATH + "helpers" + wxFILE_SEP_PATH +
                  HostHelperDirectory() + wxFILE_SEP_PATH +
                  "igrib-environment-helper";
#if defined(__WXMSW__)
  path += ".exe";
#endif
  return path;
}

wxString PortableGribHost::Impl::GeneratorHelper() const {
  wxString path = package_root + wxFILE_SEP_PATH + "helpers" + wxFILE_SEP_PATH +
                  HostHelperDirectory() + wxFILE_SEP_PATH +
                  "environmental-grib";
#if defined(__WXMSW__)
  path += ".exe";
#endif
  return path;
}

void PortableGribHost::Impl::CreateFrame() {
  LoadSettings();
  frame = new wxFrame(parent, wxID_ANY, surface_title, wxDefaultPosition,
                      wxSize(940, 385),
                      wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT);
  auto* root = new wxBoxSizer(wxVERTICAL);
  file_label = new wxStaticText(frame, wxID_ANY, "File: (none)");
  root->Add(file_label, 0, wxEXPAND | wxALL, 7);
  root->Add(new wxStaticLine(frame), 0, wxEXPAND);

  auto* center = new wxBoxSizer(wxHORIZONTAL);
  auto* controls = new wxBoxSizer(wxVERTICAL);
  auto* timeline_row = new wxBoxSizer(wxHORIZONTAL);
  auto* previous =
      new wxButton(frame, wxID_ANY, "◀", wxDefaultPosition, wxSize(42, -1));
  timeline = new wxChoice(frame, wxID_ANY);
  auto* next =
      new wxButton(frame, wxID_ANY, "▶", wxDefaultPosition, wxSize(42, -1));
  play_button = new wxButton(frame, wxID_ANY, "Play");
  auto* now = new wxButton(frame, wxID_ANY, "Now");
  timeline_row->Add(previous, 0, wxRIGHT, 5);
  timeline_row->Add(timeline, 1, wxRIGHT, 5);
  timeline_row->Add(next, 0, wxRIGHT, 5);
  timeline_row->Add(play_button, 0, wxRIGHT, 5);
  timeline_row->Add(now, 0);
  controls->Add(timeline_row, 0, wxEXPAND | wxALL, 7);

  auto* data =
      new wxStaticBoxSizer(wxVERTICAL, frame, "Data at cursor position");
  auto* values = new wxFlexGridSizer(2, 4, 12);
  values->AddGrowableCol(1, 1);
  show_wind = new wxCheckBox(frame, wxID_ANY, "Wind");
  show_pressure = new wxCheckBox(frame, wxID_ANY, "Pressure");
  show_waves = new wxCheckBox(frame, wxID_ANY, "Waves");
  show_current = new wxCheckBox(frame, wxID_ANY, "Current");
  show_temperature = new wxCheckBox(frame, wxID_ANY, "Air Temp");
  const wxString old_path = pConfig->GetPath();
  pConfig->SetPath("/PortablePlugins/org.opencpn.igrib/Display");
  show_wind->SetValue(pConfig->ReadBool("showWind", true));
  show_pressure->SetValue(pConfig->ReadBool("showPressure", true));
  show_waves->SetValue(pConfig->ReadBool("showWaves", true));
  show_current->SetValue(pConfig->ReadBool("showCurrent", true));
  show_temperature->SetValue(pConfig->ReadBool("showTemperature", false));
  pConfig->SetPath(old_path);
  wind_value = new wxStaticText(frame, wxID_ANY, "N/A");
  pressure_value = new wxStaticText(frame, wxID_ANY, "N/A");
  wave_value = new wxStaticText(frame, wxID_ANY, "N/A");
  current_value = new wxStaticText(frame, wxID_ANY, "N/A");
  temperature_value = new wxStaticText(frame, wxID_ANY, "N/A");
  for (const auto& entry : std::vector<std::pair<wxCheckBox*, wxStaticText*>>{
           {show_wind, wind_value},
           {show_pressure, pressure_value},
           {show_waves, wave_value},
           {show_current, current_value},
           {show_temperature, temperature_value}}) {
    values->Add(entry.first, 0, wxALIGN_CENTER_VERTICAL);
    values->Add(entry.second, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
  }
  data_status = new wxStaticText(frame, wxID_ANY,
                                 "Open a GRIB file to inspect its fields");
  cursor_status = new wxStaticText(frame, wxID_ANY,
                                   "Move the chart cursor to inspect data "
                                   "(model output; not for navigation)");
  data->Add(values, 0, wxEXPAND | wxALL, 5);
  data->Add(data_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
  data->Add(cursor_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
  controls->Add(data, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 7);

  progress = new wxGauge(frame, wxID_ANY, 100);
  progress->Hide();
  controls->Add(progress, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 7);
  center->Add(controls, 1, wxEXPAND);

  auto* actions = new wxBoxSizer(wxVERTICAL);
  open_button = new wxButton(frame, wxID_ANY, "Open GRIB");
  auto* settings = new wxButton(frame, wxID_ANY, "Settings");
  auto* download = new wxButton(frame, wxID_ANY, "Download GRIB");
  generate_button = new wxButton(frame, wxID_ANY, "Generate GRIB");
  cancel_button = new wxButton(frame, wxID_ANY, "Cancel");
  cancel_button->Enable(false);
  for (auto* button :
       {open_button, settings, download, generate_button, cancel_button})
    actions->Add(button, 0, wxEXPAND | wxBOTTOM, 5);
  center->Add(actions, 0, wxEXPAND | wxALL, 7);
  root->Add(center, 1, wxEXPAND);
  frame->SetSizer(root);

  frame->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
    if (event.CanVeto()) {
      frame->Hide();
      event.Veto();
    } else {
      event.Skip();
    }
  });
  open_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenFile(); });
  generate_button->Bind(wxEVT_BUTTON,
                        [this](wxCommandEvent&) { ShowGenerator(); });
  download->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowGenerator(); });
  cancel_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Cancel(); });
  settings->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowSettings(); });
  previous->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    const int selected = timeline->GetSelection();
    if (selected > 0) StartFrame(static_cast<size_t>(selected - 1));
  });
  next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    const int selected = timeline->GetSelection();
    if (selected != wxNOT_FOUND &&
        static_cast<unsigned>(selected + 1) < timeline->GetCount())
      StartFrame(static_cast<size_t>(selected + 1));
  });
  now->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (times.empty()) return;
    const wxString current = wxDateTime::Now().ToUTC().Format("%Y%m%dT%H%MZ");
    size_t best = 0;
    for (size_t i = 0; i < times.size(); ++i)
      if (times[i] <= current) best = i;
    StartFrame(best);
  });
  play_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (playback_timer.IsRunning()) {
      playback_timer.Stop();
      play_button->SetLabel("Play");
    } else if (!times.empty()) {
      playback_timer.Start(playback_interval_ms);
      play_button->SetLabel("Pause");
    }
  });
  timeline->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    const int selected = timeline->GetSelection();
    if (selected != wxNOT_FOUND) StartFrame(static_cast<size_t>(selected));
  });
  for (auto* toggle :
       {show_wind, show_pressure, show_waves, show_current, show_temperature})
    toggle->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
      SaveSettings();
      if (top_frame::Get()) top_frame::Get()->RefreshAllCanvas(false);
    });
}

void PortableGribHost::Impl::LoadSettings() {
  const wxString old_path = pConfig->GetPath();
  pConfig->SetPath("/PortablePlugins/org.opencpn.igrib/Display");
  wind_barbs = pConfig->ReadBool("windBarbs", true);
  loop_playback = pConfig->ReadBool("loopPlayback", true);
  const int legacy_vector_spacing =
      std::clamp<int>(pConfig->ReadLong("vectorSpacing", 44), 24, 120);
  const bool legacy_scalar_maps = pConfig->ReadBool("scalarMaps", true);
  auto read_layer = [&](const wxString& name, LayerDisplaySettings* settings) {
    settings->units = std::clamp<int>(
        pConfig->ReadLong(name + "Units", settings->units), 0, 4);
    settings->vectors = pConfig->ReadBool(name + "Vectors", settings->vectors);
    settings->overlay = pConfig->ReadBool(name + "Overlay", settings->overlay);
    settings->numbers = pConfig->ReadBool(name + "Numbers", settings->numbers);
    settings->contours =
        pConfig->ReadBool(name + "Contours", settings->contours);
    settings->vector_spacing = std::clamp<int>(
        pConfig->ReadLong(name + "VectorSpacing", legacy_vector_spacing), 24,
        120);
    settings->number_spacing = std::clamp<int>(
        pConfig->ReadLong(name + "NumberSpacing", settings->number_spacing), 35,
        160);
    settings->contour_spacing = std::clamp<int>(
        pConfig->ReadLong(name + "ContourSpacing", settings->contour_spacing),
        1, 100);
    settings->vector_style = std::clamp<int>(
        pConfig->ReadLong(name + "VectorStyle", settings->vector_style), 0, 2);
    settings->proportional_base_size =
        std::clamp<int>(pConfig->ReadLong(name + "ProportionalBaseSize",
                                          settings->proportional_base_size),
                        6, 40);
    settings->proportional_growth_per_knot =
        std::clamp(pConfig->ReadDouble(name + "ProportionalGrowthPerKnot",
                                       settings->proportional_growth_per_knot),
                   0.0, 12.0);
    const wxString colour_name = pConfig->Read(
        name + "Colour", settings->colour.GetAsString(wxC2S_HTML_SYNTAX));
    const wxColour colour(colour_name);
    if (colour.IsOk()) settings->colour = colour;
  };
  read_layer("Wind", &wind_display);
  read_layer("Pressure", &pressure_display);
  read_layer("Wave", &wave_display);
  read_layer("Current", &current_display);
  read_layer("Temperature", &temperature_display);
  if (!pConfig->HasEntry("WindVectorStyle"))
    wind_display.vector_style = wind_barbs ? 0 : 1;
  if (!pConfig->HasEntry("WaveOverlay"))
    wave_display.overlay = legacy_scalar_maps;
  if (!pConfig->HasEntry("CurrentOverlay"))
    current_display.overlay = legacy_scalar_maps;
  if (!pConfig->HasEntry("TemperatureOverlay"))
    temperature_display.overlay = legacy_scalar_maps;
  overlay_opacity =
      std::clamp<int>(pConfig->ReadLong("overlayOpacity", 145), 20, 255);
  playback_interval_ms =
      std::clamp<int>(pConfig->ReadLong("playbackIntervalMs", 1200), 200, 5000);
  pConfig->SetPath(old_path);
}

void PortableGribHost::Impl::SaveSettings() {
  const wxString old_path = pConfig->GetPath();
  pConfig->SetPath("/PortablePlugins/org.opencpn.igrib/Display");
  pConfig->Write("windBarbs", wind_barbs);
  pConfig->Write("loopPlayback", loop_playback);
  auto write_layer = [&](const wxString& name,
                         const LayerDisplaySettings& settings) {
    pConfig->Write(name + "Units", static_cast<long>(settings.units));
    pConfig->Write(name + "Vectors", settings.vectors);
    pConfig->Write(name + "Overlay", settings.overlay);
    pConfig->Write(name + "Numbers", settings.numbers);
    pConfig->Write(name + "Contours", settings.contours);
    pConfig->Write(name + "VectorSpacing",
                   static_cast<long>(settings.vector_spacing));
    pConfig->Write(name + "NumberSpacing",
                   static_cast<long>(settings.number_spacing));
    pConfig->Write(name + "ContourSpacing",
                   static_cast<long>(settings.contour_spacing));
    pConfig->Write(name + "VectorStyle",
                   static_cast<long>(settings.vector_style));
    pConfig->Write(name + "ProportionalBaseSize",
                   static_cast<long>(settings.proportional_base_size));
    pConfig->Write(name + "ProportionalGrowthPerKnot",
                   settings.proportional_growth_per_knot);
    pConfig->Write(name + "Colour",
                   settings.colour.GetAsString(wxC2S_HTML_SYNTAX));
  };
  write_layer("Wind", wind_display);
  write_layer("Pressure", pressure_display);
  write_layer("Wave", wave_display);
  write_layer("Current", current_display);
  write_layer("Temperature", temperature_display);
  pConfig->Write("overlayOpacity", static_cast<long>(overlay_opacity));
  pConfig->Write("playbackIntervalMs", static_cast<long>(playback_interval_ms));
  if (show_wind) pConfig->Write("showWind", show_wind->GetValue());
  if (show_pressure) pConfig->Write("showPressure", show_pressure->GetValue());
  if (show_waves) pConfig->Write("showWaves", show_waves->GetValue());
  if (show_current) pConfig->Write("showCurrent", show_current->GetValue());
  if (show_temperature)
    pConfig->Write("showTemperature", show_temperature->GetValue());
  pConfig->SetPath(old_path);
  pConfig->Flush();
}

void PortableGribHost::Impl::ShowSettings() {
  wxDialog dialog(frame, wxID_ANY, "iGRIB display settings", wxDefaultPosition,
                  wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* root = new wxBoxSizer(wxVERTICAL);
  auto* notebook = new wxNotebook(&dialog, wxID_ANY);

  struct LayerControls {
    LayerDisplaySettings* settings = nullptr;
    wxChoice* units = nullptr;
    wxColourPickerCtrl* colour = nullptr;
    wxCheckBox* vectors = nullptr;
    wxChoice* vector_style = nullptr;
    wxSpinCtrl* vector_spacing = nullptr;
    wxSpinCtrl* proportional_base_size = nullptr;
    wxSpinCtrlDouble* proportional_growth_per_knot = nullptr;
    wxCheckBox* overlay = nullptr;
    wxCheckBox* numbers = nullptr;
    wxSpinCtrl* number_spacing = nullptr;
    wxCheckBox* contours = nullptr;
    wxSpinCtrl* contour_spacing = nullptr;
  };
  std::vector<LayerControls> layer_controls;
  auto add_layer = [&](const wxString& title, LayerDisplaySettings* layer,
                       const wxArrayString& unit_names, int vector_kind,
                       const wxString& contour_name) {
    auto* page = new wxPanel(notebook);
    auto* grid = new wxFlexGridSizer(2, 9, 12);
    grid->AddGrowableCol(1, 1);
    LayerControls controls;
    controls.settings = layer;
    controls.units = new wxChoice(page, wxID_ANY, wxDefaultPosition,
                                  wxDefaultSize, unit_names);
    controls.units->SetSelection(std::min<int>(
        layer->units, static_cast<int>(unit_names.GetCount()) - 1));
    AddRow(grid, page, "Units", controls.units);
    controls.colour = new wxColourPickerCtrl(page, wxID_ANY, layer->colour);
    AddRow(grid, page, "Display colour", controls.colour);
    if (vector_kind != 0) {
      controls.vectors =
          new wxCheckBox(page, wxID_ANY,
                         vector_kind == 1 ? "Display wind vectors"
                                          : "Display direction arrows");
      controls.vectors->SetValue(layer->vectors);
      AddRow(grid, page, "Vectors", controls.vectors);
      wxArrayString styles;
      if (vector_kind == 1) {
        styles.Add("Meteorological barbs");
        styles.Add("Direction arrows");
      } else {
        styles.Add("Single arrows");
        styles.Add("Double arrows");
        styles.Add("Proportional arrows");
      }
      controls.vector_style = new wxChoice(page, wxID_ANY, wxDefaultPosition,
                                           wxDefaultSize, styles);
      controls.vector_style->SetSelection(std::min<int>(
          layer->vector_style, static_cast<int>(styles.GetCount()) - 1));
      AddRow(grid, page,
             vector_kind == 1 ? "Wind vector style" : "Current arrow form",
             controls.vector_style);
      controls.vector_spacing = new wxSpinCtrl(
          page, wxID_ANY, wxString::Format("%d", layer->vector_spacing),
          wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 24, 120,
          layer->vector_spacing);
      AddRow(grid, page, "Vector spacing (pixels)", controls.vector_spacing);
      if (vector_kind == 2) {
        controls.proportional_base_size = new wxSpinCtrl(
            page, wxID_ANY,
            wxString::Format("%d", layer->proportional_base_size),
            wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 6, 40,
            layer->proportional_base_size);
        AddRow(grid, page, "Proportional baseline size (pixels)",
               controls.proportional_base_size);
        controls.proportional_growth_per_knot = new wxSpinCtrlDouble(
            page, wxID_ANY,
            wxString::Format("%.1f", layer->proportional_growth_per_knot),
            wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0.0, 12.0,
            layer->proportional_growth_per_knot, 0.5);
        controls.proportional_growth_per_knot->SetDigits(1);
        AddRow(grid, page, "Growth (pixels per knot)",
               controls.proportional_growth_per_knot);
        auto update_proportional_controls =
            [choice = controls.vector_style,
             base = controls.proportional_base_size,
             growth = controls.proportional_growth_per_knot]() {
              const bool enabled = choice->GetSelection() == 2;
              base->Enable(enabled);
              growth->Enable(enabled);
            };
        controls.vector_style->Bind(
            wxEVT_CHOICE, [update_proportional_controls](wxCommandEvent&) {
              update_proportional_controls();
            });
        update_proportional_controls();
      }
    }
    controls.overlay =
        new wxCheckBox(page, wxID_ANY, "Display colour overlay map");
    controls.overlay->SetValue(layer->overlay);
    AddRow(grid, page, "Overlay map", controls.overlay);
    controls.numbers =
        new wxCheckBox(page, wxID_ANY, "Display values on chart");
    controls.numbers->SetValue(layer->numbers);
    AddRow(grid, page, "Numbers", controls.numbers);
    controls.number_spacing = new wxSpinCtrl(
        page, wxID_ANY, wxString::Format("%d", layer->number_spacing),
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 35, 160,
        layer->number_spacing);
    AddRow(grid, page, "Number spacing (pixels)", controls.number_spacing);
    if (!contour_name.empty()) {
      controls.contours =
          new wxCheckBox(page, wxID_ANY, "Display " + contour_name);
      controls.contours->SetValue(layer->contours);
      AddRow(grid, page, contour_name, controls.contours);
      controls.contour_spacing = new wxSpinCtrl(
          page, wxID_ANY, wxString::Format("%d", layer->contour_spacing),
          wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 100,
          layer->contour_spacing);
      AddRow(grid, page, "Contour interval", controls.contour_spacing);
    }
    auto* page_root = new wxBoxSizer(wxVERTICAL);
    page_root->Add(grid, 1, wxEXPAND | wxALL, 14);
    page->SetSizer(page_root);
    notebook->AddPage(page, title);
    layer_controls.push_back(controls);
  };

  wxArrayString speed_units;
  speed_units.Add("knots");
  speed_units.Add("m/s");
  speed_units.Add("mph");
  speed_units.Add("km/h");
  wxArrayString pressure_units;
  pressure_units.Add("hPa");
  pressure_units.Add("mmHg");
  pressure_units.Add("inHg");
  wxArrayString height_units;
  height_units.Add("metres");
  height_units.Add("feet");
  wxArrayString temperature_units;
  temperature_units.Add("°C");
  temperature_units.Add("°F");
  add_layer("Wind", &wind_display, speed_units, 1, "isotachs");
  add_layer("Pressure", &pressure_display, pressure_units, 0, "isobars");
  add_layer("Waves", &wave_display, height_units, 0, wxEmptyString);
  add_layer("Current", &current_display, speed_units, 2, wxEmptyString);
  add_layer("Air temperature", &temperature_display, temperature_units, 0,
            "isotherms");

  auto* playback_page = new wxPanel(notebook);
  auto* playback_grid = new wxFlexGridSizer(2, 9, 12);
  playback_grid->AddGrowableCol(1, 1);
  auto* opacity =
      new wxSpinCtrl(playback_page, wxID_ANY,
                     wxString::Format("%d", overlay_opacity), wxDefaultPosition,
                     wxDefaultSize, wxSP_ARROW_KEYS, 20, 255, overlay_opacity);
  auto* playback = new wxSpinCtrl(
      playback_page, wxID_ANY, wxString::Format("%d", playback_interval_ms),
      wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 200, 5000,
      playback_interval_ms);
  auto* loop =
      new wxCheckBox(playback_page, wxID_ANY, "Loop timeline playback");
  loop->SetValue(loop_playback);
  AddRow(playback_grid, playback_page, "Overlay opacity (20–255)", opacity);
  AddRow(playback_grid, playback_page, "Playback interval (milliseconds)",
         playback);
  AddRow(playback_grid, playback_page, "Timeline", loop);
  auto* playback_root = new wxBoxSizer(wxVERTICAL);
  playback_root->Add(playback_grid, 1, wxEXPAND | wxALL, 14);
  playback_page->SetSizer(playback_root);
  notebook->AddPage(playback_page, "Playback");

  root->Add(notebook, 1, wxEXPAND | wxALL, 8);
  root->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0,
            wxEXPAND | wxALL, 10);
  dialog.SetSizer(root);
  dialog.SetSize(wxSize(680, 520));
  dialog.SetMinSize(wxSize(620, 460));
  if (dialog.ShowModal() != wxID_OK) return;
  for (const auto& controls : layer_controls) {
    auto& layer = *controls.settings;
    layer.units = controls.units->GetSelection();
    layer.colour = controls.colour->GetColour();
    if (controls.vectors) layer.vectors = controls.vectors->GetValue();
    if (controls.vector_spacing)
      layer.vector_spacing = controls.vector_spacing->GetValue();
    if (controls.proportional_base_size)
      layer.proportional_base_size =
          controls.proportional_base_size->GetValue();
    if (controls.proportional_growth_per_knot)
      layer.proportional_growth_per_knot =
          controls.proportional_growth_per_knot->GetValue();
    layer.overlay = controls.overlay->GetValue();
    layer.numbers = controls.numbers->GetValue();
    layer.number_spacing = controls.number_spacing->GetValue();
    if (controls.contours) layer.contours = controls.contours->GetValue();
    if (controls.contour_spacing)
      layer.contour_spacing = controls.contour_spacing->GetValue();
    if (controls.vector_style)
      layer.vector_style = controls.vector_style->GetSelection();
  }
  wind_barbs = wind_display.vector_style == 0;
  overlay_opacity = opacity->GetValue();
  playback_interval_ms = playback->GetValue();
  loop_playback = loop->GetValue();
  if (playback_timer.IsRunning()) playback_timer.Start(playback_interval_ms);
  SaveSettings();
  if (top_frame::Get()) top_frame::Get()->RefreshAllCanvas(false);
}

bool PortableGribHost::Impl::Show(wxString* error) {
  if (stopped) {
    *error = "environmental service is stopped";
    return false;
  }
  if (!wxFileExists(DecoderHelper())) {
    *error = "this package has no decoder helper for " + HostHelperDirectory();
    return false;
  }
  if (!HelperSupervisionAvailable(error)) return false;
  if (!LoadDeclarativeSurface(package_root + wxFILE_SEP_PATH + "ui" +
                                  wxFILE_SEP_PATH + "igrib-viewer.ui.json",
                              &surface_title, error))
    return false;
  private_directory = g_BasePlatform->GetPrivateDataDir() + wxFILE_SEP_PATH +
                      "portable-plugin-data" + wxFILE_SEP_PATH +
                      "org.opencpn.igrib";
  if (!wxDirExists(private_directory) &&
      !wxFileName::Mkdir(private_directory, 0700, wxPATH_MKDIR_FULL)) {
    *error = "could not create iGRIB private storage";
    return false;
  }
  if (!frame) CreateFrame();
  frame->Show();
  frame->Raise();
  return true;
}

void PortableGribHost::Impl::OpenFile() {
  wxFileDialog dialog(frame, "Open environmental GRIB", wxEmptyString,
                      wxEmptyString,
                      "GRIB files (*.grb;*.grib;*.grb2)|*.grb;*.grib;*.grb2|"
                      "All files (*.*)|*.*",
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
  if (dialog.ShowModal() == wxID_OK) StartInspect(dialog.GetPath());
}

std::vector<wxString> PortableGribHost::Impl::DecoderCommand(
    const wxString& verb, const wxString& input, const wxString& time,
    const wxString& result) const {
#if defined(__linux__)
  if (HelperSupervisionAvailable()) {
    std::vector<wxString> command = {"/usr/bin/prlimit", "--as=536870912",
                                     "--cpu=30", "--"};
    const std::vector<wxString> sandbox = {"/usr/bin/bwrap",
                                           "--die-with-parent",
                                           "--new-session",
                                           "--unshare-user",
                                           "--unshare-pid",
                                           "--unshare-ipc",
                                           "--unshare-uts",
                                           "--ro-bind",
                                           "/usr",
                                           "/usr",
                                           "--ro-bind",
                                           "/lib",
                                           "/lib",
                                           "--ro-bind",
                                           "/lib64",
                                           "/lib64",
                                           "--ro-bind",
                                           DecoderHelper(),
                                           "/igrib-helper",
                                           "--ro-bind",
                                           input,
                                           "/input.grb",
                                           "--bind",
                                           private_directory,
                                           "/output",
                                           "/igrib-helper",
                                           verb,
                                           "/input.grb"};
    command.insert(command.end(), sandbox.begin(), sandbox.end());
    if (verb == "frame") {
      command.push_back(time);
      command.push_back("12000");
    }
    command.push_back("/output/" + wxFileName(result).GetFullName());
    return command;
  }
#endif
  static_cast<void>(verb);
  static_cast<void>(input);
  static_cast<void>(time);
  static_cast<void>(result);
  return {};
}

std::vector<wxString> PortableGribHost::Impl::GeneratorCommand(
    const wxString& job, const wxString& result, const wxString& output,
    const wxString& weather_input, const wxString& current_input) const {
#if defined(__linux__)
  if (HelperSupervisionAvailable()) {
    std::vector<wxString> command = {"/usr/bin/prlimit", "--as=4294967296",
                                     "--cpu=1800", "--"};
    const wxString output_directory = wxFileName(output).GetPath();
    std::vector<wxString> sandbox = {"/usr/bin/bwrap",
                                     "--die-with-parent",
                                     "--new-session",
                                     "--unshare-user",
                                     "--unshare-pid",
                                     "--unshare-ipc",
                                     "--unshare-uts",
                                     "--share-net",
                                     "--ro-bind",
                                     "/usr",
                                     "/usr",
                                     "--ro-bind",
                                     "/lib",
                                     "/lib",
                                     "--ro-bind",
                                     "/lib64",
                                     "/lib64",
                                     "--ro-bind",
                                     "/etc/ssl",
                                     "/etc/ssl",
                                     "--ro-bind",
                                     "/etc/resolv.conf",
                                     "/etc/resolv.conf",
                                     "--ro-bind",
                                     GeneratorHelper(),
                                     "/environmental-grib",
                                     "--bind",
                                     private_directory,
                                     "/job",
                                     "--bind",
                                     output_directory,
                                     "/output",
                                     "--tmpfs",
                                     "/tmp"};
    if (wxDirExists("/etc/ca-certificates")) {
      sandbox.insert(sandbox.end(), {"--ro-bind", "/etc/ca-certificates",
                                     "/etc/ca-certificates"});
    }
    if (!weather_input.empty() || !current_input.empty()) {
      sandbox.insert(sandbox.end(), {"--dir", "/inputs"});
      if (!weather_input.empty())
        sandbox.insert(sandbox.end(),
                       {"--ro-bind", weather_input, "/inputs/weather.grb"});
      if (!current_input.empty())
        sandbox.insert(sandbox.end(),
                       {"--ro-bind", current_input, "/inputs/current.grb"});
    }
    command.insert(command.end(), sandbox.begin(), sandbox.end());
    command.insert(command.end(),
                   {"/environmental-grib", "run-job", "--job",
                    "/job/" + wxFileName(job).GetFullName(), "--result",
                    "/job/" + wxFileName(result).GetFullName()});
    return command;
  }
#endif
  static_cast<void>(job);
  static_cast<void>(result);
  static_cast<void>(output);
  static_cast<void>(weather_input);
  static_cast<void>(current_input);
  return {};
}

bool PortableGribHost::Impl::Launch(const std::vector<wxString>& arguments,
                                    Operation next_operation,
                                    const wxString& result,
                                    const wxString& status,
                                    const wxSecretValue* secret) {
  if (process) return false;
  if (arguments.empty()) {
    wxString error;
    HelperSupervisionAvailable(&error);
    wxMessageBox("Refusing unsupervised iGRIB helper execution: " + error,
                 "iGRIB", wxOK | wxICON_ERROR, frame);
    return false;
  }
  std::vector<const wchar_t*> argv;
  argv.reserve(arguments.size() + 1);
  for (const auto& argument : arguments) argv.push_back(argument.wc_str());
  argv.push_back(nullptr);
  process = new wxProcess(this, kHelperProcessId);
  wxExecuteEnv environment;
  wxExecuteEnv* environment_pointer = nullptr;
  wxSecretString transient_secret;
  if (next_operation == Operation::Generate) {
#if defined(_WIN32)
    for (const auto* name : {"SystemRoot", "SystemDrive", "COMSPEC", "PATH",
                             "PATHEXT", "TEMP", "TMP", "USERPROFILE"}) {
      wxString value;
      if (wxGetEnv(name, &value)) environment.env[name] = value;
    }
#else
    environment.env["HOME"] = "/tmp";
    environment.env["PATH"] = "/usr/bin:/bin";
#endif
    for (const auto* name :
         {"LANG", "LC_ALL", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
          "NO_PROXY", "http_proxy", "https_proxy", "all_proxy", "no_proxy"}) {
      wxString value;
      if (wxGetEnv(name, &value)) environment.env[name] = value;
    }
    if (secret && secret->IsOk()) {
      transient_secret = wxSecretString(*secret);
      environment.env[kCopernicusPasswordEnvironment] = transient_secret;
    }
    environment_pointer = &environment;
  }
  process_id = wxExecute(argv.data(), wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE,
                         process, environment_pointer);
  auto secret_entry = environment.env.find(kCopernicusPasswordEnvironment);
  if (secret_entry != environment.env.end()) {
    wxSecretValue::WipeString(secret_entry->second);
    environment.env.erase(secret_entry);
  }
  if (process_id == 0) {
    delete process;
    process = nullptr;
    wxMessageBox("Could not start the signed iGRIB helper", "iGRIB",
                 wxOK | wxICON_ERROR, frame);
    return false;
  }
  operation = next_operation;
  result_path = result;
  SetBusy(true, status);
  return true;
}

void PortableGribHost::Impl::StartInspect(const wxString& path) {
  if (process) return;
  selected_file = path;
  const wxString result = MakeResultPath(private_directory, "inspect");
  Launch(DecoderCommand("inspect", path, wxEmptyString, result),
         Operation::Inspect, result, "Inspecting GRIB metadata…");
}

void PortableGribHost::Impl::StartFrame(size_t index) {
  if (process || index >= times.size()) return;
  timeline->SetSelection(static_cast<int>(index));
  const wxString result = MakeResultPath(private_directory, "frame");
  Launch(DecoderCommand("frame", selected_file, times[index], result),
         Operation::DecodeFrame, result, "Decoding environmental frame…");
}

void PortableGribHost::Impl::SetBusy(bool busy, const wxString& status) {
  if (!frame) return;
  open_button->Enable(!busy);
  generate_button->Enable(!busy);
  timeline->Enable(!busy);
  cancel_button->Enable(busy);
  progress->Show(busy);
  if (busy) {
    progress->Pulse();
    progress_timer.Start(120);
  } else {
    progress_timer.Stop();
    progress->SetValue(0);
  }
  if (!status.empty()) data_status->SetLabel(status);
  frame->Layout();
}

void PortableGribHost::Impl::OnProgressTimer(wxTimerEvent&) {
  if (process && progress) progress->Pulse();
}

void PortableGribHost::Impl::OnPlaybackTimer(wxTimerEvent&) {
  if (process || times.empty() || !timeline) return;
  const int selected = timeline->GetSelection();
  if (!loop_playback && selected != wxNOT_FOUND &&
      static_cast<size_t>(selected + 1) >= times.size()) {
    playback_timer.Stop();
    if (play_button) play_button->SetLabel("Play");
    return;
  }
  const size_t next = selected == wxNOT_FOUND
                          ? 0
                          : (static_cast<size_t>(selected) + 1) % times.size();
  StartFrame(next);
}

void PortableGribHost::Impl::Cancel() {
  if (!process || process_id <= 0) return;
  wxProcess::Kill(static_cast<int>(process_id), wxSIGTERM, wxKILL_CHILDREN);
  data_status->SetLabel("Cancellation requested…");
}

void PortableGribHost::Impl::OnProcessEnded(wxProcessEvent& event) {
  const Operation completed = operation;
  const wxString completed_result = result_path;
  operation = Operation::None;
  result_path.clear();
  process_id = 0;
  delete process;
  process = nullptr;
  SetBusy(false, wxEmptyString);
  if (stopped) return;

  wxJSONValue value;
  wxString error;
  if (!ReadJson(completed_result, &value, &error)) {
    data_status->SetLabel("Failed: " + error);
    wxLogError("iGRIB supervised helper failed (exit %d): %s",
               event.GetExitCode(), error);
    wxRemoveFile(completed_result);
    return;
  }
  wxRemoveFile(completed_result);
  if (event.GetExitCode() != 0) {
    data_status->SetLabel(
        wxString::Format("Helper exited with status %d", event.GetExitCode()));
    return;
  }
  switch (completed) {
    case Operation::Inspect:
      HandleInspect(value);
      break;
    case Operation::DecodeFrame:
      HandleFrame(value);
      break;
    case Operation::Generate:
      HandleGenerate(value);
      break;
    default:
      break;
  }
}

void PortableGribHost::Impl::HandleInspect(wxJSONValue& value) {
  if (!value["times"].IsArray() || !value["fields"].IsArray()) {
    data_status->SetLabel("Decoder returned an incompatible metadata schema");
    return;
  }
  times.clear();
  timeline->Clear();
  for (int i = 0; i < value["times"].Size(); ++i) {
    if (!value["times"][i].IsString()) continue;
    times.push_back(value["times"][i].AsString());
    timeline->Append(FormatGribTime(times.back()));
  }
  file_label->SetLabel("File: " + wxFileName(selected_file).GetFullName());
  data_status->SetLabel(wxString::Format(
      "%d GRIB messages; %zu forecast times; decoded out of process",
      value["messageCount"].AsInt(), times.size()));
  if (!times.empty()) StartFrame(0);
}

void PortableGribHost::Impl::HandleFrame(wxJSONValue& value) {
  if (!value["fields"].IsArray()) {
    data_status->SetLabel("Decoder returned an incompatible frame schema");
    return;
  }
  fields.clear();
  field_units.clear();
  for (int i = 0; i < value["fields"].Size(); ++i) {
    wxJSONValue field = value["fields"][i];
    if (!field.IsObject() || !field["kind"].IsString() ||
        !field["samples"].IsArray())
      continue;
    const wxString kind = field["kind"].AsString();
    auto& output = fields[kind];
    field_units[kind] = field["unit"].AsString();
    output.reserve(field["samples"].Size());
    for (int j = 0; j < field["samples"].Size(); ++j) {
      wxJSONValue sample = field["samples"][j];
      if (!sample.IsArray() || sample.Size() != 3) continue;
      const double latitude = sample[0].AsDouble();
      const double longitude = sample[1].AsDouble();
      const double sample_value = sample[2].AsDouble();
      if (std::isfinite(latitude) && std::isfinite(longitude) &&
          std::isfinite(sample_value))
        output.push_back({latitude, longitude, sample_value});
    }
  }
  data_status->SetLabel(wxString::Format(
      "%s — %d samples retained by OpenCPN (not navigation-authoritative)",
      FormatGribTime(value["time"].AsString()), value["sampleCount"].AsInt()));
  UpdateCursorStatus();
  if (top_frame::Get()) top_frame::Get()->RefreshAllCanvas(false);
}

void PortableGribHost::Impl::HandleGenerate(wxJSONValue& value) {
  const wxString status = value["status"].AsString();
  const wxString output = generated_output_path;
  generated_output_path.clear();
  if (status == "complete") {
    data_status->SetLabel("Environmental GRIB generated successfully");
    if (!output.empty() && wxFileExists(output)) StartInspect(output);
  } else {
    wxString message = value["error"]["message"].AsString();
    if (message.empty()) message = "generator returned an incomplete result";
    data_status->SetLabel("Generation failed: " + message);
  }
}

void PortableGribHost::Impl::ShowGenerator() {
  if (process) return;
  if (!wxFileExists(GeneratorHelper())) {
    wxMessageBox("This package has no environmental generator helper for " +
                     HostHelperDirectory(),
                 "iGRIB", wxOK | wxICON_WARNING, frame);
    return;
  }

  wxSecretStore secret_store = wxSecretStore::GetDefault();
  wxString secret_store_error;
  const bool secret_store_available =
      credential_access && secret_store.IsOk(&secret_store_error);
  wxString stored_username;
  wxSecretValue stored_password;
  bool have_stored_credentials =
      secret_store_available &&
      secret_store.Load(kCopernicusCredentialService, stored_username,
                        stored_password);

  wxDialog dialog(frame, wxID_ANY, "Environmental GRIB Generator",
                  wxDefaultPosition, wxSize(820, 760),
                  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* root = new wxBoxSizer(wxVERTICAL);
  auto* form = new wxScrolledWindow(&dialog, wxID_ANY, wxDefaultPosition,
                                    wxDefaultSize, wxVSCROLL);
  auto* grid = new wxFlexGridSizer(2, 6, 8);
  grid->AddGrowableCol(1, 1);
  auto* executable =
      new wxTextCtrl(form, wxID_ANY, GeneratorHelper(), wxDefaultPosition,
                     wxDefaultSize, wxTE_READONLY);
  auto* west = new wxTextCtrl(form, wxID_ANY, "-8.5");
  auto* south = new wxTextCtrl(form, wxID_ANY, "50.5");
  auto* east = new wxTextCtrl(form, wxID_ANY, "-2.5");
  auto* north = new wxTextCtrl(form, wxID_ANY, "56.5");
  auto* start = new wxTextCtrl(
      form, wxID_ANY, wxDateTime::Now().ToUTC().Format("%Y-%m-%dT%H:00:00Z"));
  auto* hours = new wxSpinCtrl(form, wxID_ANY, "72", wxDefaultPosition,
                               wxDefaultSize, wxSP_ARROW_KEYS, 1, 360, 72);
  auto* step = new wxSpinCtrl(form, wxID_ANY, "3", wxDefaultPosition,
                              wxDefaultSize, wxSP_ARROW_KEYS, 1, 24, 3);
  wxArrayString weather_providers;
  weather_providers.Add("NOAA GFS forecast");
  weather_providers.Add("UK Met Office UKV");
  weather_providers.Add("Local GRIB file…");
  constexpr int kLocalWeatherProvider = 2;
  auto* provider = new wxChoice(form, wxID_ANY, wxDefaultPosition,
                                wxDefaultSize, weather_providers);
  provider->SetSelection(0);
  wxArrayString presets;
  presets.Add("Routing");
  presets.Add("Viewer");
  auto* preset =
      new wxChoice(form, wxID_ANY, wxDefaultPosition, wxDefaultSize, presets);
  preset->SetSelection(0);
  auto* waves = new wxCheckBox(form, wxID_ANY, "Include wave fields");
  auto make_input_picker = [&](wxTextCtrl** path, wxButton** browse) {
    auto* panel = new wxPanel(form);
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);
    *path = new wxTextCtrl(panel, wxID_ANY);
    *browse = new wxButton(panel, wxID_ANY, "Browse…");
    sizer->Add(*path, 1, wxEXPAND | wxRIGHT, 6);
    sizer->Add(*browse, 0, wxEXPAND);
    panel->SetSizer(sizer);
    return panel;
  };
  wxTextCtrl* local_weather = nullptr;
  wxButton* browse_weather = nullptr;
  auto* weather_input_panel =
      make_input_picker(&local_weather, &browse_weather);
  wxArrayString current_sources;
  current_sources.Add("None");
  if (credential_access) {
    current_sources.Add("Copernicus Marine North-West Shelf (hourly, ~1.5 km)");
    current_sources.Add("Copernicus Marine Global (hourly, ~1/12 degree)");
  }
  const int local_current_source = static_cast<int>(current_sources.GetCount());
  current_sources.Add("Local GRIB file…");
  auto* current_source = new wxChoice(form, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, current_sources);
  current_source->SetSelection(credential_access ? 1 : 0);
  auto* current_note =
      new wxStaticText(form, wxID_ANY,
                       "North-West Shelf coverage: 20 W to 13 E, 40 N to 65 N. "
                       "Use Global outside this area.");
  current_note->Wrap(520);
  wxTextCtrl* local_current = nullptr;
  wxButton* browse_current = nullptr;
  auto* current_input_panel =
      make_input_picker(&local_current, &browse_current);
  auto* account = new wxHyperlinkCtrl(
      form, wxID_ANY, "Create or manage a free Copernicus Marine account",
      "https://data.marine.copernicus.eu/register");
  auto* username = new wxTextCtrl(form, wxID_ANY, stored_username);
  wxSecretString initial_password;
  if (have_stored_credentials)
    initial_password.assign(stored_password.GetAsString());
  auto* password =
      new wxTextCtrl(form, wxID_ANY, initial_password, wxDefaultPosition,
                     wxDefaultSize, wxTE_PASSWORD);
  auto* remember = new wxCheckBox(form, wxID_ANY,
                                  "Save in operating-system credential store");
  remember->SetValue(secret_store_available);
  remember->Enable(secret_store_available);
  if (!secret_store_available) {
    remember->SetToolTip("Credential storage unavailable: " +
                         secret_store_error);
  }
  auto* forget = new wxButton(form, wxID_ANY, "Forget saved login");
  forget->Enable(have_stored_credentials);
  wxString default_output_directory = wxStandardPaths::Get().GetDocumentsDir();
  if (!wxDirExists(default_output_directory))
    default_output_directory = wxGetHomeDir();
  const wxString default_output_name =
      wxDateTime::Now().ToUTC().Format("environment_igrib_%Y%m%d_%H%M.grb");
  auto* output_panel = new wxPanel(form);
  auto* output_sizer = new wxBoxSizer(wxHORIZONTAL);
  auto* output_path = new wxTextCtrl(
      output_panel, wxID_ANY,
      wxFileName(default_output_directory, default_output_name).GetFullPath());
  auto* browse_output = new wxButton(output_panel, wxID_ANY, "Browse…");
  output_sizer->Add(output_path, 1, wxEXPAND | wxRIGHT, 6);
  output_sizer->Add(browse_output, 0, wxEXPAND);
  output_panel->SetSizer(output_sizer);
  AddRow(grid, form, "Generator executable", executable);
  AddRow(grid, form, "West longitude", west);
  AddRow(grid, form, "South latitude", south);
  AddRow(grid, form, "East longitude", east);
  AddRow(grid, form, "North latitude", north);
  AddRow(grid, form, "Start UTC", start);
  AddRow(grid, form, "Forecast duration hours (maximum 15 days)", hours);
  AddRow(grid, form, "Step hours", step);
  AddRow(grid, form, "Weather provider", provider);
  AddRow(grid, form, "Weather preset", preset);
  AddRow(grid, form, "Waves", waves);
  AddRow(grid, form, "Weather GRIB file", weather_input_panel);
  AddRow(grid, form, "Current source", current_source);
  AddRow(grid, form, "Current-source details", current_note);
  AddRow(grid, form, "Current GRIB file", current_input_panel);
  AddRow(grid, form, "Copernicus account", account);
  AddRow(grid, form, "Copernicus username or email", username);
  AddRow(grid, form, "Copernicus password", password);
  AddRow(grid, form, "Credential storage", remember);
  AddRow(grid, form, "Saved credentials", forget);
  AddRow(grid, form, "Output GRIB", output_panel);
  form->SetSizer(grid);
  form->SetScrollRate(0, 12);
  root->Add(form, 1, wxEXPAND | wxALL, 10);
  root->Add(new wxStaticText(
                &dialog, wxID_ANY,
                "Generated environmental data is model output for planning "
                "and experimentation; it is not an official navigation "
                "product."),
            0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
  auto* buttons = dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL);
  if (auto* generate = wxDynamicCast(dialog.FindWindow(wxID_OK), wxButton))
    generate->SetLabel("Generate GRIB");
  root->Add(buttons, 0, wxEXPAND | wxALL, 10);
  dialog.SetSizer(root);

  auto update_weather_controls = [&]() {
    const bool local = provider->GetSelection() == kLocalWeatherProvider;
    local_weather->Enable(local);
    browse_weather->Enable(local);
    preset->Enable(!local);
    waves->Enable(!local);
    if (local)
      waves->SetToolTip(
          "Wave records already present in the local file are preserved");
    else
      waves->UnsetToolTip();
  };
  auto update_current_controls = [&]() {
    const int selection = current_source->GetSelection();
    const bool local = selection == local_current_source;
    const bool copernicus =
        credential_access && (selection == 1 || selection == 2);
    local_current->Enable(local);
    browse_current->Enable(local);
    account->Enable(copernicus);
    username->Enable(copernicus);
    password->Enable(copernicus);
    remember->Enable(copernicus && secret_store_available);
    forget->Enable(copernicus && have_stored_credentials);
    current_note->SetLabel(
        local ? "The selected local GRIB replaces the online current source."
        : selection == 1
            ? "North-West Shelf coverage: 20 W to 13 E, 40 N to 65 N. "
              "Use Global outside this area."
        : selection == 2
            ? "Global model coverage: 180 W to 180 E, 80 S to 90 N."
            : "No current fields will be included.");
    current_note->Wrap(520);
    dialog.Layout();
  };
  current_source->Bind(wxEVT_CHOICE,
                       [&](wxCommandEvent&) { update_current_controls(); });
  provider->Bind(wxEVT_CHOICE,
                 [&](wxCommandEvent&) { update_weather_controls(); });
  auto bind_input_picker = [&](wxButton* button, wxTextCtrl* path,
                               const wxString& title) {
    button->Bind(wxEVT_BUTTON, [&, path, title](wxCommandEvent&) {
      wxFileDialog input_dialog(
          &dialog, title, wxEmptyString, wxEmptyString,
          "GRIB files (*.grb;*.grib;*.grb2)|*.grb;*.grib;*.grb2|"
          "All files (*.*)|*.*",
          wxFD_OPEN | wxFD_FILE_MUST_EXIST);
      if (input_dialog.ShowModal() == wxID_OK)
        path->SetValue(input_dialog.GetPath());
    });
  };
  bind_input_picker(browse_weather, local_weather,
                    "Select a local weather GRIB to merge");
  bind_input_picker(browse_current, local_current,
                    "Select a local current GRIB to merge");
  forget->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    if (have_stored_credentials &&
        secret_store.Delete(kCopernicusCredentialService)) {
      have_stored_credentials = false;
      stored_username.clear();
      username->Clear();
      password->Clear();
      remember->SetValue(false);
      forget->Enable(false);
    }
  });
  browse_output->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    wxFileName selected(output_path->GetValue());
    wxString directory = selected.GetPath();
    if (!wxDirExists(directory)) directory = wxGetHomeDir();
    wxFileDialog output_dialog(
        &dialog, "Choose where to save the generated environmental GRIB",
        directory, selected.GetFullName(),
        "GRIB files (*.grb)|*.grb|All files (*.*)|*.*",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (output_dialog.ShowModal() == wxID_OK)
      output_path->SetValue(output_dialog.GetPath());
  });
  update_weather_controls();
  update_current_controls();
  if (dialog.ShowModal() != wxID_OK) return;

  if (hours->GetValue() % step->GetValue() != 0) {
    wxMessageBox(
        "Forecast duration must be evenly divisible by the step interval. "
        "For example, use 1 hour with a 1-hour step or 72 hours with a "
        "3-hour step.",
        "iGRIB", wxOK | wxICON_ERROR, frame);
    return;
  }

  wxString requested_output = output_path->GetValue();
  requested_output.Trim(true).Trim(false);
  wxFileName output_filename(requested_output);
  if (requested_output.empty() || !output_filename.IsAbsolute()) {
    wxMessageBox("Choose an absolute output path for the generated GRIB",
                 "iGRIB", wxOK | wxICON_ERROR, frame);
    return;
  }
  if (output_filename.GetExt().empty()) output_filename.SetExt("grb");
  output_filename.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
  const wxString output_directory = output_filename.GetPath();
  if (!wxDirExists(output_directory) ||
      !wxFileName::IsDirWritable(output_directory)) {
    wxMessageBox(
        "The selected output directory does not exist or is not "
        "writable",
        "iGRIB", wxOK | wxICON_ERROR, frame);
    return;
  }
  if (output_filename.FileExists() &&
      wxMessageBox(
          "Replace the existing GRIB file?\n" + output_filename.GetFullPath(),
          "iGRIB", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, frame) != wxYES)
    return;

  auto validate_input = [&](wxTextCtrl* control, const wxString& label,
                            bool required, wxString* value) {
    *value = control->GetValue();
    value->Trim(true).Trim(false);
    if (value->empty()) {
      if (required) {
        wxMessageBox("Choose " + label + " or select an online source", "iGRIB",
                     wxOK | wxICON_ERROR, frame);
        return false;
      }
      return true;
    }
    wxFileName file(*value);
    if (!file.IsAbsolute() || !file.FileExists()) {
      wxMessageBox(label + " must be an existing GRIB file", "iGRIB",
                   wxOK | wxICON_ERROR, frame);
      return false;
    }
    file.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
    *value = file.GetFullPath();
    return true;
  };
  wxString weather_input;
  wxString current_input;
  const bool local_weather_selected =
      provider->GetSelection() == kLocalWeatherProvider;
  const bool local_current_selected =
      current_source->GetSelection() == local_current_source;
  if ((local_weather_selected &&
       !validate_input(local_weather, "a local weather GRIB", true,
                       &weather_input)) ||
      (local_current_selected &&
       !validate_input(local_current, "a local current GRIB", true,
                       &current_input)))
    return;

  const bool use_copernicus =
      credential_access && (current_source->GetSelection() == 1 ||
                            current_source->GetSelection() == 2);
  wxString copernicus_username = username->GetValue();
  copernicus_username.Trim(true).Trim(false);
  wxSecretString copernicus_password(password->GetValue());
  wxSecretValue copernicus_secret;
  if (use_copernicus) {
    if (copernicus_username.empty() || copernicus_password.empty()) {
      wxMessageBox(
          "Copernicus Marine currents require the username (or email) and "
          "password for a free Copernicus Marine account.",
          "iGRIB", wxOK | wxICON_ERROR, frame);
      return;
    }
    copernicus_secret = wxSecretValue(copernicus_password);
    if (remember->GetValue() &&
        !secret_store.Save(kCopernicusCredentialService, copernicus_username,
                           copernicus_secret)) {
      wxMessageBox(
          "The Copernicus login could not be saved securely. It will be "
          "used for this generation only.",
          "iGRIB", wxOK | wxICON_WARNING, frame);
    }
  }
  password->Clear();

  double west_value = 0, south_value = 0, east_value = 0, north_value = 0;
  if (!west->GetValue().ToDouble(&west_value) ||
      !south->GetValue().ToDouble(&south_value) ||
      !east->GetValue().ToDouble(&east_value) ||
      !north->GetValue().ToDouble(&north_value)) {
    wxMessageBox("Bounding coordinates must be numbers", "iGRIB",
                 wxOK | wxICON_ERROR, frame);
    return;
  }

  wxJSONValue job;
  job["schemaVersion"] = 1;
  job["operation"] = wxString("generateEnvironment");
  auto& request = job["request"];
  request["bbox"]["west"] = west_value;
  request["bbox"]["south"] = south_value;
  request["bbox"]["east"] = east_value;
  request["bbox"]["north"] = north_value;
  request["start"] = start->GetValue();
  request["hours"] = hours->GetValue();
  request["stepHours"] = step->GetValue();
  request["weatherProvider"] =
      wxString(local_weather_selected
                   ? "existing-file"
                   : (provider->GetSelection() == 1 ? "ukmo_ukv" : "gfs"));
  if (local_weather_selected)
    request["weatherFile"] = wxString("/inputs/weather.grb");
  request["weatherPreset"] =
      wxString(preset->GetSelection() == 1 ? "viewer" : "routing");
  // A user-selected weather file is copied as one validated stream, including
  // any wave records it already contains.  Do not unexpectedly contact an
  // online wave provider while performing an otherwise local merge.
  request["includeWaves"] = !local_weather_selected && waves->GetValue();
  request["waveProvider"] = wxString("gfs_wave");
  request["currentSource"] =
      wxString(local_current_selected                ? "existing-file"
               : current_source->GetSelection() == 1 ? "copernicus_nws"
               : current_source->GetSelection() == 2 ? "copernicus_global"
                                                     : "none");
  if (local_current_selected)
    request["currentFile"] = wxString("/inputs/current.grb");
  if (use_copernicus) {
    request["copernicusUsername"] = copernicus_username;
    request["currentGridSpacingDeg"] =
        current_source->GetSelection() == 1 ? 0.05 : 0.1;
    job["credentials"]["copernicusPasswordEnvironment"] =
        wxString(kCopernicusPasswordEnvironment);
  }
  const bool sandboxed_generator = HelperSupervisionAvailable();
  request["output"] = sandboxed_generator
                          ? "/output/" + output_filename.GetFullName()
                          : output_filename.GetFullPath();
  request["overwrite"] = true;

  const wxString job_path = MakeResultPath(private_directory, "generate-job");
  const wxString result = MakeResultPath(private_directory, "generate-result");
  wxFileOutputStream job_output(job_path);
  if (!job_output.IsOk()) {
    wxMessageBox("Could not create the private generator request", "iGRIB",
                 wxOK | wxICON_ERROR, frame);
    return;
  }
  wxJSONWriter writer(wxJSONWRITER_STYLED);
  writer.Write(job, job_output);
  job_output.Close();
  generated_output_path = output_filename.GetFullPath();
  Launch(GeneratorCommand(job_path, result, generated_output_path,
                          weather_input, current_input),
         Operation::Generate, result,
         "Generating environmental GRIB in supervised helper…",
         use_copernicus ? &copernicus_secret : nullptr);
}

void PortableGribHost::Impl::SetCursorPosition(double latitude,
                                               double longitude) {
  if (!std::isfinite(latitude) || !std::isfinite(longitude)) return;
  cursor_latitude = latitude;
  cursor_longitude = longitude;
  have_cursor = true;
  UpdateCursorStatus();
}

void PortableGribHost::Impl::UpdateCursorStatus() {
  if (!cursor_status || !have_cursor || fields.empty()) return;
  auto nearest = [&](const wxString& kind, double* value) {
    const auto found = fields.find(kind);
    if (found == fields.end() || found->second.empty()) return false;
    const double longitude_scale =
        std::max(0.1, std::cos(cursor_latitude * kPi / 180.0));
    double best_distance = std::numeric_limits<double>::max();
    for (const auto& sample : found->second) {
      const double dy = sample.latitude - cursor_latitude;
      const double dx = (sample.longitude - cursor_longitude) * longitude_scale;
      const double distance = dx * dx + dy * dy;
      if (distance < best_distance) {
        best_distance = distance;
        *value = sample.value;
      }
    }
    return best_distance < 4.0;
  };
  auto speed_value = [](double metres_per_second, int units) {
    switch (units) {
      case 1:
        return wxString::Format("%.1f m/s", metres_per_second);
      case 2:
        return wxString::Format("%.1f mph", metres_per_second * 2.23693629);
      case 3:
        return wxString::Format("%.1f km/h", metres_per_second * 3.6);
      default:
        return wxString::Format("%.1f kt", metres_per_second * 1.94384449);
    }
  };
  if (wind_value) wind_value->SetLabel("N/A");
  if (pressure_value) pressure_value->SetLabel("N/A");
  if (wave_value) wave_value->SetLabel("N/A");
  if (current_value) current_value->SetLabel("N/A");
  if (temperature_value) temperature_value->SetLabel("N/A");
  double u = 0.0, v = 0.0, value = 0.0;
  if (nearest("wind-u", &u) && nearest("wind-v", &v)) {
    double from = std::atan2(-u, -v) * 180.0 / kPi;
    if (from < 0.0) from += 360.0;
    wind_value->SetLabel(speed_value(std::hypot(u, v), wind_display.units) +
                         wxString::Format("  %03.0f° from", from));
  }
  if (nearest("pressure", &value)) {
    const double hpa = PressureHpa(value);
    if (pressure_display.units == 1)
      pressure_value->SetLabel(
          wxString::Format("%.1f mmHg", hpa * 0.750061683));
    else if (pressure_display.units == 2)
      pressure_value->SetLabel(
          wxString::Format("%.2f inHg", hpa * 0.0295299831));
    else
      pressure_value->SetLabel(wxString::Format("%.1f hPa", hpa));
  }
  if (nearest("wave-height", &value)) {
    wave_value->SetLabel(wave_display.units == 1
                             ? wxString::Format("%.1f ft", value * 3.2808399)
                             : wxString::Format("%.2f m", value));
  }
  if (nearest("current-u", &u) && nearest("current-v", &v)) {
    double toward = std::atan2(u, v) * 180.0 / kPi;
    if (toward < 0.0) toward += 360.0;
    current_value->SetLabel(
        speed_value(std::hypot(u, v), current_display.units) +
        wxString::Format("  %03.0f° toward", toward));
  }
  if (nearest("air-temperature", &value)) {
    const double celsius = TemperatureCelsius(value);
    temperature_value->SetLabel(
        temperature_display.units == 1
            ? wxString::Format("%.1f °F", celsius * 9.0 / 5.0 + 32.0)
            : wxString::Format("%.1f °C", celsius));
  }
  cursor_status->SetLabel(wxString::Format(
      "Cursor %.4f° %c  %.4f° %c — model output; not navigation-authoritative",
      std::abs(cursor_latitude), cursor_latitude >= 0 ? 'N' : 'S',
      std::abs(cursor_longitude), cursor_longitude >= 0 ? 'E' : 'W'));
  if (frame) frame->Layout();
}

bool PortableGribHost::Impl::Render(ocpnDC& dc, const ViewPort& viewport) {
  if (fields.empty()) return false;
  bool rendered = false;
  ViewPort projection = viewport;

  auto vector_samples = [&](const wxString& u_name, const wxString& v_name) {
    std::vector<Sample> output;
    const auto u = fields.find(u_name);
    const auto v = fields.find(v_name);
    if (u == fields.end() || v == fields.end()) return output;
    const size_t count = std::min(u->second.size(), v->second.size());
    output.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const auto& us = u->second[i];
      const auto& vs = v->second[i];
      if (std::abs(us.latitude - vs.latitude) <= 0.001 &&
          std::abs(us.longitude - vs.longitude) <= 0.001)
        output.push_back(
            {us.latitude, us.longitude, std::hypot(us.value, vs.value)});
    }
    return output;
  };

  auto draw_wind_barb = [&](const wxPoint& origin, double u, double v,
                            bool southern_hemisphere, const wxColour& colour) {
    const double speed = std::hypot(u, v);
    const double knots = speed * 1.94384449;
    dc.SetPen(wxPen(colour, 2, wxPENSTYLE_SOLID));
    dc.SetBrush(wxBrush(colour));
    if (knots < 2.5) {
      dc.SetBrush(*wxTRANSPARENT_BRUSH);
      dc.DrawCircle(origin, 4);
      return;
    }
    // The staff points into the direction from which the wind arrives.
    const double staff_x = -u / speed;
    const double staff_y = v / speed;
    const double hemisphere = southern_hemisphere ? -1.0 : 1.0;
    const double perpendicular_x = -staff_y * hemisphere;
    const double perpendicular_y = staff_x * hemisphere;
    constexpr double length = 28.0;
    const wxPoint tip(
        origin.x + static_cast<int>(std::lround(staff_x * length)),
        origin.y + static_cast<int>(std::lround(staff_y * length)));
    dc.DrawLine(origin.x, origin.y, tip.x, tip.y);
    int remaining = static_cast<int>(std::floor((knots + 2.5) / 5.0)) * 5;
    double offset = 0.0;
    while (remaining >= 50) {
      const wxPoint first(
          tip.x - static_cast<int>(std::lround(staff_x * offset)),
          tip.y - static_cast<int>(std::lround(staff_y * offset)));
      const wxPoint second(
          tip.x - static_cast<int>(std::lround(staff_x * (offset + 6.0))),
          tip.y - static_cast<int>(std::lround(staff_y * (offset + 6.0))));
      wxPoint triangle[3] = {
          first, second,
          wxPoint(first.x + static_cast<int>(std::lround(
                                perpendicular_x * 12.0 + staff_x * 4.0)),
                  first.y + static_cast<int>(std::lround(
                                perpendicular_y * 12.0 + staff_y * 4.0)))};
      dc.DrawPolygon(3, triangle);
      remaining -= 50;
      offset += 8.0;
    }
    while (remaining >= 10) {
      const wxPoint base(
          tip.x - static_cast<int>(std::lround(staff_x * offset)),
          tip.y - static_cast<int>(std::lround(staff_y * offset)));
      dc.DrawLine(base.x, base.y,
                  base.x + static_cast<int>(std::lround(perpendicular_x * 12.0 +
                                                        staff_x * 4.0)),
                  base.y + static_cast<int>(std::lround(perpendicular_y * 12.0 +
                                                        staff_y * 4.0)));
      remaining -= 10;
      offset += 5.0;
    }
    if (remaining >= 5) {
      const wxPoint base(
          tip.x - static_cast<int>(std::lround(staff_x * (offset + 2.0))),
          tip.y - static_cast<int>(std::lround(staff_y * (offset + 2.0))));
      dc.DrawLine(base.x, base.y,
                  base.x + static_cast<int>(std::lround(perpendicular_x * 7.0 +
                                                        staff_x * 2.5)),
                  base.y + static_cast<int>(std::lround(perpendicular_y * 7.0 +
                                                        staff_y * 2.5)));
    }
  };

  auto draw_tidal_arrow = [&](const wxPoint& origin, double u, double v,
                              const wxColour& colour,
                              const LayerDisplaySettings& settings) {
    const double magnitude = std::hypot(u, v);
    if (magnitude < 0.01) return;
    const double knots = magnitude * 1.94384449;
    const double length =
        std::clamp(settings.proportional_base_size +
                       settings.proportional_growth_per_knot * knots,
                   6.0, 64.0);
    const double direction_x = u / magnitude;
    const double direction_y = -v / magnitude;
    const double perpendicular_x = -direction_y;
    const double perpendicular_y = direction_x;
    const double head_length = std::clamp(length * 0.34, 4.0, 13.0);
    const double head_half_width = std::clamp(length * 0.20, 3.0, 8.0);
    const wxPoint tail(
        origin.x - static_cast<int>(std::lround(direction_x * length * 0.42)),
        origin.y - static_cast<int>(std::lround(direction_y * length * 0.42)));
    const wxPoint tip(
        origin.x + static_cast<int>(std::lround(direction_x * length * 0.58)),
        origin.y + static_cast<int>(std::lround(direction_y * length * 0.58)));
    const double neck_x = tip.x - direction_x * head_length;
    const double neck_y = tip.y - direction_y * head_length;
    const wxPoint neck(static_cast<int>(std::lround(neck_x)),
                       static_cast<int>(std::lround(neck_y)));
    const wxPoint left(static_cast<int>(std::lround(
                           neck_x + perpendicular_x * head_half_width)),
                       static_cast<int>(std::lround(
                           neck_y + perpendicular_y * head_half_width)));
    const wxPoint right(static_cast<int>(std::lround(
                            neck_x - perpendicular_x * head_half_width)),
                        static_cast<int>(std::lround(
                            neck_y - perpendicular_y * head_half_width)));
    const int width =
        std::clamp<int>(static_cast<int>(std::lround(length / 12.0)), 1, 4);
    dc.SetPen(wxPen(colour, width, wxPENSTYLE_SOLID));
    dc.SetBrush(wxBrush(colour));
    dc.DrawLine(tail.x, tail.y, neck.x, neck.y);
    wxPoint head[3] = {tip, left, right};
    dc.DrawPolygon(3, head);
  };

  auto draw_vectors = [&](const wxString& u_name, const wxString& v_name,
                          const wxColour& colour, bool enabled,
                          const LayerDisplaySettings& settings,
                          bool meteorological) {
    if (!enabled) return;
    const auto u = fields.find(u_name);
    const auto v = fields.find(v_name);
    if (u == fields.end() || v == fields.end()) return;
    const size_t count = std::min(u->second.size(), v->second.size());
    dc.SetPen(wxPen(colour, 2, wxPENSTYLE_SOLID));
    std::set<std::pair<int, int>> occupied;
    for (size_t i = 0; i < count; ++i) {
      const auto& us = u->second[i];
      const auto& vs = v->second[i];
      if (std::abs(us.latitude - vs.latitude) > 0.001 ||
          std::abs(us.longitude - vs.longitude) > 0.001)
        continue;
      const double magnitude = std::hypot(us.value, vs.value);
      if (magnitude < 0.01) continue;
      const wxPoint origin = projection.GetPixFromLL(us.latitude, us.longitude);
      if (origin.x < 0 || origin.y < 0 || origin.x >= viewport.pix_width ||
          origin.y >= viewport.pix_height)
        continue;
      const auto cell = std::make_pair(origin.x / settings.vector_spacing,
                                       origin.y / settings.vector_spacing);
      if (!occupied.insert(cell).second) continue;
      if (meteorological && settings.vector_style == 0) {
        draw_wind_barb(origin, us.value, vs.value, us.latitude < 0.0, colour);
        rendered = true;
        continue;
      }
      if (!meteorological && settings.vector_style == 2) {
        draw_tidal_arrow(origin, us.value, vs.value, colour, settings);
        rendered = true;
        continue;
      }
      const double length =
          !meteorological ? 22.0
                          : std::clamp(11.0 + magnitude * 1.4, 12.0, 30.0);
      const double dx = us.value / magnitude * length;
      const double dy = -vs.value / magnitude * length;
      const wxPoint tip(origin.x + static_cast<int>(dx),
                        origin.y + static_cast<int>(dy));
      dc.DrawLine(origin.x, origin.y, tip.x, tip.y);
      dc.DrawLine(tip.x, tip.y, tip.x - static_cast<int>(0.35 * dx - 0.25 * dy),
                  tip.y - static_cast<int>(0.35 * dy + 0.25 * dx));
      dc.DrawLine(tip.x, tip.y, tip.x - static_cast<int>(0.35 * dx + 0.25 * dy),
                  tip.y - static_cast<int>(0.35 * dy - 0.25 * dx));
      if (!meteorological && settings.vector_style == 1) {
        const wxPoint second(tip.x - static_cast<int>(0.30 * dx),
                             tip.y - static_cast<int>(0.30 * dy));
        dc.DrawLine(second.x, second.y,
                    second.x - static_cast<int>(0.30 * dx - 0.22 * dy),
                    second.y - static_cast<int>(0.30 * dy + 0.22 * dx));
        dc.DrawLine(second.x, second.y,
                    second.x - static_cast<int>(0.30 * dx + 0.22 * dy),
                    second.y - static_cast<int>(0.30 * dy - 0.22 * dx));
      }
      rendered = true;
    }
  };
  auto draw_scalar = [&](const std::vector<Sample>& samples, bool enabled,
                         const wxColour& low, const wxColour& high) {
    if (!enabled || samples.empty()) return;
    const auto limits =
        std::minmax_element(samples.begin(), samples.end(),
                            [](const Sample& left, const Sample& right) {
                              return left.value < right.value;
                            });
    const double minimum = limits.first->value;
    const double span = std::max(1e-12, limits.second->value - minimum);
    dc.SetPen(wxPen(wxColour(40, 40, 40), 1));
    std::set<std::pair<int, int>> occupied;
    for (const auto& sample : samples) {
      const double fraction =
          std::clamp((sample.value - minimum) / span, 0.0, 1.0);
      const auto channel = [fraction](unsigned char low_channel,
                                      unsigned char high_channel) {
        return static_cast<unsigned char>(
            static_cast<double>(low_channel) +
            fraction * (static_cast<double>(high_channel) - low_channel));
      };
      dc.SetBrush(wxBrush(wxColour(
          channel(low.Red(), high.Red()), channel(low.Green(), high.Green()),
          channel(low.Blue(), high.Blue()), overlay_opacity)));
      const wxPoint point =
          projection.GetPixFromLL(sample.latitude, sample.longitude);
      if (point.x < 0 || point.y < 0 || point.x >= viewport.pix_width ||
          point.y >= viewport.pix_height)
        continue;
      const auto cell = std::make_pair(point.x / 18, point.y / 18);
      if (!occupied.insert(cell).second) continue;
      dc.DrawCircle(point, 10);
    }
    rendered = true;
  };

  auto field_samples = [&](const wxString& name,
                           double (*convert)(double) = nullptr) {
    std::vector<Sample> output;
    const auto found = fields.find(name);
    if (found == fields.end()) return output;
    output = found->second;
    if (convert)
      for (auto& sample : output) sample.value = convert(sample.value);
    return output;
  };
  const auto wind_speed = vector_samples("wind-u", "wind-v");
  const auto current_speed = vector_samples("current-u", "current-v");
  const auto pressure = field_samples("pressure", PressureHpa);
  const auto waves = field_samples("wave-height");
  const auto temperature = field_samples("air-temperature", TemperatureCelsius);
  auto pale = [](const wxColour& colour) {
    return wxColour(static_cast<unsigned char>(190 + colour.Red() * 65 / 255),
                    static_cast<unsigned char>(190 + colour.Green() * 65 / 255),
                    static_cast<unsigned char>(190 + colour.Blue() * 65 / 255));
  };
  draw_scalar(wind_speed,
              show_wind && show_wind->GetValue() && wind_display.overlay,
              pale(wind_display.colour), wind_display.colour);
  draw_scalar(
      pressure,
      show_pressure && show_pressure->GetValue() && pressure_display.overlay,
      pale(pressure_display.colour), pressure_display.colour);
  draw_scalar(waves,
              show_waves && show_waves->GetValue() && wave_display.overlay,
              pale(wave_display.colour), wave_display.colour);
  draw_scalar(
      current_speed,
      show_current && show_current->GetValue() && current_display.overlay,
      pale(current_display.colour), current_display.colour);
  draw_scalar(temperature,
              show_temperature && show_temperature->GetValue() &&
                  temperature_display.overlay,
              pale(temperature_display.colour), temperature_display.colour);

  auto convert_display = [](double value, const LayerDisplaySettings& settings,
                            int kind) {
    if (kind == 0 || kind == 3) {
      if (settings.units == 1) return value;
      if (settings.units == 2) return value * 2.23693629;
      if (settings.units == 3) return value * 3.6;
      return value * 1.94384449;
    }
    if (kind == 1) {
      if (settings.units == 1) return value * 0.750061683;
      if (settings.units == 2) return value * 0.0295299831;
      return value;
    }
    if (kind == 2) return settings.units == 1 ? value * 3.2808399 : value;
    return settings.units == 1 ? value * 9.0 / 5.0 + 32.0 : value;
  };
  auto draw_numbers = [&](const std::vector<Sample>& samples, bool enabled,
                          const LayerDisplaySettings& settings, int kind) {
    if (!enabled || samples.empty()) return;
    dc.SetTextForeground(settings.colour);
    dc.SetFont(*wxSMALL_FONT);
    std::set<std::pair<int, int>> occupied;
    for (const auto& sample : samples) {
      const wxPoint point =
          projection.GetPixFromLL(sample.latitude, sample.longitude);
      if (point.x < 0 || point.y < 0 || point.x >= viewport.pix_width ||
          point.y >= viewport.pix_height)
        continue;
      const auto cell = std::make_pair(point.x / settings.number_spacing,
                                       point.y / settings.number_spacing);
      if (!occupied.insert(cell).second) continue;
      const double value = convert_display(sample.value, settings, kind);
      const int precision = kind == 1 || kind == 4 ? 1 : 1;
      dc.DrawText(wxString::Format("%.*f", precision, value), point.x + 3,
                  point.y + 3);
      rendered = true;
    }
  };
  draw_numbers(wind_speed,
               show_wind && show_wind->GetValue() && wind_display.numbers,
               wind_display, 0);
  draw_numbers(
      pressure,
      show_pressure && show_pressure->GetValue() && pressure_display.numbers,
      pressure_display, 1);
  draw_numbers(waves,
               show_waves && show_waves->GetValue() && wave_display.numbers,
               wave_display, 2);
  draw_numbers(
      current_speed,
      show_current && show_current->GetValue() && current_display.numbers,
      current_display, 3);
  draw_numbers(temperature,
               show_temperature && show_temperature->GetValue() &&
                   temperature_display.numbers,
               temperature_display, 4);

  auto draw_contours = [&](const std::vector<Sample>& samples, bool enabled,
                           int interval, const wxColour& colour) {
    if (!enabled || samples.empty() || interval <= 0) return;
    struct CellValue {
      double total = 0.0;
      int count = 0;
    };
    constexpr int cell_size = 22;
    std::map<std::pair<int, int>, CellValue> cells;
    double minimum = std::numeric_limits<double>::max();
    double maximum = std::numeric_limits<double>::lowest();
    for (const auto& sample : samples) {
      const wxPoint point =
          projection.GetPixFromLL(sample.latitude, sample.longitude);
      if (point.x < -cell_size || point.y < -cell_size ||
          point.x > viewport.pix_width + cell_size ||
          point.y > viewport.pix_height + cell_size)
        continue;
      auto& cell = cells[{point.x / cell_size, point.y / cell_size}];
      cell.total += sample.value;
      ++cell.count;
      minimum = std::min(minimum, sample.value);
      maximum = std::max(maximum, sample.value);
    }
    if (cells.size() < 4 || minimum >= maximum) return;
    dc.SetPen(wxPen(colour, 2, wxPENSTYLE_SOLID));
    const double first = std::ceil(minimum / interval) * interval;
    for (double level = first; level <= maximum; level += interval) {
      bool labelled = false;
      for (int y = -1; y <= viewport.pix_height / cell_size; ++y) {
        for (int x = -1; x <= viewport.pix_width / cell_size; ++x) {
          const std::pair<int, int> keys[4] = {
              {x, y}, {x + 1, y}, {x + 1, y + 1}, {x, y + 1}};
          double values[4];
          bool complete = true;
          for (int corner = 0; corner < 4; ++corner) {
            const auto found = cells.find(keys[corner]);
            if (found == cells.end() || found->second.count == 0) {
              complete = false;
              break;
            }
            values[corner] = found->second.total / found->second.count;
          }
          if (!complete) continue;
          const wxPoint points[4] = {{x * cell_size, y * cell_size},
                                     {(x + 1) * cell_size, y * cell_size},
                                     {(x + 1) * cell_size, (y + 1) * cell_size},
                                     {x * cell_size, (y + 1) * cell_size}};
          std::vector<wxPoint> crossings;
          for (int edge = 0; edge < 4; ++edge) {
            const int next = (edge + 1) % 4;
            if ((values[edge] < level) == (values[next] < level) ||
                values[edge] == values[next])
              continue;
            const double fraction =
                (level - values[edge]) / (values[next] - values[edge]);
            crossings.emplace_back(
                points[edge].x +
                    static_cast<int>(std::lround(
                        fraction * (points[next].x - points[edge].x))),
                points[edge].y +
                    static_cast<int>(std::lround(
                        fraction * (points[next].y - points[edge].y))));
          }
          if (crossings.size() == 2 || crossings.size() == 4) {
            dc.DrawLine(crossings[0].x, crossings[0].y, crossings[1].x,
                        crossings[1].y);
            if (crossings.size() == 4)
              dc.DrawLine(crossings[2].x, crossings[2].y, crossings[3].x,
                          crossings[3].y);
            if (!labelled && x > 1 && y > 1) {
              dc.SetTextForeground(colour);
              dc.SetFont(*wxSMALL_FONT);
              dc.DrawText(wxString::Format("%.0f", level), crossings[0].x,
                          crossings[0].y);
              labelled = true;
            }
            rendered = true;
          }
        }
      }
    }
  };
  draw_contours(wind_speed,
                show_wind && show_wind->GetValue() && wind_display.contours,
                wind_display.contour_spacing, wind_display.colour);
  draw_contours(
      pressure,
      show_pressure && show_pressure->GetValue() && pressure_display.contours,
      pressure_display.contour_spacing, pressure_display.colour);
  draw_contours(temperature,
                show_temperature && show_temperature->GetValue() &&
                    temperature_display.contours,
                temperature_display.contour_spacing,
                temperature_display.colour);
  // Vectors are rendered last so translucent scalar maps cannot obscure them.
  draw_vectors("wind-u", "wind-v", wind_display.colour,
               show_wind && show_wind->GetValue() && wind_display.vectors,
               wind_display, true);
  draw_vectors(
      "current-u", "current-v", current_display.colour,
      show_current && show_current->GetValue() && current_display.vectors,
      current_display, false);
  return rendered;
}

void PortableGribHost::Impl::Shutdown() {
  if (stopped) return;
  stopped = true;
  progress_timer.Stop();
  playback_timer.Stop();
  if (process && process_id > 0)
    wxProcess::Kill(static_cast<int>(process_id), wxSIGKILL, wxKILL_CHILDREN);
  if (process) {
    process->Detach();
    process = nullptr;
  }
  if (frame) {
    frame->Destroy();
    frame = nullptr;
  }
  fields.clear();
  field_units.clear();
}

PortableGribHost::PortableGribHost(wxWindow* parent,
                                   const wxString& package_root,
                                   bool credential_access)
    : m_impl(std::make_unique<Impl>(parent, package_root, credential_access)) {}

PortableGribHost::~PortableGribHost() = default;

bool PortableGribHost::Show(wxString* error) { return m_impl->Show(error); }

bool PortableGribHost::Render(ocpnDC& dc, const ViewPort& viewport) {
  return m_impl->Render(dc, viewport);
}

void PortableGribHost::SetCursorPosition(double latitude, double longitude) {
  m_impl->SetCursorPosition(latitude, longitude);
}

void PortableGribHost::Shutdown() { m_impl->Shutdown(); }
