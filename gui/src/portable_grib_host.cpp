/***************************************************************************
 * Host-owned iGRIB environmental service implementation.
 ***************************************************************************/

#include "portable_grib_host.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/datetime.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/gauge.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/jsonwriter.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/process.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/wfstream.h>

#include "model/base_platform.h"
#include "ocpndc.h"
#include "top_frame.h"
#include "viewport.h"

namespace {

constexpr int kHelperProcessId = wxID_HIGHEST + 711;
constexpr int kProgressTimerId = wxID_HIGHEST + 712;
constexpr int kPlaybackTimerId = wxID_HIGHEST + 713;
constexpr size_t kMaximumResultBytes = 32U * 1024U * 1024U;

struct Sample {
  double latitude = 0.0;
  double longitude = 0.0;
  double value = 0.0;
};

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

  Impl(wxWindow* parent_value, wxString package_root_value)
      : parent(parent_value),
        package_root(std::move(package_root_value)),
        progress_timer(this, kProgressTimerId),
        playback_timer(this, kPlaybackTimerId) {
    Bind(wxEVT_END_PROCESS, &Impl::OnProcessEnded, this, kHelperProcessId);
    Bind(wxEVT_TIMER, &Impl::OnProgressTimer, this, kProgressTimerId);
    Bind(wxEVT_TIMER, &Impl::OnPlaybackTimer, this, kPlaybackTimerId);
  }

  ~Impl() override { Shutdown(); }

  bool Show(wxString* error);
  bool Render(ocpnDC& dc, const ViewPort& viewport);
  void Shutdown();

private:
  void CreateFrame();
  void OpenFile();
  void StartInspect(const wxString& path);
  void StartFrame(size_t index);
  void ShowGenerator();
  void Cancel();
  bool Launch(const std::vector<wxString>& arguments, Operation next_operation,
              const wxString& result, const wxString& status);
  std::vector<wxString> DecoderCommand(const wxString& verb,
                                       const wxString& input,
                                       const wxString& time,
                                       const wxString& result) const;
  std::vector<wxString> GeneratorCommand(const wxString& job,
                                         const wxString& result,
                                         const wxString& output,
                                         const wxString& input_cache,
                                         const wxString& model_directory) const;
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
  wxStaticText* data_status = nullptr;
  wxGauge* progress = nullptr;
  wxButton* cancel_button = nullptr;
  wxButton* open_button = nullptr;
  wxButton* generate_button = nullptr;
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
  frame = new wxFrame(parent, wxID_ANY, surface_title, wxDefaultPosition,
                      wxSize(930, 280),
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
  auto* play = new wxButton(frame, wxID_ANY, "Play");
  auto* now = new wxButton(frame, wxID_ANY, "Now");
  timeline_row->Add(previous, 0, wxRIGHT, 5);
  timeline_row->Add(timeline, 1, wxRIGHT, 5);
  timeline_row->Add(next, 0, wxRIGHT, 5);
  timeline_row->Add(play, 0, wxRIGHT, 5);
  timeline_row->Add(now, 0);
  controls->Add(timeline_row, 0, wxEXPAND | wxALL, 7);

  auto* data =
      new wxStaticBoxSizer(wxVERTICAL, frame, "Data at cursor position");
  auto* toggles = new wxBoxSizer(wxHORIZONTAL);
  show_wind = new wxCheckBox(frame, wxID_ANY, "Wind");
  show_pressure = new wxCheckBox(frame, wxID_ANY, "Pressure");
  show_waves = new wxCheckBox(frame, wxID_ANY, "Waves");
  show_current = new wxCheckBox(frame, wxID_ANY, "Current");
  show_temperature = new wxCheckBox(frame, wxID_ANY, "Air Temp");
  show_wind->SetValue(true);
  show_pressure->SetValue(true);
  show_waves->SetValue(true);
  show_current->SetValue(true);
  for (auto* toggle :
       {show_wind, show_pressure, show_waves, show_current, show_temperature})
    toggles->Add(toggle, 0, wxRIGHT, 14);
  data_status = new wxStaticText(frame, wxID_ANY,
                                 "Open a GRIB file to inspect its fields");
  data->Add(toggles, 0, wxEXPAND | wxALL, 5);
  data->Add(data_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
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
  settings->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxMessageBox(
        "iGRIB uses a host-rendered, accessible control surface. GRIB data is "
        "decoded in an isolated helper and overlays are retained by OpenCPN.",
        "iGRIB settings", wxOK | wxICON_INFORMATION, frame);
  });
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
  play->Bind(wxEVT_BUTTON, [this, play](wxCommandEvent&) {
    if (playback_timer.IsRunning()) {
      playback_timer.Stop();
      play->SetLabel("Play");
    } else if (!times.empty()) {
      playback_timer.Start(1200);
      play->SetLabel("Pause");
    }
  });
  timeline->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    const int selected = timeline->GetSelection();
    if (selected != wxNOT_FOUND) StartFrame(static_cast<size_t>(selected));
  });
  for (auto* toggle :
       {show_wind, show_pressure, show_waves, show_current, show_temperature})
    toggle->Bind(wxEVT_CHECKBOX, [](wxCommandEvent&) {
      if (top_frame::Get()) top_frame::Get()->RefreshAllCanvas(false);
    });
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
      command.push_back("1500");
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
    const wxString& input_cache, const wxString& model_directory) const {
#if defined(__linux__)
  if (HelperSupervisionAvailable()) {
    std::vector<wxString> command = {"/usr/bin/prlimit", "--as=4294967296",
                                     "--cpu=1800", "--"};
    const wxString output_directory = wxFileName(output).GetPath();
    const std::vector<wxString> sandbox = {"/usr/bin/bwrap",
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
    command.insert(command.end(), sandbox.begin(), sandbox.end());
    if (wxDirExists("/etc/ca-certificates")) {
      command.insert(command.end(), {"--ro-bind", "/etc/ca-certificates",
                                     "/etc/ca-certificates"});
    }
    if (!input_cache.empty()) {
      command.insert(command.end(), {"--ro-bind", input_cache, "/input-cache"});
    }
    if (!model_directory.empty()) {
      command.insert(command.end(),
                     {"--ro-bind", model_directory, "/tpxo-model"});
    }
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
  static_cast<void>(input_cache);
  static_cast<void>(model_directory);
  return {};
}

bool PortableGribHost::Impl::Launch(const std::vector<wxString>& arguments,
                                    Operation next_operation,
                                    const wxString& result,
                                    const wxString& status) {
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
  process_id =
      wxExecute(argv.data(), wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE, process);
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
    timeline->Append(times.back());
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
      value["time"].AsString(), value["sampleCount"].AsInt()));
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

  wxDialog dialog(frame, wxID_ANY, "Environmental GRIB Generator",
                  wxDefaultPosition, wxSize(780, 720),
                  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* root = new wxBoxSizer(wxVERTICAL);
  auto* grid = new wxFlexGridSizer(2, 6, 8);
  grid->AddGrowableCol(1, 1);
  auto* executable =
      new wxTextCtrl(&dialog, wxID_ANY, GeneratorHelper(), wxDefaultPosition,
                     wxDefaultSize, wxTE_READONLY);
  auto* west = new wxTextCtrl(&dialog, wxID_ANY, "-8.5");
  auto* south = new wxTextCtrl(&dialog, wxID_ANY, "50.5");
  auto* east = new wxTextCtrl(&dialog, wxID_ANY, "-2.5");
  auto* north = new wxTextCtrl(&dialog, wxID_ANY, "56.5");
  auto* start =
      new wxTextCtrl(&dialog, wxID_ANY,
                     wxDateTime::Now().ToUTC().Format("%Y-%m-%dT%H:00:00Z"));
  auto* hours = new wxSpinCtrl(&dialog, wxID_ANY, "72", wxDefaultPosition,
                               wxDefaultSize, wxSP_ARROW_KEYS, 1, 360, 72);
  auto* step = new wxSpinCtrl(&dialog, wxID_ANY, "3", wxDefaultPosition,
                              wxDefaultSize, wxSP_ARROW_KEYS, 1, 24, 3);
  wxArrayString weather_providers;
  weather_providers.Add("NOAA GFS forecast");
  weather_providers.Add("UK Met Office UKV");
  auto* provider = new wxChoice(&dialog, wxID_ANY, wxDefaultPosition,
                                wxDefaultSize, weather_providers);
  provider->SetSelection(0);
  wxArrayString presets;
  presets.Add("Routing");
  presets.Add("Viewer");
  auto* preset = new wxChoice(&dialog, wxID_ANY, wxDefaultPosition,
                              wxDefaultSize, presets);
  preset->SetSelection(0);
  auto* waves = new wxCheckBox(&dialog, wxID_ANY, "Include wave fields");
  auto* currents =
      new wxCheckBox(&dialog, wxID_ANY, "Generate/include currents");
  auto* tpxo_directory = new wxTextCtrl(&dialog, wxID_ANY, wxEmptyString);
  auto* tpxo_cache = new wxTextCtrl(&dialog, wxID_ANY, wxEmptyString);
  AddRow(grid, &dialog, "Generator executable", executable);
  AddRow(grid, &dialog, "West longitude", west);
  AddRow(grid, &dialog, "South latitude", south);
  AddRow(grid, &dialog, "East longitude", east);
  AddRow(grid, &dialog, "North latitude", north);
  AddRow(grid, &dialog, "Start UTC", start);
  AddRow(grid, &dialog, "Forecast duration hours (maximum 15 days)", hours);
  AddRow(grid, &dialog, "Step hours", step);
  AddRow(grid, &dialog, "Weather provider", provider);
  AddRow(grid, &dialog, "Weather preset", preset);
  AddRow(grid, &dialog, "Waves", waves);
  AddRow(grid, &dialog, "Currents", currents);
  AddRow(grid, &dialog, "TPXO model directory", tpxo_directory);
  AddRow(grid, &dialog, "TPXO cache file", tpxo_cache);
  root->Add(grid, 1, wxEXPAND | wxALL, 10);
  root->Add(new wxStaticText(
                &dialog, wxID_ANY,
                "Generated environmental data is model output for planning "
                "and experimentation; it is not an official navigation "
                "product."),
            0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
  auto* buttons = dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL);
  root->Add(buttons, 0, wxEXPAND | wxALL, 10);
  dialog.SetSizer(root);
  if (dialog.ShowModal() != wxID_OK) return;

  wxFileDialog output_dialog(frame, "Save generated environmental GRIB",
                             wxEmptyString, "environment_igrib.grb",
                             "GRIB files (*.grb)|*.grb|All files (*.*)|*.*",
                             wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
  if (output_dialog.ShowModal() != wxID_OK) return;

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
      wxString(provider->GetSelection() == 1 ? "ukmo_ukv" : "gfs");
  request["weatherPreset"] =
      wxString(preset->GetSelection() == 1 ? "viewer" : "routing");
  request["includeWaves"] = waves->GetValue();
  request["waveProvider"] = wxString("gfs_wave");
  request["currentSource"] =
      wxString(currents->GetValue() ? "tpxo-cache" : "none");
  const bool sandboxed_generator = HelperSupervisionAvailable();
  if (!tpxo_directory->GetValue().empty())
    request["tpxoModelDirectory"] =
        sandboxed_generator ? "/tpxo-model" : tpxo_directory->GetValue();
  if (!tpxo_cache->GetValue().empty())
    request["inputCache"] =
        sandboxed_generator ? "/input-cache" : tpxo_cache->GetValue();
  request["autoPrepareTpxoCache"] = currents->GetValue();
  request["output"] = sandboxed_generator
                          ? "/output/" + output_dialog.GetFilename()
                          : output_dialog.GetPath();
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
  generated_output_path = output_dialog.GetPath();
  Launch(GeneratorCommand(job_path, result, generated_output_path,
                          tpxo_cache->GetValue(), tpxo_directory->GetValue()),
         Operation::Generate, result,
         "Generating environmental GRIB in supervised helper…");
}

bool PortableGribHost::Impl::Render(ocpnDC& dc, const ViewPort& viewport) {
  if (fields.empty()) return false;
  bool rendered = false;
  ViewPort projection = viewport;
  auto draw_vectors = [&](const wxString& u_name, const wxString& v_name,
                          const wxColour& colour, bool enabled) {
    if (!enabled) return;
    const auto u = fields.find(u_name);
    const auto v = fields.find(v_name);
    if (u == fields.end() || v == fields.end()) return;
    const size_t count = std::min(u->second.size(), v->second.size());
    dc.SetPen(wxPen(colour, 2, wxPENSTYLE_SOLID));
    for (size_t i = 0; i < count; ++i) {
      const auto& us = u->second[i];
      const auto& vs = v->second[i];
      if (std::abs(us.latitude - vs.latitude) > 0.001 ||
          std::abs(us.longitude - vs.longitude) > 0.001)
        continue;
      const double magnitude = std::hypot(us.value, vs.value);
      if (magnitude < 0.01) continue;
      const double length = std::clamp(7.0 + magnitude * 0.7, 8.0, 28.0);
      const wxPoint origin = projection.GetPixFromLL(us.latitude, us.longitude);
      const double dx = us.value / magnitude * length;
      const double dy = -vs.value / magnitude * length;
      const wxPoint tip(origin.x + static_cast<int>(dx),
                        origin.y + static_cast<int>(dy));
      dc.DrawLine(origin.x, origin.y, tip.x, tip.y);
      dc.DrawLine(tip.x, tip.y, tip.x - static_cast<int>(0.35 * dx - 0.25 * dy),
                  tip.y - static_cast<int>(0.35 * dy + 0.25 * dx));
      dc.DrawLine(tip.x, tip.y, tip.x - static_cast<int>(0.35 * dx + 0.25 * dy),
                  tip.y - static_cast<int>(0.35 * dy - 0.25 * dx));
    }
    rendered = count != 0 || rendered;
  };
  draw_vectors("wind-u", "wind-v", *wxBLACK,
               show_wind && show_wind->GetValue());
  draw_vectors("current-u", "current-v", wxColour(150, 90, 15),
               show_current && show_current->GetValue());

  auto draw_scalar = [&](const wxString& name, bool enabled,
                         const wxColour& low, const wxColour& high) {
    if (!enabled) return;
    const auto found = fields.find(name);
    if (found == fields.end() || found->second.empty()) return;
    const auto& samples = found->second;
    const auto limits =
        std::minmax_element(samples.begin(), samples.end(),
                            [](const Sample& left, const Sample& right) {
                              return left.value < right.value;
                            });
    const double minimum = limits.first->value;
    const double span = std::max(1e-12, limits.second->value - minimum);
    const size_t stride = std::max<size_t>(1, samples.size() / 180);
    dc.SetPen(wxPen(wxColour(40, 40, 40), 1));
    for (size_t i = 0; i < samples.size(); i += stride) {
      const auto& sample = samples[i];
      const double fraction =
          std::clamp((sample.value - minimum) / span, 0.0, 1.0);
      const auto channel = [fraction](unsigned char low_channel,
                                      unsigned char high_channel) {
        return static_cast<unsigned char>(
            static_cast<double>(low_channel) +
            fraction * (static_cast<double>(high_channel) - low_channel));
      };
      dc.SetBrush(wxBrush(wxColour(channel(low.Red(), high.Red()),
                                   channel(low.Green(), high.Green()),
                                   channel(low.Blue(), high.Blue()), 150)));
      const wxPoint point =
          projection.GetPixFromLL(sample.latitude, sample.longitude);
      dc.DrawCircle(point, 4);
    }
    rendered = true;
  };
  draw_scalar("pressure", show_pressure && show_pressure->GetValue(),
              wxColour(70, 120, 255), wxColour(220, 50, 50));
  draw_scalar("wave-height", show_waves && show_waves->GetValue(),
              wxColour(100, 220, 255), wxColour(20, 30, 170));
  draw_scalar("air-temperature",
              show_temperature && show_temperature->GetValue(),
              wxColour(30, 80, 220), wxColour(250, 80, 30));
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
                                   const wxString& package_root)
    : m_impl(std::make_unique<Impl>(parent, package_root)) {}

PortableGribHost::~PortableGribHost() = default;

bool PortableGribHost::Show(wxString* error) { return m_impl->Show(error); }

bool PortableGribHost::Render(ocpnDC& dc, const ViewPort& viewport) {
  return m_impl->Render(dc, viewport);
}

void PortableGribHost::Shutdown() { m_impl->Shutdown(); }
