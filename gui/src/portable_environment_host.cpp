/***************************************************************************
 * Generic host-owned environmental service implementation.
 ***************************************************************************/

#include "portable_environment_host.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <wx/button.h>
#ifdef ocpnUSE_wxBitmapBundle
#include <wx/bmpbndl.h>
#endif
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/collpane.h>
#include <wx/clrpicker.h>
#include <wx/datetime.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/gauge.h>
#include <wx/hyperlink.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/jsonwriter.h>
#include <wx/listctrl.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/process.h>
#include <wx/scrolwin.h>
#include <wx/secretstore.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/wfstream.h>

#include "model/base_platform.h"
#include "chcanv.h"
#include "navutil.h"
#include "ocpn_portable_runtime.h"
#include "ocpndc.h"
#include "picosha2.h"
#include "shapefile_basemap.h"
#include "top_frame.h"
#include "viewport.h"

namespace {

constexpr int kHelperProcessId = wxID_HIGHEST + 711;
constexpr int kProgressTimerId = wxID_HIGHEST + 712;
constexpr int kPlaybackTimerId = wxID_HIGHEST + 713;
constexpr int kAnimationTimerId = wxID_HIGHEST + 714;
constexpr size_t kMaximumResultBytes = 32U * 1024U * 1024U;
constexpr double kPi = 3.14159265358979323846;
// This is a deliberately conservative physical ceiling, not a display clamp.
// Values at or above common GRIB/NetCDF fill sentinels (for example 9999)
// must be discarded rather than rendered or supplied to route calculations.
constexpr double kMaximumCurrentMetresPerSecond = 12.0;

wxString OptionalJsonString(wxJSONValue object, const wxString& key) {
  if (!object.IsObject() || !object.HasMember(key) || !object[key].IsString())
    return {};
  return object[key].AsString();
}

bool OptionalJsonBool(wxJSONValue object, const wxString& key) {
  return object.IsObject() && object.HasMember(key) && object[key].IsBool() &&
         object[key].AsBool();
}

bool IsSafeSurfaceResource(const wxString& value);

struct Sample {
  double latitude = 0.0;
  double longitude = 0.0;
  double value = 0.0;
};

struct DecodedEnvironmentFrame {
  std::map<wxString, std::vector<Sample>> fields;
  std::map<wxString, wxString> units;
  std::map<wxString, wxString> source_times;
};

size_t EstimatedFrameBytes(const DecodedEnvironmentFrame& frame) {
  size_t bytes = sizeof(frame);
  for (const auto& [name, samples] : frame.fields)
    bytes += name.length() * sizeof(wxChar) +
             samples.capacity() * sizeof(Sample);
  for (const auto& [name, value] : frame.units)
    bytes += (name.length() + value.length()) * sizeof(wxChar);
  for (const auto& [name, value] : frame.source_times)
    bytes += (name.length() + value.length()) * sizeof(wxChar);
  return bytes;
}

bool PlausibleDecodedValue(const wxString& kind, double value) {
  if (!std::isfinite(value)) return false;
  if (kind == "current-u" || kind == "current-v")
    return std::abs(value) < kMaximumCurrentMetresPerSecond;
  if (kind == "wind-u" || kind == "wind-v") return std::abs(value) <= 200.0;
  if (kind == "wave-height") return value >= 0.0 && value <= 100.0;
  if (kind == "wave-period") return value >= 0.0 && value <= 100.0;
  if (kind == "wave-direction") return value >= 0.0 && value <= 360.0;
  return true;
}

bool PlausibleCurrent(double u, double v) {
  return std::isfinite(u) && std::isfinite(v) &&
         std::hypot(u, v) < kMaximumCurrentMetresPerSecond;
}

bool ParseDecodedFrame(wxJSONValue& value, DecodedEnvironmentFrame* decoded,
                       wxString* error) {
  if (!decoded || !value["fields"].IsArray()) {
    if (error) *error = "decoder returned an incompatible frame schema";
    return false;
  }
  for (int i = 0; i < value["fields"].Size(); ++i) {
    wxJSONValue field = value["fields"][i];
    if (!field.IsObject() || !field["kind"].IsString() ||
        !field["samples"].IsArray())
      continue;
    const wxString kind = field["kind"].AsString();
    auto& output = decoded->fields[kind];
    decoded->units[kind] = field["unit"].AsString();
    if (field["sourceTime"].IsString())
      decoded->source_times[kind] = field["sourceTime"].AsString();
    output.reserve(field["samples"].Size());
    for (int j = 0; j < field["samples"].Size(); ++j) {
      wxJSONValue sample = field["samples"][j];
      if (!sample.IsArray() || sample.Size() != 3) continue;
      const double latitude = sample[0].AsDouble();
      const double longitude = sample[1].AsDouble();
      const double sample_value = sample[2].AsDouble();
      if (std::isfinite(latitude) && latitude >= -90.0 && latitude <= 90.0 &&
          std::isfinite(longitude) && longitude >= -180.0 &&
          longitude <= 180.0 && PlausibleDecodedValue(kind, sample_value))
        output.push_back({latitude, longitude, sample_value});
    }
  }
  if (decoded->fields.empty()) {
    if (error) *error = "decoded frame contains no supported fields";
    return false;
  }
  return true;
}

struct LayerDisplaySettings {
  int units = 0;
  bool vectors = false;
  bool overlay = false;
  bool numbers = false;
  bool contours = false;
  int vector_spacing = 44;
  int number_spacing = 70;
  double contour_spacing = 4.0;
  int vector_style = 0;
  wxColour colour = *wxBLACK;
  int proportional_base_size = 12;
  double proportional_growth_per_knot = 6.0;
  bool vector_fixed_spacing = false;
  bool number_fixed_spacing = false;
  bool contour_labels = true;
  bool contour_labels_abbreviated = false;
  int overlay_palette = 0;
  bool particles = false;
  int particle_density = 2;
};

struct EnvironmentalFieldGroup {
  wxString id;
  wxString label;
  wxString kind;
  std::vector<wxString> components;
  std::set<wxString> presentations;
  bool levels = false;
  bool marine = false;
  bool default_visible = false;
  LayerDisplaySettings display;
  wxCheckBox* visible = nullptr;
  wxStaticText* value = nullptr;
};

wxString ConfigurationName(const wxString& id) {
  wxString result;
  bool upper = true;
  for (const auto character : id) {
    const wxChar value = static_cast<wxChar>(character.GetValue());
    if (value == '-' || value == '_' || value == ' ') {
      upper = true;
      continue;
    }
    result += upper ? static_cast<wxChar>(wxToupper(value)) : value;
    upper = false;
  }
  return result;
}

bool HasPresentation(const EnvironmentalFieldGroup& group,
                     const wxString& presentation) {
  return group.presentations.count(presentation) != 0;
}

bool IsSchemaIdentifier(const wxString& value) {
  if (value.empty() || value.length() > 96) return false;
  return std::all_of(value.begin(), value.end(), [](const wxUniChar& item) {
    const auto code = item.GetValue();
    return (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z') ||
           (code >= '0' && code <= '9') || code == '-' || code == '_' ||
           code == '.';
  });
}

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

wxColour EnvironmentalPaletteColour(int palette, double fraction,
                                    const wxColour& selected, bool gradual) {
  fraction = std::clamp(fraction, 0.0, 1.0);
  if (!gradual) fraction = std::floor(fraction * 8.0) / 8.0;
  struct Stop {
    double position;
    unsigned char red;
    unsigned char green;
    unsigned char blue;
  };
  const std::vector<Stop> selected_stops = {
      {0.0, static_cast<unsigned char>(190 + selected.Red() * 65 / 255),
       static_cast<unsigned char>(190 + selected.Green() * 65 / 255),
       static_cast<unsigned char>(190 + selected.Blue() * 65 / 255)},
      {1.0, selected.Red(), selected.Green(), selected.Blue()}};
  static const std::vector<std::vector<Stop>> palettes = {
      {},
      {{0.0, 15, 55, 180}, {0.5, 0, 210, 230}, {1.0, 220, 255, 255}},
      {{0.0, 25, 130, 70}, {0.5, 250, 220, 40}, {1.0, 210, 30, 25}},
      {{0.0, 35, 75, 210}, {0.5, 245, 245, 245}, {1.0, 205, 30, 45}},
      {{0.0, 68, 1, 84},
       {0.33, 49, 104, 142},
       {0.66, 53, 183, 121},
       {1.0, 253, 231, 37}},
      {{0.0, 245, 245, 245}, {1.0, 65, 65, 75}},
      {{0.0, 215, 245, 255}, {0.45, 25, 130, 220}, {1.0, 120, 20, 160}},
      {{0.0, 40, 90, 205}, {0.5, 245, 245, 235}, {1.0, 220, 55, 30}}};
  const auto& stops =
      palette <= 0 || palette >= static_cast<int>(palettes.size())
          ? selected_stops
          : palettes[static_cast<size_t>(palette)];
  auto upper = std::upper_bound(
      stops.begin(), stops.end(), fraction,
      [](double value, const Stop& stop) { return value < stop.position; });
  if (upper == stops.begin())
    return wxColour(upper->red, upper->green, upper->blue);
  if (upper == stops.end()) {
    const auto& stop = stops.back();
    return wxColour(stop.red, stop.green, stop.blue);
  }
  const auto& left = *(upper - 1);
  const auto& right = *upper;
  const double span = std::max(1e-12, right.position - left.position);
  const double local = (fraction - left.position) / span;
  const auto channel = [local](unsigned char low, unsigned char high) {
    return static_cast<unsigned char>(std::lround(
        static_cast<double>(low) + local * (static_cast<double>(high) - low)));
  };
  return wxColour(channel(left.red, right.red),
                  channel(left.green, right.green),
                  channel(left.blue, right.blue));
}

bool IsFlatpakRuntime() {
#if defined(__linux__)
  wxString application_id;
  return wxGetEnv("FLATPAK_ID", &application_id) && !application_id.empty();
#else
  return false;
#endif
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
  return IsFlatpakRuntime() ? "flatpak-aarch64" : "linux-gnu-aarch64";
#else
  return IsFlatpakRuntime() ? "flatpak-x86_64" : "linux-gnu-x86_64";
#endif
}

bool HelperSupervisionAvailable(wxString* error = nullptr) {
#if defined(__linux__)
  if (IsFlatpakRuntime()) {
    if (wxFileExists("/usr/bin/prlimit")) return true;
    if (error)
      *error = "Flatpak helper execution requires prlimit inside the runtime";
    return false;
  }
  if (wxFileExists("/usr/bin/bwrap") && wxFileExists("/usr/bin/prlimit"))
    return true;
  if (error)
    *error = "native helper execution requires bubblewrap and prlimit on Linux";
#elif defined(__APPLE__)
  if (wxFileExists("/usr/bin/sandbox-exec")) return true;
  if (error)
    *error =
        "native helper execution requires sandbox-exec on this macOS "
        "prototype host";
#elif defined(_WIN32)
  // Decoder calls use a one-process Windows Job Object with memory, CPU and
  // lifetime limits. GUI-launched generator calls remain separate signed
  // processes and are forcibly terminated with the host service.
  return true;
#else
  if (error)
    *error = "native helper supervision is not implemented for this host";
#endif
  return false;
}

bool UsesFilesystemSandbox() {
#if defined(__linux__)
  // A Flatpak host is already confined by the application sandbox. It uses
  // explicitly selected document/private paths directly and adds rlimits,
  // rather than attempting an unavailable nested user namespace.
  return !IsFlatpakRuntime() && HelperSupervisionAvailable();
#else
  // macOS sandbox-exec restricts the real paths but does not create the
  // Linux bubblewrap namespace used by the portable job document.
  return false;
#endif
}

#if defined(__APPLE__)
wxString SandboxLiteral(const wxString& value) {
  wxString escaped = value;
  escaped.Replace("\\", "\\\\");
  escaped.Replace("\"", "\\\"");
  return "\"" + escaped + "\"";
}

wxString MacSandboxProfile(const wxString& helper,
                           const std::vector<wxString>& inputs,
                           const wxString& private_directory,
                           const wxString& output_directory, bool network) {
  wxString profile =
      "(version 1)(deny default)(allow process-exec)(allow process-fork)"
      "(allow signal (target self))(allow sysctl-read)"
      "(allow file-read-metadata)"
      "(allow file-read* (subpath \"/System\") (subpath \"/usr/lib\") "
      "(subpath \"/usr/share\") (subpath \"/opt/homebrew\") "
      "(subpath \"/usr/local\") (literal " +
      SandboxLiteral(helper) + ")";
  for (const auto& input : inputs)
    if (!input.empty()) profile += " (literal " + SandboxLiteral(input) + ")";
  if (!private_directory.empty())
    profile += " (subpath " + SandboxLiteral(private_directory) + ")";
  profile += ")";
  if (!private_directory.empty())
    profile += "(allow file-write* (subpath " +
               SandboxLiteral(private_directory) + "))";
  if (!output_directory.empty())
    profile += "(allow file-write* (subpath " +
               SandboxLiteral(output_directory) + "))";
  if (network) profile += "(allow network-outbound)(allow system-socket)";
  return profile;
}
#endif

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

bool SnapshotIdentity(const wxString& path, uint64_t* byte_size,
                      wxString* sha256, wxString* error) {
  if (!byte_size || !sha256 || !wxFileExists(path)) {
    if (error) *error = "environmental dataset snapshot is unavailable";
    return false;
  }
  std::ifstream input(path.ToStdString(), std::ios::binary);
  if (!input) {
    if (error) *error = "environmental dataset snapshot cannot be read";
    return false;
  }
  picosha2::hash256_one_by_one hasher;
  uint64_t size = 0;
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      hasher.process(buffer.data(), buffer.data() + count);
      size += static_cast<uint64_t>(count);
    }
  }
  if (!input.eof()) {
    if (error) *error = "environmental dataset snapshot read failed";
    return false;
  }
  hasher.finish();
  *byte_size = size;
  *sha256 = wxString::FromUTF8(picosha2::get_hash_hex_string(hasher));
  return !sha256->empty();
}

bool LoadDeclarativeSurface(const wxString& path, wxJSONValue* output,
                            wxString* error) {
  wxJSONValue definition;
  if (!ReadJson(path, &definition, error)) return false;
  const int schema = definition["schema_version"].AsInt();
  if (!definition["schema_version"].IsInt() || (schema != 1 && schema != 2) ||
      !definition["surface_id"].IsString() ||
      definition["surface_id"].AsString().empty() ||
      definition["kind"].AsString() != "floating-panel" ||
      !definition["title"].IsString() || !definition["controls"].IsArray() ||
      definition["controls"].Size() > 128) {
    *error = "declarative UI definition is incompatible with schemas 1-2";
    return false;
  }
  static const std::set<wxString> allowed_types = {
      "status",    "choice",    "button",
      "toggle",    "file-open", "file-open-multiple",
      "file-save", "cancel",    "progress",
      "slider",    "spin",      "number",
      "text",      "secret",    "colour",
      "table",     "panel",     "tabs"};
  std::set<wxString> identifiers;
  for (int i = 0; i < definition["controls"].Size(); ++i) {
    wxJSONValue control = definition["controls"][i];
    const wxString id = control["id"].AsString();
    const wxString type = control["type"].AsString();
    if (!control.IsObject() || !IsSchemaIdentifier(id) ||
        !control["label"].IsString() || !allowed_types.count(type) ||
        !identifiers.insert(id).second) {
      *error = "declarative UI contains an invalid or duplicate control";
      return false;
    }
    for (const auto* member : {"icon_resource", "alternate_icon_resource"}) {
      if (control.HasMember(member) &&
          (!control[member].IsString() ||
           !IsSafeSurfaceResource(control[member].AsString()))) {
        *error = "declarative UI contains an unsafe control resource";
        return false;
      }
    }
  }
  if (!identifiers.count("timeline") || !identifiers.count("open")) {
    *error = "environmental surface omits required timeline/open controls";
    return false;
  }
  if (schema == 2) {
    if (definition["role"].AsString() != "environmental-data-viewer" ||
        !definition["field_groups"].IsArray() ||
        definition["field_groups"].Size() == 0 ||
        definition["field_groups"].Size() > 64 ||
        !definition["provider_catalog"].IsObject()) {
      *error = "environmental declarative surface is missing schema-2 data";
      return false;
    }
    auto helpers = definition["helpers"];
    const auto valid_executable_name = [](const wxString& name) {
      return !name.empty() && name != "." && name != ".." &&
             name.Find('/') == wxNOT_FOUND && name.Find('\\') == wxNOT_FOUND;
    };
    if (!helpers.IsObject() || !helpers["decoder_executable"].IsString() ||
        !valid_executable_name(helpers["decoder_executable"].AsString()) ||
        !helpers["generator_executable"].IsString() ||
        !valid_executable_name(helpers["generator_executable"].AsString())) {
      *error = "environmental surface has no typed helper declarations";
      return false;
    }
    auto generator = definition["generator"];
    if (!generator.IsObject() || !generator["title"].IsString() ||
        !generator["output_basename"].IsString() ||
        !IsSchemaIdentifier(generator["output_basename"].AsString()) ||
        !generator["maximum_hours"].IsInt() ||
        generator["maximum_hours"].AsInt() < 1 ||
        generator["maximum_hours"].AsInt() > 24 * 31 ||
        !generator["safety_notice"].IsString()) {
      *error = "environmental surface has an invalid generator declaration";
      return false;
    }
    auto area_presets = generator["area_presets"];
    if (!area_presets.IsArray() || area_presets.Size() == 0 ||
        area_presets.Size() > 32) {
      *error = "environmental surface has an invalid area-preset catalogue";
      return false;
    }
    std::set<wxString> area_ids;
    for (int index = 0; index < area_presets.Size(); ++index) {
      auto preset = area_presets[index];
      const wxString id = preset["id"].AsString();
      const wxString kind = preset["kind"].AsString();
      if (!preset.IsObject() || !IsSchemaIdentifier(id) ||
          !area_ids.insert(id).second || !preset["label"].IsString() ||
          (kind != "custom" && kind != "current-view" && kind != "bbox")) {
        *error = "environmental surface has an invalid area preset";
        return false;
      }
      if (kind == "bbox") {
        const auto is_number = [](const wxJSONValue& value) {
          return value.IsDouble() || value.IsInt();
        };
        if (!is_number(preset["west"]) || !is_number(preset["south"]) ||
            !is_number(preset["east"]) || !is_number(preset["north"]) ||
            preset["west"].AsDouble() >= preset["east"].AsDouble() ||
            preset["south"].AsDouble() >= preset["north"].AsDouble()) {
          *error = "environmental surface has an invalid area-preset bbox";
          return false;
        }
      }
    }
    auto weather_presets = generator["weather_presets"];
    if (!weather_presets.IsArray() || weather_presets.Size() == 0 ||
        weather_presets.Size() > 16) {
      *error = "environmental surface has an invalid weather-preset catalogue";
      return false;
    }
    std::set<wxString> weather_preset_ids;
    int weather_preset_defaults = 0;
    for (int index = 0; index < weather_presets.Size(); ++index) {
      auto preset = weather_presets[index];
      const wxString id = preset["id"].AsString();
      if (!preset.IsObject() || !IsSchemaIdentifier(id) ||
          !weather_preset_ids.insert(id).second ||
          !preset["label"].IsString()) {
        *error = "environmental surface has an invalid weather preset";
        return false;
      }
      weather_preset_defaults += OptionalJsonBool(preset, "default") ? 1 : 0;
    }
    if (weather_preset_defaults > 1) {
      *error = "environmental surface has multiple default weather presets";
      return false;
    }
    std::set<wxString> credential_schemes;
    auto credentials = definition["credential_catalog"];
    if (!credentials.IsObject()) {
      *error = "environmental surface has no credential catalogue";
      return false;
    }
    for (const auto& scheme : credentials.GetMemberNames()) {
      auto credential = credentials[scheme];
      if (!IsSchemaIdentifier(scheme) || !credential.IsObject() ||
          !credential["label"].IsString() ||
          !credential["account_label"].IsString() ||
          !credential["account_url"].IsString() ||
          !credential["username_label"].IsString() ||
          !credential["secret_label"].IsString() ||
          !credential["service_suffix"].IsString() ||
          !IsSchemaIdentifier(credential["request_username_key"].AsString()) ||
          !IsSchemaIdentifier(credential["job_environment_key"].AsString())) {
        *error = "environmental surface has an invalid credential declaration";
        return false;
      }
      credential_schemes.insert(scheme);
    }
    std::set<wxString> groups;
    for (int i = 0; i < definition["field_groups"].Size(); ++i) {
      auto field = definition["field_groups"][i];
      const wxString id = field["id"].AsString();
      if (!field.IsObject() || !IsSchemaIdentifier(id) ||
          !field["label"].IsString() || !field["kind"].IsString() ||
          !field["presentations"].IsArray() || !groups.insert(id).second) {
        *error = "environmental surface has an invalid field group";
        return false;
      }
    }
    if (definition["layout"]["primary_field_rows"].IsArray()) {
      std::set<wxString> primary_groups;
      auto rows = definition["layout"]["primary_field_rows"];
      if (rows.Size() == 0 || rows.Size() > 8) {
        *error = "environmental surface has an invalid primary field layout";
        return false;
      }
      for (int row = 0; row < rows.Size(); ++row) {
        if (!rows[row].IsArray() || rows[row].Size() == 0 ||
            rows[row].Size() > 6) {
          *error = "environmental surface has an invalid primary field row";
          return false;
        }
        for (int item = 0; item < rows[row].Size(); ++item) {
          const wxString id = rows[row][item].AsString();
          if (!rows[row][item].IsString() || !groups.count(id) ||
              !primary_groups.insert(id).second) {
            *error = "environmental surface primary field layout is invalid";
            return false;
          }
        }
      }
    }
    for (const auto* category : {"weather", "waves", "current"}) {
      auto providers = definition["provider_catalog"][category];
      if (!providers.IsArray() || providers.Size() == 0 ||
          providers.Size() > 64) {
        *error = wxString::Format(
            "environmental provider category %s is invalid", category);
        return false;
      }
      std::set<wxString> ids;
      int defaults = 0;
      for (int i = 0; i < providers.Size(); ++i) {
        auto provider = providers[i];
        const wxString id = provider["id"].AsString();
        if (!provider.IsObject() || !IsSchemaIdentifier(id) ||
            !provider["label"].IsString() || !provider["status"].IsString() ||
            !ids.insert(id).second) {
          *error = wxString::Format(
              "environmental provider category %s has an invalid entry",
              category);
          return false;
        }
        defaults += OptionalJsonBool(provider, "default") ? 1 : 0;
        const wxString credential = provider["credential"].AsString();
        if (!provider["credential"].IsString() ||
            (credential != "none" && credential != "file" &&
             credential != "directory" && credential != "conditional" &&
             !credential_schemes.count(credential))) {
          *error = wxString::Format(
              "environmental provider %s has an unknown credential scheme", id);
          return false;
        }
        const wxString input_kind = OptionalJsonString(provider, "input_kind");
        if (!input_kind.empty() && input_kind != "file" &&
            input_kind != "directory") {
          *error = wxString::Format(
              "environmental provider %s has an invalid input kind", id);
          return false;
        }
        if (!input_kind.empty() &&
            (!provider["input_label"].IsString() ||
             !IsSchemaIdentifier(provider["input_request_key"].AsString()))) {
          *error = wxString::Format(
              "environmental provider %s has an incomplete input capability",
              id);
          return false;
        }
        if (provider["mode_options"].IsArray()) {
          if (!IsSchemaIdentifier(provider["mode_request_key"].AsString())) {
            *error = wxString::Format(
                "environmental provider %s has no mode request key", id);
            return false;
          }
          for (int mode_index = 0; mode_index < provider["mode_options"].Size();
               ++mode_index) {
            auto mode = provider["mode_options"][mode_index];
            if (!mode.IsObject() || !mode["label"].IsString() ||
                !IsSchemaIdentifier(mode["value"].AsString())) {
              *error = wxString::Format(
                  "environmental provider %s has an invalid mode option", id);
              return false;
            }
          }
        }
      }
      if (defaults > 1) {
        *error = wxString::Format(
            "environmental provider category %s has multiple defaults",
            category);
        return false;
      }
    }
  }
  if (output) *output = definition;
  return true;
}

wxString SurfaceControlLabel(const wxJSONValue& definition,
                             const wxString& identifier,
                             const wxString& fallback) {
  wxJSONValue copy = definition;
  auto controls = copy["controls"];
  for (int i = 0; i < controls.Size(); ++i) {
    if (controls[i]["id"].AsString() == identifier)
      return controls[i]["label"].AsString();
  }
  return fallback;
}

wxJSONValue SurfaceControlDefinition(const wxJSONValue& definition,
                                     const wxString& identifier) {
  wxJSONValue copy = definition;
  auto controls = copy["controls"];
  for (int i = 0; i < controls.Size(); ++i)
    if (controls[i]["id"].AsString() == identifier) return controls[i];
  return {};
}

bool IsSafeSurfaceResource(const wxString& value) {
  if (value.empty()) return false;
  wxFileName path(value);
  if (path.IsAbsolute()) return false;
  for (const auto& directory : path.GetDirs())
    if (directory == "." || directory == "..") return false;
  return value.Find('\\') == wxNOT_FOUND;
}

bool ApplySurfaceButtonPresentation(wxButton* button,
                                    const wxJSONValue& definition,
                                    const wxString& identifier,
                                    const wxString& package_root,
                                    bool alternate = false) {
  if (!button) return false;
  auto control = SurfaceControlDefinition(definition, identifier);
  const wxString member = alternate ? "alternate_icon_resource"
                                    : "icon_resource";
  const wxString resource = OptionalJsonString(control, member);
  if (!IsSafeSurfaceResource(resource)) {
    wxLogWarning("Portable surface control %s has no safe %s", identifier,
                 member);
    return false;
  }
  wxFileName icon(package_root + wxFILE_SEP_PATH + resource);
  icon.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
  if (!icon.FileExists()) {
    wxLogWarning("Portable surface control %s icon is missing: %s", identifier,
                 icon.GetFullPath());
    return false;
  }
#ifdef ocpnUSE_wxBitmapBundle
  const wxSize size(32, 32);
  const auto bundle = wxBitmapBundle::FromSVGFile(icon.GetFullPath(), size);
  if (!bundle.IsOk()) {
    wxLogWarning("Portable surface control %s icon could not be decoded: %s",
                 identifier, icon.GetFullPath());
    return false;
  }
  button->SetBitmap(bundle);
  button->SetBitmapPosition(wxLEFT);
  if (OptionalJsonBool(control, "icon_only")) {
    button->SetBitmapMargins(0, 0);
    button->SetLabel(wxEmptyString);
    button->SetMinSize(wxSize(42, 38));
  } else {
    button->SetBitmapMargins(4, 0);
    button->SetWindowStyleFlag(button->GetWindowStyleFlag() | wxBU_LEFT);
  }
  return true;
#else
  wxUnusedVar(alternate);
  return false;
#endif
}

struct EnvironmentalProviderOption {
  wxString id;
  wxString label;
  wxString status;
  wxString credential;
  wxString coverage;
  wxString requirements;
  wxString limitations;
  bool fallback = false;
  bool selected_by_default = false;
  bool disabled = false;
  wxString input_kind;
  wxString input_label;
  wxString input_request_key;
  wxString input_filter;
  wxString mode_request_key;
  std::vector<std::pair<wxString, wxString>> mode_options;
};

struct EnvironmentalAreaPreset {
  wxString id;
  wxString label;
  wxString kind;
  double west = 0.0;
  double south = 0.0;
  double east = 0.0;
  double north = 0.0;
  wxString current_provider;
};

std::vector<EnvironmentalAreaPreset> SurfaceAreaPresets(
    const wxJSONValue& definition) {
  std::vector<EnvironmentalAreaPreset> result;
  wxJSONValue copy = definition;
  auto presets = copy["generator"]["area_presets"];
  for (int index = 0; index < presets.Size(); ++index) {
    auto value = presets[index];
    EnvironmentalAreaPreset preset;
    preset.id = value["id"].AsString();
    preset.label = value["label"].AsString();
    preset.kind = value["kind"].AsString();
    preset.west = value["west"].AsDouble();
    preset.south = value["south"].AsDouble();
    preset.east = value["east"].AsDouble();
    preset.north = value["north"].AsDouble();
    preset.current_provider = OptionalJsonString(value, "current_provider");
    result.push_back(std::move(preset));
  }
  return result;
}

struct EnvironmentalWeatherPreset {
  wxString id;
  wxString label;
  bool selected_by_default = false;
};

std::vector<EnvironmentalWeatherPreset> SurfaceWeatherPresets(
    const wxJSONValue& definition) {
  std::vector<EnvironmentalWeatherPreset> result;
  wxJSONValue copy = definition;
  auto presets = copy["generator"]["weather_presets"];
  for (int index = 0; index < presets.Size(); ++index) {
    auto value = presets[index];
    result.push_back({value["id"].AsString(), value["label"].AsString(),
                      OptionalJsonBool(value, "default")});
  }
  return result;
}

std::vector<EnvironmentalProviderOption> SurfaceProviderOptions(
    const wxJSONValue& definition, const wxString& category) {
  wxJSONValue copy = definition;
  auto entries = copy["provider_catalog"][category];
  std::vector<EnvironmentalProviderOption> result;
  result.reserve(entries.Size());
  for (int i = 0; i < entries.Size(); ++i) {
    auto entry = entries[i];
    EnvironmentalProviderOption option;
    option.id = entry["id"].AsString();
    option.label = entry["label"].AsString();
    option.status = entry["status"].AsString();
    option.credential = entry["credential"].AsString();
    option.coverage = OptionalJsonString(entry, "coverage");
    option.requirements = OptionalJsonString(entry, "requirements");
    option.limitations = OptionalJsonString(entry, "limitations");
    option.fallback = OptionalJsonBool(entry, "fallback");
    option.selected_by_default = OptionalJsonBool(entry, "default");
    option.disabled = OptionalJsonBool(entry, "disabled");
    option.input_kind = OptionalJsonString(entry, "input_kind");
    option.input_label = OptionalJsonString(entry, "input_label");
    option.input_request_key = OptionalJsonString(entry, "input_request_key");
    option.input_filter = OptionalJsonString(entry, "input_filter");
    option.mode_request_key = OptionalJsonString(entry, "mode_request_key");
    if (entry["mode_options"].IsArray()) {
      for (int item = 0; item < entry["mode_options"].Size(); ++item) {
        auto mode = entry["mode_options"][item];
        if (mode.IsObject() && mode["label"].IsString() &&
            mode["value"].IsString())
          option.mode_options.emplace_back(mode["label"].AsString(),
                                           mode["value"].AsString());
      }
    }
    result.push_back(std::move(option));
  }
  return result;
}

std::vector<EnvironmentalProviderOption> FallbackProviderOptions(
    const std::vector<EnvironmentalProviderOption>& providers) {
  std::vector<EnvironmentalProviderOption> result;
  std::copy_if(providers.begin(), providers.end(), std::back_inserter(result),
               [](const auto& provider) { return provider.fallback; });
  return result;
}

wxArrayString ProviderLabels(
    const std::vector<EnvironmentalProviderOption>& providers) {
  wxArrayString labels;
  for (const auto& provider : providers) labels.Add(provider.label);
  return labels;
}

int ProviderIndex(const std::vector<EnvironmentalProviderOption>& providers,
                  const wxString& id, int fallback = 0) {
  const auto found =
      std::find_if(providers.begin(), providers.end(),
                   [&](const auto& item) { return item.id == id; });
  return found == providers.end()
             ? fallback
             : static_cast<int>(std::distance(providers.begin(), found));
}

int DefaultProviderIndex(
    const std::vector<EnvironmentalProviderOption>& providers) {
  const auto found =
      std::find_if(providers.begin(), providers.end(),
                   [](const auto& item) { return item.selected_by_default; });
  return found == providers.end()
             ? (providers.empty() ? wxNOT_FOUND : 0)
             : static_cast<int>(std::distance(providers.begin(), found));
}

wxString MakeResultPath(const wxString& directory, const wxString& operation) {
  const auto ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return directory + wxFILE_SEP_PATH +
         wxString::Format("%s-%lld.json", operation,
                          static_cast<long long>(ticks));
}

// wxExecute(wxEXEC_SYNC) uses wxGUIAppTraits in a GUI application. When it is
// called by a routing worker this enters wxWindowDisabler and GTK from the
// wrong thread. Routing helpers are already wrapped by prlimit and bubblewrap;
// launch that argv directly without involving wxWidgets' GUI process layer.
bool RunHeadlessProcess(const std::vector<wxString>& arguments, long* exit_code,
                        wxString* error) {
#if defined(__linux__) || defined(__APPLE__)
  if (arguments.empty() || !exit_code) {
    if (error) *error = "invalid headless helper command";
    return false;
  }
  std::vector<std::string> encoded;
  encoded.reserve(arguments.size());
  for (const auto& argument : arguments) {
    const wxScopedCharBuffer utf8 = argument.ToUTF8();
    if (!utf8.data()) {
      if (error) *error = "helper argument is not valid UTF-8";
      return false;
    }
    encoded.emplace_back(utf8.data());
  }
  std::vector<char*> argv;
  argv.reserve(encoded.size() + 1);
  for (auto& argument : encoded) argv.push_back(argument.data());
  argv.push_back(nullptr);

  pid_t child = -1;
  const int spawn_error =
      posix_spawn(&child, argv.front(), nullptr, nullptr, argv.data(), environ);
  if (spawn_error != 0) {
    if (error)
      *error = "could not start supervised decoder: " +
               wxString::FromUTF8(std::strerror(spawn_error));
    return false;
  }
  int status = 0;
  pid_t waited = -1;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child) {
    if (error)
      *error = "could not wait for supervised decoder: " +
               wxString::FromUTF8(std::strerror(errno));
    return false;
  }
  if (WIFEXITED(status))
    *exit_code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    *exit_code = 128 + WTERMSIG(status);
  else
    *exit_code = -1;
  return true;
#elif defined(_WIN32)
  if (arguments.empty() || !exit_code) {
    if (error) *error = "invalid headless helper command";
    return false;
  }
  auto quote = [](const wxString& argument) {
    std::wstring value(argument.wc_str());
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t character : value) {
      if (character == L'\\') {
        ++backslashes;
      } else if (character == L'\"') {
        result.append(backslashes * 2 + 1, L'\\');
        result.push_back(character);
        backslashes = 0;
      } else {
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
      }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
  };
  std::wstring command_line;
  for (const auto& argument : arguments) {
    if (!command_line.empty()) command_line.push_back(L' ');
    command_line += quote(argument);
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) {
    if (error) *error = "could not create Windows helper job object";
    return false;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
      JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_PROCESS_TIME;
  limits.BasicLimitInformation.ActiveProcessLimit = 1;
  limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
      30LL * 10'000'000LL;
  limits.ProcessMemoryLimit = 512ULL * 1024ULL * 1024ULL;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits)) ||
      !CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
                      &startup, &process) ||
      !AssignProcessToJobObject(job, process.hProcess)) {
    if (process.hProcess) TerminateProcess(process.hProcess, 127);
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    CloseHandle(job);
    if (error) *error = "could not start contained Windows decoder process";
    return false;
  }
  ResumeThread(process.hThread);
  const DWORD wait_result = WaitForSingleObject(process.hProcess, 45'000);
  DWORD code = 127;
  if (wait_result == WAIT_TIMEOUT) {
    TerminateJobObject(job, 124);
    if (error) *error = "contained decoder exceeded its 45 second deadline";
  } else if (wait_result != WAIT_OBJECT_0 ||
             !GetExitCodeProcess(process.hProcess, &code)) {
    TerminateJobObject(job, 127);
    if (error) *error = "could not wait for contained Windows decoder";
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(job);
  *exit_code = wait_result == WAIT_OBJECT_0 ? static_cast<long>(code) : 124;
  return wait_result == WAIT_OBJECT_0;
#else
  static_cast<void>(arguments);
  static_cast<void>(exit_code);
  if (error)
    *error = "headless helper supervision is not implemented for this host";
  return false;
#endif
}

#if defined(_WIN32)
bool AssignWindowsHelperJob(long process_id, bool generator, HANDLE* output,
                            wxString* error) {
  if (!output || process_id <= 0) return false;
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  HANDLE process = OpenProcess(
      PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
      FALSE, static_cast<DWORD>(process_id));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
      JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_PROCESS_TIME;
  limits.BasicLimitInformation.ActiveProcessLimit = 1;
  limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
      (generator ? 1800LL : 30LL) * 10'000'000LL;
  limits.ProcessMemoryLimit = (generator ? 4ULL * 1024ULL * 1024ULL * 1024ULL
                                         : 512ULL * 1024ULL * 1024ULL);
  const bool accepted =
      job && process &&
      SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                              sizeof(limits)) &&
      AssignProcessToJobObject(job, process);
  if (process) CloseHandle(process);
  if (!accepted) {
    if (job) CloseHandle(job);
    if (error) *error = "could not contain the Windows helper in a Job Object";
    return false;
  }
  *output = job;
  return true;
}
#endif

void AddRow(wxFlexGridSizer* grid, wxWindow* parent, const wxString& label,
            wxWindow* control) {
  grid->Add(new wxStaticText(parent, wxID_ANY, label), 0,
            wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  grid->Add(control, 1, wxEXPAND);
}

}  // namespace

class PortableEnvironmentHost::Impl : public wxEvtHandler {
public:
  enum class Operation {
    None,
    Inspect,
    DecodeFrame,
    PrefetchFrame,
    WeatherTable,
    Generate
  };

  Impl(wxWindow* parent_value, wxString plugin_id_value,
       wxString package_root_value, wxString surface_resource_value,
       bool credential_access_value, ocpn_portable_runtime* runtime_value,
       std::shared_ptr<std::mutex> runtime_mutex_value)
      : parent(parent_value),
        plugin_id(std::move(plugin_id_value)),
        package_root(std::move(package_root_value)),
        surface_resource(std::move(surface_resource_value)),
        credential_access(credential_access_value),
        runtime(runtime_value),
        runtime_mutex(std::move(runtime_mutex_value)),
        progress_timer(this, kProgressTimerId),
        playback_timer(this, kPlaybackTimerId),
        animation_timer(this, kAnimationTimerId) {
    Bind(wxEVT_END_PROCESS, &Impl::OnProcessEnded, this, kHelperProcessId);
    Bind(wxEVT_TIMER, &Impl::OnProgressTimer, this, kProgressTimerId);
    Bind(wxEVT_TIMER, &Impl::OnPlaybackTimer, this, kPlaybackTimerId);
    Bind(wxEVT_TIMER, &Impl::OnAnimationTimer, this, kAnimationTimerId);
  }

  ~Impl() override { Shutdown(); }

  bool Show(wxString* error);
  bool Render(ocpnDC& dc, const ViewPort& viewport);
  bool SampleBatch(const std::vector<PortableEnvironmentRequest>& requests,
                   std::vector<PortableEnvironmentSample>* results,
                   wxString* error) const;
  std::shared_ptr<const PortableEnvironmentDataset> AcquireDataset(
      wxString* error) const;
  bool SampleBatch(
      const std::shared_ptr<const PortableEnvironmentDataset>& dataset,
      const std::vector<PortableEnvironmentRequest>& requests,
      std::vector<PortableEnvironmentSample>* results, wxString* error) const;
  wxString DatasetSummary() const;
  bool DisplayedTime(int64_t* unix_time) const;
  void SetCursorPosition(double latitude, double longitude);
  void Shutdown();

private:
  void CreateFrame();
  void LoadFieldGroups();
  EnvironmentalFieldGroup* FindFieldGroup(const wxString& id);
  const EnvironmentalFieldGroup* FindFieldGroup(const wxString& id) const;
  wxString ActiveFieldId(const wxString& group,
                         const wxString& component = wxEmptyString) const;
  void OpenFile();
  bool StageDataset(const wxArrayString& paths, const wxString& display_name,
                    wxString* staged_path, wxString* error);
  void ShowWeatherTable();
  void StartInspect(const wxString& path,
                    const wxString& display_name = wxEmptyString);
  void StartFrame(size_t index);
  void StartFrameTime(const wxString& time, int slider_value,
                      size_t source_index);
  void PrefetchNextFrame(int slider_value);
  void HandlePrefetchFrame(wxJSONValue& value);
  void ApplyDisplayedFrame(const wxString& requested_time,
                           const DecodedEnvironmentFrame& decoded,
                           size_t sample_count, bool cached);
  wxString TimeForSlider(int value, size_t* source_index = nullptr) const;
  void ShowGenerator();
  void ShowSettings();
  void LoadSettings();
  void SaveSettings();
  wxString DisplayStateJson() const;
  bool ApplyDisplayStateJson(const wxString& state, wxString* error);
  bool NotifySurfaceEvent(const wxString& control_id,
                          const wxString& value_json, wxString* error = nullptr,
                          wxString* accepted_state = nullptr) const;
  void UpdateCursorStatus();
  void Cancel();
  bool Launch(const std::vector<wxString>& arguments, Operation next_operation,
              const wxString& result, const wxString& status,
              const wxSecretValue* secret = nullptr);
  std::vector<wxString> DecoderCommand(
      const wxString& verb, const wxString& input, const wxString& time,
      const wxString& result, const std::vector<wxString>& options = {}) const;
  std::vector<wxString> GeneratorCommand(
      const wxString& job, const wxString& result, const wxString& output,
      const wxString& weather_input, const wxString& current_input,
      const wxString& fallback_current_input) const;
  void OnProcessEnded(wxProcessEvent& event);
  void OnProgressTimer(wxTimerEvent& event);
  void OnPlaybackTimer(wxTimerEvent& event);
  void OnAnimationTimer(wxTimerEvent& event);
  void UpdateAnimationTimer();
  void HandleInspect(wxJSONValue& value);
  void HandleFrame(wxJSONValue& value);
  void HandleWeatherTable(wxJSONValue& value);
  void HandleGenerate(wxJSONValue& value);
  bool DecodeRoutingFrame(const wxString& source, const wxString& time,
                          DecodedEnvironmentFrame* decoded,
                          wxString* error) const;
  void CacheRoutingFrame(const wxString& source, const wxString& time,
                         const DecodedEnvironmentFrame& decoded) const;
  wxString FrameCacheKey(const wxString& source,
                         const wxString& time) const;
  bool FindCachedFrame(const wxString& source, const wxString& time,
                       DecodedEnvironmentFrame* decoded) const;
  void TrimFrameCache() const;
  bool IsMarinePoint(double latitude, double longitude) const;
  void SetBusy(bool busy, const wxString& status);
  wxString DecoderHelper() const;
  wxString GeneratorHelper() const;

  wxWindow* parent = nullptr;
  wxString plugin_id;
  wxString package_root;
  wxString surface_resource;
  bool credential_access = false;
  ocpn_portable_runtime* runtime = nullptr;
  std::shared_ptr<std::mutex> runtime_mutex;
  wxJSONValue surface_definition;
  wxString surface_title;
  wxString decoder_executable;
  wxString generator_executable;
  wxString credential_environment;
  wxString private_directory;
  wxFrame* frame = nullptr;
  wxStaticText* file_label = nullptr;
  wxChoice* timeline = nullptr;
  wxChoice* level_choice = nullptr;
  wxSlider* time_slider = nullptr;
  wxButton* weather_table_button = nullptr;
  std::vector<EnvironmentalFieldGroup> field_groups;
  std::map<wxString, size_t> field_group_indices;
  std::vector<wxString> level_ids;
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
  wxTimer animation_timer;
  wxProcess* process = nullptr;
  long process_id = 0;
#if defined(_WIN32)
  HANDLE windows_helper_job = nullptr;
#endif
  Operation operation = Operation::None;
  wxString result_path;
  wxString selected_file;
  wxString selected_display_name;
  wxString pending_file;
  wxString pending_display_name;
  wxString pending_frame_time;
  wxString pending_prefetch_time;
  wxString queued_frame_time;
  int queued_frame_slider = 0;
  size_t queued_frame_source = 0;
  wxString displayed_time;
  uint64_t dataset_revision = 0;
  wxString generated_output_path;
  bool generated_open_after = true;
  wxString last_grib_directory;
  std::vector<wxString> times;
  std::map<wxString, std::vector<Sample>> fields;
  std::map<wxString, wxString> field_units;
  std::map<wxString, wxString> field_source_times;
  mutable std::mutex field_mutex;
  mutable std::mutex routing_decode_mutex;
  mutable std::map<wxString, DecodedEnvironmentFrame> routing_frames;
  mutable std::list<wxString> routing_frame_lru;
  mutable std::map<wxString, size_t> routing_frame_bytes;
  mutable size_t routing_frame_cache_bytes = 0;
  mutable std::mutex land_mask_mutex;
  mutable std::map<std::pair<int32_t, int32_t>, bool> land_mask_cache;
  double cursor_latitude = 0.0;
  double cursor_longitude = 0.0;
  bool have_cursor = false;
  bool wind_barbs = true;
  bool loop_playback = true;
  bool interpolate_timeline = true;
  bool gradual_colours = true;
  bool high_definition = false;
  int interpolation_slices = 4;
  int loop_start_percent = 0;
  LayerDisplaySettings wind_display{
      0, true, false, false, false, 44, 70, 5, 0, wxColour(145, 88, 25)};
  LayerDisplaySettings pressure_display{
      0, false, false, false, true, 44, 70, 4, 0, wxColour(40, 90, 190)};
  LayerDisplaySettings wave_display{
      0,  true, false, false, false, 44, 70, 1, 0, wxColour(0, 180, 210),
      18, 0.0};
  LayerDisplaySettings current_display{
      0, true, false, false, false, 44, 70, 1, 2, wxColour(30, 90, 220)};
  LayerDisplaySettings temperature_display{
      0, false, true, false, false, 44, 70, 2, 0, wxColour(220, 65, 35)};
  int overlay_opacity = 145;
  int playback_interval_ms = 1200;
  size_t frame_cache_budget_bytes = 256U * 1024U * 1024U;
  bool stopped = false;
};

wxString PortableEnvironmentHost::Impl::DecoderHelper() const {
  wxString path = package_root + wxFILE_SEP_PATH + "helpers" + wxFILE_SEP_PATH +
                  HostHelperDirectory() + wxFILE_SEP_PATH + decoder_executable;
#if defined(__WXMSW__)
  path += ".exe";
#endif
  return path;
}

wxString PortableEnvironmentHost::Impl::GeneratorHelper() const {
  wxString path = package_root + wxFILE_SEP_PATH + "helpers" + wxFILE_SEP_PATH +
                  HostHelperDirectory() + wxFILE_SEP_PATH +
                  generator_executable;
#if defined(__WXMSW__)
  path += ".exe";
#endif
  return path;
}

void PortableEnvironmentHost::Impl::LoadFieldGroups() {
  field_groups.clear();
  field_group_indices.clear();
  level_ids.clear();
  if (surface_definition["levels"].IsArray()) {
    for (int index = 0; index < surface_definition["levels"].Size(); ++index) {
      wxJSONValue value = surface_definition["levels"][index];
      if (value.IsObject() && value["id"].IsString())
        level_ids.push_back(value["id"].AsString());
    }
  }
  if (level_ids.empty()) level_ids.push_back("surface");
  if (!surface_definition["field_groups"].IsArray()) return;
  for (int index = 0; index < surface_definition["field_groups"].Size();
       ++index) {
    wxJSONValue value = surface_definition["field_groups"][index];
    if (!value.IsObject() || !value["id"].IsString() ||
        !value["label"].IsString())
      continue;
    EnvironmentalFieldGroup group;
    group.id = value["id"].AsString();
    group.label = value["label"].AsString();
    group.kind = value["kind"].AsString();
    group.levels = OptionalJsonBool(value, "levels");
    group.marine = OptionalJsonBool(value, "marine");
    group.default_visible = OptionalJsonBool(value, "default_visible");
    if (value["components"].IsArray())
      for (int item = 0; item < value["components"].Size(); ++item)
        if (value["components"][item].IsString())
          group.components.push_back(value["components"][item].AsString());
    if (value["presentations"].IsArray())
      for (int item = 0; item < value["presentations"].Size(); ++item)
        if (value["presentations"][item].IsString())
          group.presentations.insert(value["presentations"][item].AsString());
    const wxColour colour(value["default_colour"].AsString());
    if (colour.IsOk()) group.display.colour = colour;
    group.display.vectors =
        group.kind == "vector" || group.kind == "directional-scalar";
    group.display.overlay =
        HasPresentation(group, "scalar-map") && !group.display.vectors;
    group.display.numbers = false;
    group.display.contours = HasPresentation(group, "isobars") ||
                             HasPresentation(group, "isotachs") ||
                             HasPresentation(group, "isotherms") ||
                             HasPresentation(group, "contours");
    if (group.id == "pressure") group.display.contour_spacing = 4;
    if (group.id == "air-temperature" || group.id == "sea-temperature")
      group.display.contour_spacing = 2;
    if (group.id == "cloud" || group.id == "relative-humidity")
      group.display.contour_spacing = 10;
    if (group.id == "cape") group.display.contour_spacing = 100;
    if (group.id == "composite-reflectivity") group.display.contour_spacing = 5;
    if (group.id == "geopotential-height") group.display.contour_spacing = 50;
    if (group.id == "current") group.display.vector_style = 2;
    if (group.id == "wave") {
      group.display.vector_style = 0;
      group.display.proportional_base_size = 18;
    }
    field_group_indices[group.id] = field_groups.size();
    field_groups.push_back(std::move(group));
  }
}

EnvironmentalFieldGroup* PortableEnvironmentHost::Impl::FindFieldGroup(
    const wxString& id) {
  const auto found = field_group_indices.find(id);
  return found == field_group_indices.end() ? nullptr
                                            : &field_groups[found->second];
}

const EnvironmentalFieldGroup* PortableEnvironmentHost::Impl::FindFieldGroup(
    const wxString& id) const {
  const auto found = field_group_indices.find(id);
  return found == field_group_indices.end() ? nullptr
                                            : &field_groups[found->second];
}

wxString PortableEnvironmentHost::Impl::ActiveFieldId(
    const wxString& group, const wxString& component) const {
  wxString result = group;
  if (!component.empty()) result += "-" + component;
  const auto* definition = FindFieldGroup(group);
  if (definition && definition->levels && level_choice &&
      level_choice->GetSelection() > 0 &&
      static_cast<size_t>(level_choice->GetSelection()) < level_ids.size())
    result +=
        "@" + level_ids[static_cast<size_t>(level_choice->GetSelection())];
  return result;
}

void PortableEnvironmentHost::Impl::CreateFrame() {
  LoadFieldGroups();
  LoadSettings();
  wxString controller_state;
  wxString controller_error;
  if (NotifySurfaceEvent("display-settings", "{\"request\":\"restore\"}",
                         &controller_error, &controller_state) &&
      controller_state != "{}" && !controller_state.empty()) {
    wxString state_error;
    if (!ApplyDisplayStateJson(controller_state, &state_error))
      wxLogWarning("Portable environmental display state was ignored: %s",
                   state_error);
  } else if (!controller_error.empty()) {
    wxLogWarning("Portable environmental controller state unavailable: %s",
                 controller_error);
  }
  surface_title = surface_definition["title"].AsString();
  auto layout = surface_definition["layout"];
  const int preferred_width =
      layout["preferred_width"].IsInt()
          ? std::clamp(layout["preferred_width"].AsInt(), 640, 1600)
          : 1000;
  const int preferred_height =
      layout["preferred_height"].IsInt()
          ? std::clamp(layout["preferred_height"].AsInt(), 240, 1000)
          : 420;
  frame = new wxFrame(parent, wxID_ANY, surface_title, wxDefaultPosition,
                      wxSize(preferred_width, preferred_height),
                      wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT);
  auto* root = new wxBoxSizer(wxVERTICAL);
  file_label = new wxStaticText(
      frame, wxID_ANY,
      SurfaceControlLabel(surface_definition, "file", "File") + ": (none)");
  root->Add(file_label, 0, wxEXPAND | wxALL, 7);
  root->Add(new wxStaticLine(frame), 0, wxEXPAND);

  auto* center = new wxBoxSizer(wxHORIZONTAL);
  auto* controls = new wxBoxSizer(wxVERTICAL);
  auto* timeline_row = new wxBoxSizer(wxHORIZONTAL);
  auto* previous = new wxButton(frame, wxID_ANY, "◀", wxDefaultPosition,
                                wxSize(42, -1));
  timeline = new wxChoice(frame, wxID_ANY);
  auto* next = new wxButton(frame, wxID_ANY, "▶", wxDefaultPosition,
                            wxSize(42, -1));
  play_button = new wxButton(
      frame, wxID_ANY, SurfaceControlLabel(surface_definition, "play", "Play"));
  auto* now = new wxButton(
      frame, wxID_ANY, SurfaceControlLabel(surface_definition, "now", "Now"));
  previous->SetToolTip(
      SurfaceControlLabel(surface_definition, "previous", "Previous forecast"));
  next->SetToolTip(
      SurfaceControlLabel(surface_definition, "next", "Next forecast"));
  play_button->SetToolTip(
      SurfaceControlLabel(surface_definition, "play", "Play"));
  now->SetToolTip(SurfaceControlLabel(surface_definition, "now", "Now"));
  ApplySurfaceButtonPresentation(previous, surface_definition, "previous",
                                 package_root);
  ApplySurfaceButtonPresentation(next, surface_definition, "next",
                                 package_root);
  ApplySurfaceButtonPresentation(play_button, surface_definition, "play",
                                 package_root);
  ApplySurfaceButtonPresentation(now, surface_definition, "now", package_root);
  timeline_row->Add(previous, 0, wxRIGHT, 5);
  timeline_row->Add(timeline, 1, wxRIGHT, 5);
  timeline_row->Add(next, 0, wxRIGHT, 5);
  timeline_row->Add(play_button, 0, wxRIGHT, 5);
  timeline_row->Add(now, 0);
  controls->Add(timeline_row, 0, wxEXPAND | wxALL, 7);

  auto* selection_row = new wxBoxSizer(wxHORIZONTAL);
  time_slider = new wxSlider(frame, wxID_ANY, 0, 0, 0, wxDefaultPosition,
                             wxDefaultSize, wxSL_HORIZONTAL);
  level_choice = new wxChoice(frame, wxID_ANY);
  for (int index = 0; index < surface_definition["levels"].Size(); ++index) {
    wxJSONValue level = surface_definition["levels"][index];
    level_choice->Append(level["label"].IsString() ? level["label"].AsString()
                                                   : level["id"].AsString());
  }
  if (level_choice->IsEmpty()) level_choice->Append("Surface");
  level_choice->SetSelection(0);
  selection_row->Add(new wxStaticText(
                         frame, wxID_ANY,
                         SurfaceControlLabel(surface_definition, "time-slider",
                                             "Timeline")),
                     0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
  selection_row->Add(time_slider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  selection_row->Add(new wxStaticText(
                         frame, wxID_ANY,
                         SurfaceControlLabel(surface_definition, "level",
                                             "Level")),
                     0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
  selection_row->Add(level_choice, 0, wxALIGN_CENTER_VERTICAL);
  controls->Add(selection_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 7);

  auto* data =
      new wxStaticBoxSizer(wxVERTICAL, frame, "Data at cursor position");
  std::set<wxString> primary_groups;
  std::vector<std::vector<wxString>> primary_row_ids;
  auto primary_rows = layout["primary_field_rows"];
  if (primary_rows.IsArray()) {
    for (int row = 0; row < primary_rows.Size(); ++row) {
      std::vector<wxString> ids;
      for (int item = 0; item < primary_rows[row].Size(); ++item) {
        const wxString id = primary_rows[row][item].AsString();
        if (FindFieldGroup(id) && primary_groups.insert(id).second)
          ids.push_back(id);
      }
      if (!ids.empty()) primary_row_ids.push_back(std::move(ids));
    }
  }
  if (primary_groups.empty()) {
    for (const auto& fallback :
         std::vector<std::vector<wxString>>{{"wind", "pressure", "wave"},
                                            {"current", "air-temperature"}}) {
      std::vector<wxString> ids;
      for (const auto& id : fallback)
        if (FindFieldGroup(id) && primary_groups.insert(id).second)
          ids.push_back(id);
      if (!ids.empty()) primary_row_ids.push_back(std::move(ids));
    }
  }
  for (auto& group : field_groups) {
    if (!primary_groups.count(group.id)) continue;
    group.visible = new wxCheckBox(frame, wxID_ANY, group.label);
    group.visible->SetValue(group.default_visible);
    group.value = new wxStaticText(frame, wxID_ANY, "N/A");
  }
  auto* primary_values = new wxBoxSizer(wxVERTICAL);
  for (const auto& ids : primary_row_ids) {
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    for (const auto& id : ids) {
      auto* group = FindFieldGroup(id);
      if (!group) continue;
      auto* cell = new wxBoxSizer(wxHORIZONTAL);
      cell->Add(group->visible, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
      cell->Add(group->value, 1, wxALIGN_CENTER_VERTICAL);
      row->Add(cell, 1, wxEXPAND | wxRIGHT, 14);
    }
    primary_values->Add(row, 0, wxEXPAND | wxBOTTOM, 5);
  }
  data->Add(primary_values, 0, wxEXPAND | wxALL, 5);

  std::vector<EnvironmentalFieldGroup*> additional_groups;
  for (auto& group : field_groups)
    if (!primary_groups.count(group.id)) additional_groups.push_back(&group);
  if (!additional_groups.empty()) {
    const wxString additional_label =
        layout["additional_fields_label"].IsString()
            ? layout["additional_fields_label"].AsString()
            : "Additional fields";
    auto* additional =
        new wxCollapsiblePane(frame, wxID_ANY, additional_label);
    wxWindow* pane = additional->GetPane();
    auto* additional_values = new wxFlexGridSizer(4, 4, 12);
    additional_values->AddGrowableCol(1, 1);
    additional_values->AddGrowableCol(3, 1);
    for (auto* group : additional_groups) {
      group->visible = new wxCheckBox(pane, wxID_ANY, group->label);
      group->visible->SetValue(group->default_visible);
      group->value = new wxStaticText(pane, wxID_ANY, "N/A");
      additional_values->Add(group->visible, 0, wxALIGN_CENTER_VERTICAL);
      additional_values->Add(group->value, 1,
                             wxEXPAND | wxALIGN_CENTER_VERTICAL);
    }
    pane->SetSizer(additional_values);
    additional->Collapse(true);
    additional->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED,
                     [this](wxCollapsiblePaneEvent&) {
                       if (frame) {
                         frame->Layout();
                         frame->Fit();
                       }
                     });
    data->Add(additional, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
  }
  auto assign_legacy = [&](const wxString& id, wxCheckBox** visible,
                           wxStaticText** value) {
    if (auto* group = FindFieldGroup(id)) {
      *visible = group->visible;
      *value = group->value;
    }
  };
  assign_legacy("wind", &show_wind, &wind_value);
  assign_legacy("pressure", &show_pressure, &pressure_value);
  assign_legacy("wave", &show_waves, &wave_value);
  assign_legacy("current", &show_current, &current_value);
  assign_legacy("air-temperature", &show_temperature, &temperature_value);
  data_status = new wxStaticText(frame, wxID_ANY,
                                 "Open a GRIB file to inspect its fields");
  cursor_status = new wxStaticText(frame, wxID_ANY,
                                   "Move the chart cursor to inspect data");
  data->Add(data_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
  data->Add(cursor_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
  controls->Add(data, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 7);

  progress = new wxGauge(frame, wxID_ANY, 100);
  progress->Hide();
  controls->Add(progress, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 7);
  center->Add(controls, 1, wxEXPAND);

  auto* actions = new wxBoxSizer(wxVERTICAL);
  auto make_action = [&](const wxString& id, const wxString& fallback) {
    auto* button = new wxButton(
        frame, wxID_ANY, SurfaceControlLabel(surface_definition, id, fallback),
        wxDefaultPosition, wxDefaultSize, wxBU_LEFT);
    ApplySurfaceButtonPresentation(button, surface_definition, id,
                                   package_root);
    return button;
  };
  open_button = make_action("open", "Open GRIB");
  auto* settings = make_action("settings", "Settings");
  weather_table_button = make_action("weather-table", "Weather table");
  auto* download = make_action("download", "Download GRIB");
  generate_button = make_action("generate", "Generate GRIB");
  cancel_button = make_action("cancel", "Cancel");
  cancel_button->Enable(false);
  for (auto* button : {open_button, settings, weather_table_button, download,
                       generate_button, cancel_button})
    actions->Add(button, 0, wxEXPAND | wxBOTTOM, 5);
  center->Add(actions, 0, wxEXPAND | wxALL, 7);
  root->Add(center, 1, wxEXPAND);
  frame->SetSizer(root);
  frame->SetMinSize(wxSize(700, 260));

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
  weather_table_button->Bind(wxEVT_BUTTON,
                             [this](wxCommandEvent&) { ShowWeatherTable(); });
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
      play_button->SetLabel(
          SurfaceControlLabel(surface_definition, "play", "Play"));
      ApplySurfaceButtonPresentation(play_button, surface_definition, "play",
                                     package_root);
    } else if (!times.empty()) {
      playback_timer.Start(playback_interval_ms);
      play_button->SetLabel("Pause");
      ApplySurfaceButtonPresentation(play_button, surface_definition, "play",
                                     package_root, true);
    }
  });
  timeline->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    const int selected = timeline->GetSelection();
    if (selected != wxNOT_FOUND) StartFrame(static_cast<size_t>(selected));
  });
  for (auto& group : field_groups)
    group.visible->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
      SaveSettings();
      UpdateAnimationTimer();
      wxString ignored;
      if (!NotifySurfaceEvent("display-settings", DisplayStateJson(), &ignored))
        wxLogWarning("Portable environmental controller rejected state: %s",
                     ignored);
      if (top_frame::Get()) top_frame::Get()->RefreshAllCanvas(false);
    });
  time_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
    const int value = time_slider->GetValue();
    if (value >= 0) {
      size_t source_index = 0;
      const wxString time = TimeForSlider(value, &source_index);
      if (!time.empty()) StartFrameTime(time, value, source_index);
    }
  });
  level_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    const int selected = timeline->GetSelection();
    if (selected != wxNOT_FOUND && !process)
      StartFrame(static_cast<size_t>(selected));
    else if (top_frame::Get())
      top_frame::Get()->RefreshAllCanvas(false);
  });
  if (controller_state == "{}" || controller_state.empty()) {
    wxString migration_error;
    if (!NotifySurfaceEvent("display-settings", DisplayStateJson(),
                            &migration_error))
      wxLogWarning("Portable environmental state migration failed: %s",
                   migration_error);
  }
  UpdateAnimationTimer();
}

void PortableEnvironmentHost::Impl::LoadSettings() {
  const wxString old_path = pConfig->GetPath();
  pConfig->SetPath("/PortablePlugins/" + plugin_id + "/Display");
  const long display_settings_schema =
      pConfig->ReadLong("displaySettingsSchema", 1);
  wind_barbs = pConfig->ReadBool("windBarbs", true);
  loop_playback = pConfig->ReadBool("loopPlayback", true);
  interpolate_timeline = pConfig->ReadBool("interpolateTimeline", true);
  gradual_colours = pConfig->ReadBool("gradualColours", true);
  high_definition = pConfig->ReadBool("highDefinition", false);
  interpolation_slices =
      std::clamp<int>(pConfig->ReadLong("interpolationSlices", 4), 1, 12);
  loop_start_percent =
      std::clamp<int>(pConfig->ReadLong("loopStartPercent", 0), 0, 95);
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
    settings->contour_spacing = std::clamp(
        pConfig->ReadDouble(name + "ContourSpacing", settings->contour_spacing),
        0.1, 1000.0);
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
    settings->vector_fixed_spacing = pConfig->ReadBool(
        name + "VectorFixedSpacing", settings->vector_fixed_spacing);
    settings->number_fixed_spacing = pConfig->ReadBool(
        name + "NumberFixedSpacing", settings->number_fixed_spacing);
    settings->contour_labels =
        pConfig->ReadBool(name + "ContourLabels", settings->contour_labels);
    settings->contour_labels_abbreviated =
        pConfig->ReadBool(name + "ContourLabelsAbbreviated",
                          settings->contour_labels_abbreviated);
    settings->overlay_palette = std::clamp<int>(
        pConfig->ReadLong(name + "OverlayPalette", settings->overlay_palette),
        0, 7);
    settings->particles =
        pConfig->ReadBool(name + "Particles", settings->particles);
    settings->particle_density = std::clamp<int>(
        pConfig->ReadLong(name + "ParticleDensity", settings->particle_density),
        1, 5);
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
  if (display_settings_schema < 2) {
    current_display.overlay = false;
    pConfig->Write("CurrentOverlay", false);
  }
  if (display_settings_schema < 3) {
    wave_display.vectors = true;
    wave_display.overlay = false;
    wave_display.vector_style = 0;
    wave_display.proportional_base_size = 18;
    pConfig->Write("WaveVectors", true);
    pConfig->Write("WaveOverlay", false);
    pConfig->Write("WaveVectorStyle", 0L);
    pConfig->Write("WaveProportionalBaseSize", 18L);
    pConfig->Write("displaySettingsSchema", 3L);
    pConfig->Flush();
  }
  if (!pConfig->HasEntry("TemperatureOverlay"))
    temperature_display.overlay = legacy_scalar_maps;
  auto seed_legacy_group = [&](const wxString& id,
                               const LayerDisplaySettings& settings) {
    if (auto* group = FindFieldGroup(id)) group->display = settings;
  };
  seed_legacy_group("wind", wind_display);
  seed_legacy_group("pressure", pressure_display);
  seed_legacy_group("wave", wave_display);
  seed_legacy_group("current", current_display);
  seed_legacy_group("air-temperature", temperature_display);
  for (auto& group : field_groups) {
    if (group.id == "wind" || group.id == "pressure" || group.id == "wave" ||
        group.id == "current" || group.id == "air-temperature")
      continue;
    read_layer(ConfigurationName(group.id), &group.display);
  }
  for (auto& group : field_groups)
    group.default_visible = pConfig->ReadBool(
        "show" + ConfigurationName(group.id), group.default_visible);
  overlay_opacity =
      std::clamp<int>(pConfig->ReadLong("overlayOpacity", 145), 20, 255);
  playback_interval_ms =
      std::clamp<int>(pConfig->ReadLong("playbackIntervalMs", 1200), 200, 5000);
  const long frame_cache_mib =
      std::clamp<long>(pConfig->ReadLong("frameCacheMiB", 256), 32, 4096);
  frame_cache_budget_bytes =
      static_cast<size_t>(frame_cache_mib) * 1024U * 1024U;
  last_grib_directory = pConfig->Read("lastGribDirectory", wxEmptyString);
  if (!last_grib_directory.empty() && !wxDirExists(last_grib_directory))
    last_grib_directory.clear();
  pConfig->SetPath(old_path);
}

void PortableEnvironmentHost::Impl::SaveSettings() {
  const wxString old_path = pConfig->GetPath();
  pConfig->SetPath("/PortablePlugins/" + plugin_id + "/Display");
  pConfig->Write("windBarbs", wind_barbs);
  pConfig->Write("loopPlayback", loop_playback);
  pConfig->Write("interpolateTimeline", interpolate_timeline);
  pConfig->Write("gradualColours", gradual_colours);
  pConfig->Write("highDefinition", high_definition);
  pConfig->Write("interpolationSlices",
                 static_cast<long>(interpolation_slices));
  pConfig->Write("loopStartPercent", static_cast<long>(loop_start_percent));
  pConfig->Write("displaySettingsSchema", 4L);
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
    pConfig->Write(name + "ContourSpacing", settings.contour_spacing);
    pConfig->Write(name + "VectorStyle",
                   static_cast<long>(settings.vector_style));
    pConfig->Write(name + "ProportionalBaseSize",
                   static_cast<long>(settings.proportional_base_size));
    pConfig->Write(name + "ProportionalGrowthPerKnot",
                   settings.proportional_growth_per_knot);
    pConfig->Write(name + "VectorFixedSpacing", settings.vector_fixed_spacing);
    pConfig->Write(name + "NumberFixedSpacing", settings.number_fixed_spacing);
    pConfig->Write(name + "ContourLabels", settings.contour_labels);
    pConfig->Write(name + "ContourLabelsAbbreviated",
                   settings.contour_labels_abbreviated);
    pConfig->Write(name + "OverlayPalette",
                   static_cast<long>(settings.overlay_palette));
    pConfig->Write(name + "Particles", settings.particles);
    pConfig->Write(name + "ParticleDensity",
                   static_cast<long>(settings.particle_density));
    pConfig->Write(name + "Colour",
                   settings.colour.GetAsString(wxC2S_HTML_SYNTAX));
  };
  write_layer("Wind", wind_display);
  write_layer("Pressure", pressure_display);
  write_layer("Wave", wave_display);
  write_layer("Current", current_display);
  write_layer("Temperature", temperature_display);
  for (const auto& group : field_groups) {
    if (group.id == "wind" || group.id == "pressure" || group.id == "wave" ||
        group.id == "current" || group.id == "air-temperature")
      continue;
    write_layer(ConfigurationName(group.id), group.display);
  }
  pConfig->Write("overlayOpacity", static_cast<long>(overlay_opacity));
  pConfig->Write("playbackIntervalMs", static_cast<long>(playback_interval_ms));
  pConfig->Write("frameCacheMiB",
                 static_cast<long>(frame_cache_budget_bytes / 1024U / 1024U));
  if (!last_grib_directory.empty())
    pConfig->Write("lastGribDirectory", last_grib_directory);
  if (show_wind) pConfig->Write("showWind", show_wind->GetValue());
  if (show_pressure) pConfig->Write("showPressure", show_pressure->GetValue());
  if (show_waves) pConfig->Write("showWaves", show_waves->GetValue());
  if (show_current) pConfig->Write("showCurrent", show_current->GetValue());
  if (show_temperature)
    pConfig->Write("showTemperature", show_temperature->GetValue());
  for (const auto& group : field_groups)
    if (group.visible)
      pConfig->Write("show" + ConfigurationName(group.id),
                     group.visible->GetValue());
  pConfig->SetPath(old_path);
  pConfig->Flush();
}

wxString PortableEnvironmentHost::Impl::DisplayStateJson() const {
  wxJSONValue state;
  state["schema"] = 1;
  auto& global = state["global"];
  global["loopPlayback"] = loop_playback;
  global["interpolateTimeline"] = interpolate_timeline;
  global["interpolationSlices"] = interpolation_slices;
  global["loopStartPercent"] = loop_start_percent;
  global["gradualColours"] = gradual_colours;
  global["highDefinition"] = high_definition;
  global["overlayOpacity"] = overlay_opacity;
  global["playbackIntervalMs"] = playback_interval_ms;
  for (const auto& group : field_groups) {
    auto& layer = state["layers"][group.id];
    const auto& settings = group.display;
    layer["visible"] =
        group.visible ? group.visible->GetValue() : group.default_visible;
    layer["units"] = settings.units;
    layer["vectors"] = settings.vectors;
    layer["overlay"] = settings.overlay;
    layer["numbers"] = settings.numbers;
    layer["contours"] = settings.contours;
    layer["vectorSpacing"] = settings.vector_spacing;
    layer["numberSpacing"] = settings.number_spacing;
    layer["contourSpacing"] = settings.contour_spacing;
    layer["vectorStyle"] = settings.vector_style;
    layer["colour"] = settings.colour.GetAsString(wxC2S_HTML_SYNTAX);
    layer["proportionalBaseSize"] = settings.proportional_base_size;
    layer["proportionalGrowthPerKnot"] = settings.proportional_growth_per_knot;
    layer["vectorFixedSpacing"] = settings.vector_fixed_spacing;
    layer["numberFixedSpacing"] = settings.number_fixed_spacing;
    layer["contourLabels"] = settings.contour_labels;
    layer["contourLabelsAbbreviated"] = settings.contour_labels_abbreviated;
    layer["overlayPalette"] = settings.overlay_palette;
    layer["particles"] = settings.particles;
    layer["particleDensity"] = settings.particle_density;
  }
  wxString encoded;
  wxJSONWriter writer;
  writer.Write(state, encoded);
  return encoded;
}

bool PortableEnvironmentHost::Impl::ApplyDisplayStateJson(
    const wxString& encoded, wxString* error) {
  wxJSONValue state;
  wxJSONReader reader;
  if (reader.Parse(encoded, &state) != 0 || !state.IsObject() ||
      state["schema"].AsInt() != 1 || !state["layers"].IsObject()) {
    if (error) *error = "portable display state has an incompatible schema";
    return false;
  }
  const auto assign_bool = [](wxJSONValue& object, const wxString& key,
                              bool* target) {
    if (object.HasMember(key) && object[key].IsBool())
      *target = object[key].AsBool();
  };
  auto global = state["global"];
  if (global.IsObject()) {
    assign_bool(global, "loopPlayback", &loop_playback);
    assign_bool(global, "interpolateTimeline", &interpolate_timeline);
    assign_bool(global, "gradualColours", &gradual_colours);
    assign_bool(global, "highDefinition", &high_definition);
    if (global["interpolationSlices"].IsInt())
      interpolation_slices =
          std::clamp(global["interpolationSlices"].AsInt(), 1, 12);
    if (global["loopStartPercent"].IsInt())
      loop_start_percent =
          std::clamp(global["loopStartPercent"].AsInt(), 0, 95);
    if (global["overlayOpacity"].IsInt())
      overlay_opacity = std::clamp(global["overlayOpacity"].AsInt(), 20, 255);
    if (global["playbackIntervalMs"].IsInt())
      playback_interval_ms =
          std::clamp(global["playbackIntervalMs"].AsInt(), 200, 5000);
  }
  for (auto& group : field_groups) {
    auto layer = state["layers"][group.id];
    if (!layer.IsObject()) continue;
    auto& settings = group.display;
    if (layer["visible"].IsBool())
      group.default_visible = layer["visible"].AsBool();
    assign_bool(layer, "vectors", &settings.vectors);
    assign_bool(layer, "overlay", &settings.overlay);
    assign_bool(layer, "numbers", &settings.numbers);
    assign_bool(layer, "contours", &settings.contours);
    assign_bool(layer, "vectorFixedSpacing", &settings.vector_fixed_spacing);
    assign_bool(layer, "numberFixedSpacing", &settings.number_fixed_spacing);
    assign_bool(layer, "contourLabels", &settings.contour_labels);
    assign_bool(layer, "contourLabelsAbbreviated",
                &settings.contour_labels_abbreviated);
    assign_bool(layer, "particles", &settings.particles);
    if (layer["units"].IsInt())
      settings.units = std::clamp(layer["units"].AsInt(), 0, 4);
    if (layer["vectorSpacing"].IsInt())
      settings.vector_spacing =
          std::clamp(layer["vectorSpacing"].AsInt(), 24, 120);
    if (layer["numberSpacing"].IsInt())
      settings.number_spacing =
          std::clamp(layer["numberSpacing"].AsInt(), 35, 160);
    if (layer["contourSpacing"].IsDouble() || layer["contourSpacing"].IsInt())
      settings.contour_spacing =
          std::clamp(layer["contourSpacing"].AsDouble(), 0.1, 1000.0);
    if (layer["vectorStyle"].IsInt())
      settings.vector_style = std::clamp(layer["vectorStyle"].AsInt(), 0, 2);
    if (layer["proportionalBaseSize"].IsInt())
      settings.proportional_base_size =
          std::clamp(layer["proportionalBaseSize"].AsInt(), 6, 40);
    if (layer["proportionalGrowthPerKnot"].IsDouble() ||
        layer["proportionalGrowthPerKnot"].IsInt())
      settings.proportional_growth_per_knot =
          std::clamp(layer["proportionalGrowthPerKnot"].AsDouble(), 0.0, 12.0);
    if (layer["overlayPalette"].IsInt())
      settings.overlay_palette =
          std::clamp(layer["overlayPalette"].AsInt(), 0, 7);
    if (layer["particleDensity"].IsInt())
      settings.particle_density =
          std::clamp(layer["particleDensity"].AsInt(), 1, 5);
    if (layer["colour"].IsString()) {
      const wxColour colour(layer["colour"].AsString());
      if (colour.IsOk()) settings.colour = colour;
    }
  }
  auto adopt = [this](const wxString& id, LayerDisplaySettings* output) {
    if (const auto* group = FindFieldGroup(id)) *output = group->display;
  };
  adopt("wind", &wind_display);
  adopt("pressure", &pressure_display);
  adopt("wave", &wave_display);
  adopt("current", &current_display);
  adopt("air-temperature", &temperature_display);
  wind_barbs = wind_display.vector_style == 0;
  return true;
}

bool PortableEnvironmentHost::Impl::NotifySurfaceEvent(
    const wxString& control_id, const wxString& value_json, wxString* error,
    wxString* accepted_state) const {
  if (!runtime || !runtime_mutex) {
    if (error) *error = "portable component controller is unavailable";
    return false;
  }
  wxJSONValue definition = surface_definition;
  const wxString surface_id = definition["surface_id"].AsString();
  const auto encoded_surface = surface_id.ToUTF8();
  const auto encoded_control = control_id.ToUTF8();
  const auto encoded_value = value_json.ToUTF8();
  if (!encoded_surface.data() || !encoded_control.data() ||
      !encoded_value.data()) {
    if (error) *error = "portable surface event is not valid UTF-8";
    return false;
  }
  std::vector<char> state(64U * 1024U);
  size_t state_length = 0;
  char callback_error[4096] = {};
  std::lock_guard<std::mutex> lock(*runtime_mutex);
  const int result = ocpn_portable_runtime_on_surface_event(
      runtime, encoded_surface.data(), std::strlen(encoded_surface.data()),
      encoded_control.data(), std::strlen(encoded_control.data()),
      encoded_value.data(), std::strlen(encoded_value.data()), state.data(),
      state.size(), &state_length, callback_error, sizeof(callback_error));
  if (result != 0) {
    if (error) *error = wxString::FromUTF8(callback_error);
    return false;
  }
  if (accepted_state)
    *accepted_state = wxString::FromUTF8(state.data(), state_length);
  return true;
}

void PortableEnvironmentHost::Impl::ShowSettings() {
  wxDialog dialog(frame, wxID_ANY, surface_title + " — display settings",
                  wxDefaultPosition, wxDefaultSize,
                  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* root = new wxBoxSizer(wxVERTICAL);
  auto* notebook = new wxNotebook(&dialog, wxID_ANY);

  struct LayerControls {
    LayerDisplaySettings* settings = nullptr;
    wxChoice* units = nullptr;
    wxColourPickerCtrl* colour = nullptr;
    wxCheckBox* vectors = nullptr;
    wxChoice* vector_style = nullptr;
    wxCheckBox* vector_fixed_spacing = nullptr;
    wxSpinCtrl* vector_spacing = nullptr;
    wxSpinCtrl* proportional_base_size = nullptr;
    wxSpinCtrlDouble* proportional_growth_per_knot = nullptr;
    wxCheckBox* overlay = nullptr;
    wxChoice* overlay_palette = nullptr;
    wxCheckBox* numbers = nullptr;
    wxCheckBox* number_fixed_spacing = nullptr;
    wxSpinCtrl* number_spacing = nullptr;
    wxCheckBox* contours = nullptr;
    wxSpinCtrlDouble* contour_spacing = nullptr;
    wxCheckBox* contour_labels = nullptr;
    wxCheckBox* contour_labels_abbreviated = nullptr;
    wxCheckBox* particles = nullptr;
    wxSpinCtrl* particle_density = nullptr;
  };
  std::vector<LayerControls> layer_controls;
  auto add_layer = [&](const wxString& title, LayerDisplaySettings* layer,
                       const wxArrayString& unit_names, int vector_kind,
                       const wxString& contour_name, bool supports_particles) {
    auto* page = new wxScrolledWindow(notebook, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, wxVSCROLL);
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
      wxString vector_label = "Display direction arrows";
      if (vector_kind == 1) vector_label = "Display wind vectors";
      if (vector_kind == 3) vector_label = "Display wave symbols";
      controls.vectors = new wxCheckBox(page, wxID_ANY, vector_label);
      controls.vectors->SetValue(layer->vectors);
      AddRow(grid, page, vector_kind == 3 ? "Symbols" : "Vectors",
             controls.vectors);
      wxArrayString styles;
      if (vector_kind == 1) {
        styles.Add("Meteorological barbs");
        styles.Add("Direction arrows");
      } else if (vector_kind == 2) {
        styles.Add("Single arrows");
        styles.Add("Double arrows");
        styles.Add("Proportional arrows");
      } else {
        styles.Add("Wave crests with travel marker");
        styles.Add("Travel-direction arrows");
        styles.Add("Height circles with direction tick");
      }
      controls.vector_style = new wxChoice(page, wxID_ANY, wxDefaultPosition,
                                           wxDefaultSize, styles);
      controls.vector_style->SetSelection(std::min<int>(
          layer->vector_style, static_cast<int>(styles.GetCount()) - 1));
      wxString style_label = "Current arrow form";
      if (vector_kind == 1) style_label = "Wind vector style";
      if (vector_kind == 3) style_label = "Wave symbol style";
      AddRow(grid, page, style_label, controls.vector_style);
      controls.vector_fixed_spacing = new wxCheckBox(
          page, wxID_ANY,
          "Use a fixed display grid (otherwise enforce minimum spacing)");
      controls.vector_fixed_spacing->SetValue(layer->vector_fixed_spacing);
      AddRow(grid, page, "Vector placement", controls.vector_fixed_spacing);
      controls.vector_spacing = new wxSpinCtrl(
          page, wxID_ANY, wxString::Format("%d", layer->vector_spacing),
          wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 24, 120,
          layer->vector_spacing);
      AddRow(grid, page,
             vector_kind == 3 ? "Symbol spacing (pixels)"
                              : "Vector spacing (pixels)",
             controls.vector_spacing);
      if (vector_kind == 2 || vector_kind == 3) {
        controls.proportional_base_size = new wxSpinCtrl(
            page, wxID_ANY,
            wxString::Format("%d", layer->proportional_base_size),
            wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS,
            vector_kind == 3 ? 8 : 6, 40, layer->proportional_base_size);
        AddRow(grid, page,
               vector_kind == 3 ? "Wave symbol size (pixels)"
                                : "Proportional baseline size (pixels)",
               controls.proportional_base_size);
      }
      if (vector_kind == 2) {
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
        new wxCheckBox(page, wxID_ANY,
                       vector_kind == 3 ? "Display wave-height colour overlay"
                                        : "Display colour overlay map");
    controls.overlay->SetValue(layer->overlay);
    AddRow(grid, page, "Overlay map", controls.overlay);
    wxArrayString palettes;
    palettes.Add("Selected colour (pale to strong)");
    palettes.Add("Blue–cyan");
    palettes.Add("Green–yellow–red");
    palettes.Add("Blue–white–red");
    palettes.Add("Viridis-style");
    palettes.Add("Cloud greys");
    palettes.Add("Rain blue–purple");
    palettes.Add("Temperature blue–red");
    controls.overlay_palette = new wxChoice(page, wxID_ANY, wxDefaultPosition,
                                            wxDefaultSize, palettes);
    controls.overlay_palette->SetSelection(std::clamp(
        layer->overlay_palette, 0, static_cast<int>(palettes.size()) - 1));
    AddRow(grid, page, "Overlay colour map", controls.overlay_palette);
    controls.numbers =
        new wxCheckBox(page, wxID_ANY, "Display values on chart");
    controls.numbers->SetValue(layer->numbers);
    AddRow(grid, page, "Numbers", controls.numbers);
    controls.number_fixed_spacing = new wxCheckBox(
        page, wxID_ANY,
        "Use a fixed display grid (otherwise enforce minimum spacing)");
    controls.number_fixed_spacing->SetValue(layer->number_fixed_spacing);
    AddRow(grid, page, "Number placement", controls.number_fixed_spacing);
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
      controls.contour_spacing = new wxSpinCtrlDouble(
          page, wxID_ANY, wxString::Format("%.1f", layer->contour_spacing),
          wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0.1, 1000.0,
          layer->contour_spacing, 0.1);
      controls.contour_spacing->SetDigits(1);
      AddRow(grid, page, "Contour interval", controls.contour_spacing);
      controls.contour_labels =
          new wxCheckBox(page, wxID_ANY, "Label contour lines");
      controls.contour_labels->SetValue(layer->contour_labels);
      AddRow(grid, page, "Contour labels", controls.contour_labels);
      controls.contour_labels_abbreviated =
          new wxCheckBox(page, wxID_ANY, "Use abbreviated contour labels");
      controls.contour_labels_abbreviated->SetValue(
          layer->contour_labels_abbreviated);
      AddRow(grid, page, "Label format", controls.contour_labels_abbreviated);
    }
    if (supports_particles) {
      controls.particles =
          new wxCheckBox(page, wxID_ANY, "Display animated flow particles");
      controls.particles->SetValue(layer->particles);
      AddRow(grid, page, "Particles", controls.particles);
      controls.particle_density = new wxSpinCtrl(
          page, wxID_ANY, wxString::Format("%d", layer->particle_density),
          wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 5,
          layer->particle_density);
      AddRow(grid, page, "Particle density", controls.particle_density);
    }
    auto* page_root = new wxBoxSizer(wxVERTICAL);
    page_root->Add(grid, 1, wxEXPAND | wxALL, 14);
    page->SetSizer(page_root);
    page->SetScrollRate(0, 12);
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
  wxArrayString native_units;
  native_units.Add("source units");
  for (auto& group : field_groups) {
    const wxArrayString* units = &native_units;
    if (group.id == "wind" || group.id == "wind-gust" || group.id == "current")
      units = &speed_units;
    else if (group.id == "pressure")
      units = &pressure_units;
    else if (group.id == "wave" || group.id == "geopotential-height")
      units = &height_units;
    else if (group.id == "air-temperature" || group.id == "sea-temperature")
      units = &temperature_units;
    int vector_kind = 0;
    if (group.id == "wind") vector_kind = 1;
    if (group.id == "current") vector_kind = 2;
    if (group.id == "wave") vector_kind = 3;
    wxString contour_name;
    if (HasPresentation(group, "isobars")) contour_name = "isobars";
    if (HasPresentation(group, "isotachs")) contour_name = "isotachs";
    if (HasPresentation(group, "isotherms")) contour_name = "isotherms";
    if (HasPresentation(group, "contours")) contour_name = "contours";
    add_layer(group.label, &group.display, *units, vector_kind, contour_name,
              HasPresentation(group, "particles"));
  }

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
  auto* interpolate = new wxCheckBox(
      playback_page, wxID_ANY, "Interpolate between source forecast times");
  interpolate->SetValue(interpolate_timeline);
  auto* slices = new wxSpinCtrl(playback_page, wxID_ANY,
                                wxString::Format("%d", interpolation_slices),
                                wxDefaultPosition, wxDefaultSize,
                                wxSP_ARROW_KEYS, 1, 12, interpolation_slices);
  auto* loop_start = new wxSpinCtrl(playback_page, wxID_ANY,
                                    wxString::Format("%d", loop_start_percent),
                                    wxDefaultPosition, wxDefaultSize,
                                    wxSP_ARROW_KEYS, 0, 95, loop_start_percent);
  auto* gradual = new wxCheckBox(playback_page, wxID_ANY,
                                 "Use gradual colour-map transitions");
  gradual->SetValue(gradual_colours);
  auto* high_definition = new wxCheckBox(
      playback_page, wxID_ANY, "High-definition overlays (more samples)");
  high_definition->SetValue(this->high_definition);
  auto* frame_cache = new wxSpinCtrl(
      playback_page, wxID_ANY,
      wxString::Format("%zu", frame_cache_budget_bytes / 1024U / 1024U),
      wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 32, 4096,
      static_cast<int>(frame_cache_budget_bytes / 1024U / 1024U));
  AddRow(playback_grid, playback_page, "Overlay opacity (20–255)", opacity);
  AddRow(playback_grid, playback_page, "Playback interval (milliseconds)",
         playback);
  AddRow(playback_grid, playback_page, "Timeline", loop);
  AddRow(playback_grid, playback_page, "Interpolation", interpolate);
  AddRow(playback_grid, playback_page, "Slices per forecast interval", slices);
  AddRow(playback_grid, playback_page, "Loop restart point (%)", loop_start);
  AddRow(playback_grid, playback_page, "Colour maps", gradual);
  AddRow(playback_grid, playback_page, "Resolution", high_definition);
  AddRow(playback_grid, playback_page, "Decoded-frame cache (MiB)",
         frame_cache);
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
    if (controls.vector_fixed_spacing)
      layer.vector_fixed_spacing = controls.vector_fixed_spacing->GetValue();
    if (controls.proportional_base_size)
      layer.proportional_base_size =
          controls.proportional_base_size->GetValue();
    if (controls.proportional_growth_per_knot)
      layer.proportional_growth_per_knot =
          controls.proportional_growth_per_knot->GetValue();
    layer.overlay = controls.overlay->GetValue();
    layer.overlay_palette = controls.overlay_palette->GetSelection();
    layer.numbers = controls.numbers->GetValue();
    layer.number_fixed_spacing = controls.number_fixed_spacing->GetValue();
    layer.number_spacing = controls.number_spacing->GetValue();
    if (controls.contours) layer.contours = controls.contours->GetValue();
    if (controls.contour_spacing)
      layer.contour_spacing = controls.contour_spacing->GetValue();
    if (controls.contour_labels)
      layer.contour_labels = controls.contour_labels->GetValue();
    if (controls.contour_labels_abbreviated)
      layer.contour_labels_abbreviated =
          controls.contour_labels_abbreviated->GetValue();
    if (controls.particles) layer.particles = controls.particles->GetValue();
    if (controls.particle_density)
      layer.particle_density = controls.particle_density->GetValue();
    if (controls.vector_style)
      layer.vector_style = controls.vector_style->GetSelection();
  }
  wind_barbs = wind_display.vector_style == 0;
  auto adopt_group = [&](const wxString& id, LayerDisplaySettings* settings) {
    if (const auto* group = FindFieldGroup(id)) *settings = group->display;
  };
  adopt_group("wind", &wind_display);
  adopt_group("pressure", &pressure_display);
  adopt_group("wave", &wave_display);
  adopt_group("current", &current_display);
  adopt_group("air-temperature", &temperature_display);
  wind_barbs = wind_display.vector_style == 0;
  overlay_opacity = opacity->GetValue();
  playback_interval_ms = playback->GetValue();
  loop_playback = loop->GetValue();
  interpolate_timeline = interpolate->GetValue();
  interpolation_slices = slices->GetValue();
  loop_start_percent = loop_start->GetValue();
  gradual_colours = gradual->GetValue();
  this->high_definition = high_definition->GetValue();
  frame_cache_budget_bytes =
      static_cast<size_t>(frame_cache->GetValue()) * 1024U * 1024U;
  {
    std::lock_guard<std::mutex> decode_lock(routing_decode_mutex);
    TrimFrameCache();
  }
  if (time_slider && !times.empty()) {
    const int maximum = static_cast<int>(times.size() - 1) *
                        (interpolate_timeline ? interpolation_slices : 1);
    time_slider->SetRange(0, maximum);
    time_slider->SetValue(std::clamp(time_slider->GetValue(), 0, maximum));
  }
  if (playback_timer.IsRunning()) playback_timer.Start(playback_interval_ms);
  UpdateAnimationTimer();
  SaveSettings();
  wxString controller_error;
  if (!NotifySurfaceEvent("display-settings", DisplayStateJson(),
                          &controller_error))
    wxLogWarning(
        "Portable environmental controller rejected display settings: %s",
        controller_error);
  if (top_frame::Get()) top_frame::Get()->RefreshAllCanvas(false);
}

bool PortableEnvironmentHost::Impl::Show(wxString* error) {
  if (stopped) {
    *error = "environmental service is stopped";
    return false;
  }
  if (!LoadDeclarativeSurface(package_root + wxFILE_SEP_PATH + surface_resource,
                              &surface_definition, error))
    return false;
  decoder_executable =
      surface_definition["helpers"]["decoder_executable"].AsString();
  generator_executable =
      surface_definition["helpers"]["generator_executable"].AsString();
  credential_environment =
      surface_definition["helpers"]["credential_environment"].AsString();
  if (!wxFileExists(DecoderHelper())) {
    *error = "this package has no declared decoder helper for " +
             HostHelperDirectory();
    return false;
  }
  if (!HelperSupervisionAvailable(error)) return false;
  private_directory = g_BasePlatform->GetPrivateDataDir() + wxFILE_SEP_PATH +
                      "portable-plugin-data" + wxFILE_SEP_PATH + plugin_id;
  if (!wxDirExists(private_directory) &&
      !wxFileName::Mkdir(private_directory, 0700, wxPATH_MKDIR_FULL)) {
    *error = "could not create portable environmental private storage";
    return false;
  }
  if (!frame) CreateFrame();
  frame->Show();
  frame->Raise();
  return true;
}

void PortableEnvironmentHost::Impl::OpenFile() {
  wxFileDialog dialog(frame, "Open environmental GRIB", last_grib_directory,
                      wxEmptyString,
                      "GRIB files (*.grb;*.grib;*.grb2)|*.grb;*.grib;*.grb2|"
                      "All files (*.*)|*.*",
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
  if (dialog.ShowModal() != wxID_OK) return;
  wxArrayString paths;
  dialog.GetPaths(paths);
  if (paths.empty()) return;
  last_grib_directory = wxFileName(paths[0]).GetPath();
  SaveSettings();
  wxString display_name;
  for (size_t index = 0; index < paths.size(); ++index) {
    if (!display_name.empty()) display_name += ", ";
    display_name += wxFileName(paths[index]).GetFullName();
  }
  wxString staged_path;
  wxString error;
  if (!StageDataset(paths, display_name, &staged_path, &error)) {
    wxMessageBox(error, surface_title, wxOK | wxICON_ERROR, frame);
    return;
  }
  StartInspect(staged_path, display_name);
}

bool PortableEnvironmentHost::Impl::StageDataset(const wxArrayString& paths,
                                                 const wxString& display_name,
                                                 wxString* staged_path,
                                                 wxString* error) {
  if (!staged_path || paths.empty()) {
    if (error) *error = "No environmental source files were selected.";
    return false;
  }
  const auto ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const wxString target =
      private_directory + wxFILE_SEP_PATH +
      wxString::Format("dataset-%lld.grb", static_cast<long long>(ticks));
  const wxString temporary = target + ".tmp";
  wxFFileOutputStream output(temporary);
  bool valid = output.IsOk();
  for (size_t index = 0; valid && index < paths.size(); ++index) {
    wxFFileInputStream input(paths[index]);
    valid = input.IsOk();
    if (valid) {
      output.Write(input);
      valid = output.IsOk() && input.Eof();
    }
  }
  output.Close();
  valid = valid && wxRenameFile(temporary, target, false);
  if (!valid) {
    if (wxFileExists(temporary)) wxRemoveFile(temporary);
    if (wxFileExists(target)) wxRemoveFile(target);
    if (error)
      *error = "Could not construct the immutable environmental dataset '" +
               display_name + "'.";
    return false;
  }
  // The viewer and consumers read a private snapshot, never the mutable file
  // selected by the user. This also makes deterministic last-file-wins merge
  // ordering stable for the complete lifetime of a held dataset handle.
  wxChmod(target, 0400);
  *staged_path = target;
  return true;
}

void PortableEnvironmentHost::Impl::ShowWeatherTable() {
  if (process || selected_file.empty()) return;
  if (!have_cursor) {
    wxMessageBox("Move the chart cursor to the position for the weather table.",
                 surface_title, wxOK | wxICON_INFORMATION, frame);
    return;
  }
  const wxString result = MakeResultPath(private_directory, "weather-table");
  const std::vector<wxString> options = {
      wxString::Format("%.8f", cursor_latitude),
      wxString::Format("%.8f", cursor_longitude)};
  Launch(DecoderCommand("table", selected_file, wxEmptyString, result, options),
         Operation::WeatherTable, result,
         "Building weather table in supervised decoder…");
}

std::vector<wxString> PortableEnvironmentHost::Impl::DecoderCommand(
    const wxString& verb, const wxString& input, const wxString& time,
    const wxString& result, const std::vector<wxString>& options) const {
#if defined(__linux__)
  if (HelperSupervisionAvailable()) {
    std::vector<wxString> command = {"/usr/bin/prlimit", "--as=536870912",
                                     "--cpu=30", "--"};
    if (IsFlatpakRuntime()) {
      command.insert(command.end(), {DecoderHelper(), verb, input});
      if (verb == "frame") {
        command.push_back(time);
        command.push_back(high_definition ? "30000" : "12000");
      } else {
        command.insert(command.end(), options.begin(), options.end());
      }
      command.push_back(result);
      return command;
    }
    const std::vector<wxString> sandbox = {
        "/usr/bin/bwrap",
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
        wxFileName(DecoderHelper()).GetPath(),
        "/helper",
        "--ro-bind",
        input,
        "/input.grb",
        "--bind",
        private_directory,
        "/output",
        "/helper/" + wxFileName(DecoderHelper()).GetFullName(),
        verb,
        "/input.grb"};
    command.insert(command.end(), sandbox.begin(), sandbox.end());
    if (verb == "frame") {
      command.push_back(time);
      command.push_back(high_definition ? "30000" : "12000");
    } else {
      command.insert(command.end(), options.begin(), options.end());
    }
    command.push_back("/output/" + wxFileName(result).GetFullName());
    return command;
  }
#elif defined(__APPLE__)
  if (HelperSupervisionAvailable()) {
    std::vector<wxString> command = {
        "/usr/bin/sandbox-exec",
        "-p",
        MacSandboxProfile(DecoderHelper(), {input}, private_directory,
                          private_directory, false),
        DecoderHelper(),
        verb,
        input};
    if (verb == "frame") {
      command.push_back(time);
      command.push_back(high_definition ? "30000" : "12000");
    } else {
      command.insert(command.end(), options.begin(), options.end());
    }
    command.push_back(result);
    return command;
  }
#elif defined(_WIN32)
  if (HelperSupervisionAvailable()) {
    std::vector<wxString> command = {DecoderHelper(), verb, input};
    if (verb == "frame") {
      command.push_back(time);
      command.push_back(high_definition ? "30000" : "12000");
    } else {
      command.insert(command.end(), options.begin(), options.end());
    }
    command.push_back(result);
    return command;
  }
#endif
  static_cast<void>(verb);
  static_cast<void>(input);
  static_cast<void>(time);
  static_cast<void>(result);
  static_cast<void>(options);
  return {};
}

std::vector<wxString> PortableEnvironmentHost::Impl::GeneratorCommand(
    const wxString& job, const wxString& result, const wxString& output,
    const wxString& weather_input, const wxString& current_input,
    const wxString& fallback_current_input) const {
#if defined(__linux__)
  if (HelperSupervisionAvailable()) {
    std::vector<wxString> command = {"/usr/bin/prlimit", "--as=4294967296",
                                     "--cpu=1800", "--"};
    if (IsFlatpakRuntime()) {
      command.insert(command.end(), {GeneratorHelper(), "run-job", "--job", job,
                                     "--result", result});
      return command;
    }
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
                                     wxFileName(GeneratorHelper()).GetPath(),
                                     "/helper",
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
    if (!weather_input.empty() || !current_input.empty() ||
        !fallback_current_input.empty()) {
      sandbox.insert(sandbox.end(), {"--dir", "/inputs"});
      if (!weather_input.empty())
        sandbox.insert(sandbox.end(),
                       {"--ro-bind", weather_input, "/inputs/weather.grb"});
      if (!current_input.empty())
        sandbox.insert(sandbox.end(),
                       {"--ro-bind", current_input, "/inputs/current-source"});
      if (!fallback_current_input.empty())
        sandbox.insert(sandbox.end(), {"--ro-bind", fallback_current_input,
                                       "/inputs/fallback-current-source"});
    }
    command.insert(command.end(), sandbox.begin(), sandbox.end());
    command.insert(command.end(),
                   {"/helper/" + wxFileName(GeneratorHelper()).GetFullName(),
                    "run-job", "--job", "/job/" + wxFileName(job).GetFullName(),
                    "--result", "/job/" + wxFileName(result).GetFullName()});
    return command;
  }
#elif defined(__APPLE__)
  if (HelperSupervisionAvailable()) {
    const wxString output_directory = wxFileName(output).GetPath();
    return {"/usr/bin/sandbox-exec",
            "-p",
            MacSandboxProfile(
                GeneratorHelper(),
                {job, weather_input, current_input, fallback_current_input},
                private_directory, output_directory, true),
            GeneratorHelper(),
            "run-job",
            "--job",
            job,
            "--result",
            result};
  }
#elif defined(_WIN32)
  if (HelperSupervisionAvailable())
    return {GeneratorHelper(), "run-job", "--job", job, "--result", result};
#endif
  static_cast<void>(job);
  static_cast<void>(result);
  static_cast<void>(output);
  static_cast<void>(weather_input);
  static_cast<void>(current_input);
  static_cast<void>(fallback_current_input);
  return {};
}

bool PortableEnvironmentHost::Impl::Launch(
    const std::vector<wxString>& arguments, Operation next_operation,
    const wxString& result, const wxString& status,
    const wxSecretValue* secret) {
  if (process) return false;
  if (arguments.empty()) {
    wxString error;
    HelperSupervisionAvailable(&error);
    wxMessageBox(
        "Refusing unsupervised environmental helper execution: " + error,
        surface_title, wxOK | wxICON_ERROR, frame);
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
      if (!credential_environment.empty())
        environment.env[credential_environment] = transient_secret;
    }
    environment_pointer = &environment;
  }
  process_id = wxExecute(argv.data(), wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE,
                         process, environment_pointer);
  auto secret_entry = environment.env.find(credential_environment);
  if (secret_entry != environment.env.end()) {
    wxSecretValue::WipeString(secret_entry->second);
    environment.env.erase(secret_entry);
  }
  if (process_id == 0) {
    delete process;
    process = nullptr;
    wxMessageBox("Could not start the signed environmental helper",
                 surface_title, wxOK | wxICON_ERROR, frame);
    return false;
  }
#if defined(_WIN32)
  wxString containment_error;
  if (!AssignWindowsHelperJob(process_id, next_operation == Operation::Generate,
                              &windows_helper_job, &containment_error)) {
    wxProcess::Kill(static_cast<int>(process_id), wxSIGKILL, wxKILL_CHILDREN);
    process->Detach();
    delete process;
    process = nullptr;
    process_id = 0;
    wxMessageBox(
        "Refusing an uncontained environmental helper: " + containment_error,
        surface_title, wxOK | wxICON_ERROR, frame);
    return false;
  }
#endif
  operation = next_operation;
  result_path = result;
  if (next_operation != Operation::PrefetchFrame) SetBusy(true, status);
  return true;
}

void PortableEnvironmentHost::Impl::StartInspect(const wxString& path,
                                                 const wxString& display_name) {
  if (process) return;
  {
    std::lock_guard<std::mutex> land_lock(land_mask_mutex);
    land_mask_cache.clear();
  }
  pending_file = path;
  pending_display_name =
      display_name.empty() ? wxFileName(path).GetFullName() : display_name;
  const wxString result = MakeResultPath(private_directory, "inspect");
  Launch(DecoderCommand("inspect", path, wxEmptyString, result),
         Operation::Inspect, result, "Inspecting GRIB metadata…");
}

void PortableEnvironmentHost::Impl::StartFrame(size_t index) {
  if (index >= times.size()) return;
  const int slices = interpolate_timeline ? interpolation_slices : 1;
  StartFrameTime(times[index], static_cast<int>(index) * slices, index);
}

wxString PortableEnvironmentHost::Impl::TimeForSlider(
    int value, size_t* source_index) const {
  if (times.empty()) return wxEmptyString;
  const int slices = interpolate_timeline ? interpolation_slices : 1;
  const int maximum = static_cast<int>(times.size() - 1) * slices;
  value = std::clamp(value, 0, maximum);
  const size_t lower =
      std::min<size_t>(static_cast<size_t>(value / slices), times.size() - 1);
  if (source_index) *source_index = lower;
  const int remainder = value % slices;
  if (remainder == 0 || lower + 1 >= times.size()) return times[lower];
  wxDateTime first;
  wxDateTime second;
  if (!ParseGribTime(times[lower], &first) ||
      !ParseGribTime(times[lower + 1], &second))
    return times[lower];
  const wxLongLong delta = second.GetTicks() - first.GetTicks();
  const wxLongLong interpolated = first.GetTicks() + delta * remainder / slices;
  wxDateTime result(static_cast<time_t>(interpolated.GetValue()));
  return result.ToUTC().Format("%Y%m%dT%H%MZ");
}

void PortableEnvironmentHost::Impl::StartFrameTime(const wxString& time,
                                                   int slider_value,
                                                   size_t source_index) {
  if (time.empty() || source_index >= times.size()) return;
  timeline->SetSelection(static_cast<int>(source_index));
  if (time_slider) time_slider->SetValue(slider_value);
  if (process) {
    if (operation == Operation::PrefetchFrame) {
      queued_frame_time = time;
      queued_frame_slider = slider_value;
      queued_frame_source = source_index;
    }
    return;
  }
  DecodedEnvironmentFrame cached;
  bool cache_hit = false;
  {
    std::lock_guard<std::mutex> decode_lock(routing_decode_mutex);
    cache_hit = FindCachedFrame(selected_file, time, &cached);
  }
  if (cache_hit) {
    ApplyDisplayedFrame(time, cached, 0, true);
    CallAfter([this, slider_value] {
      if (!stopped) PrefetchNextFrame(slider_value);
    });
    return;
  }
  pending_frame_time = time;
  const wxString result = MakeResultPath(private_directory, "frame");
  Launch(DecoderCommand("frame", selected_file, time, result),
         Operation::DecodeFrame, result, "Decoding environmental frame…");
}

void PortableEnvironmentHost::Impl::PrefetchNextFrame(int slider_value) {
  if (process || selected_file.empty() || times.empty() || !time_slider) return;
  const int maximum = time_slider->GetMax();
  if (slider_value >= maximum) return;
  const int next_slider = slider_value + 1;
  size_t source_index = 0;
  const wxString time = TimeForSlider(next_slider, &source_index);
  if (time.empty()) return;
  DecodedEnvironmentFrame cached;
  {
    std::lock_guard<std::mutex> decode_lock(routing_decode_mutex);
    if (FindCachedFrame(selected_file, time, &cached)) return;
  }
  pending_prefetch_time = time;
  const wxString result = MakeResultPath(private_directory, "prefetch-frame");
  Launch(DecoderCommand("frame", selected_file, time, result),
         Operation::PrefetchFrame, result, wxEmptyString);
}

void PortableEnvironmentHost::Impl::SetBusy(bool busy, const wxString& status) {
  if (!frame) return;
  open_button->Enable(!busy);
  generate_button->Enable(!busy);
  timeline->Enable(!busy);
  if (time_slider) time_slider->Enable(!busy);
  if (level_choice) level_choice->Enable(!busy);
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

void PortableEnvironmentHost::Impl::OnProgressTimer(wxTimerEvent&) {
  if (process && progress) progress->Pulse();
}

void PortableEnvironmentHost::Impl::OnPlaybackTimer(wxTimerEvent&) {
  if (process || times.empty() || !timeline) return;
  const int slices = interpolate_timeline ? interpolation_slices : 1;
  const int maximum = static_cast<int>(times.size() - 1) * slices;
  const int selected = time_slider ? time_slider->GetValue() : 0;
  if (!loop_playback && selected >= maximum) {
    playback_timer.Stop();
    if (play_button) {
      play_button->SetLabel(
          SurfaceControlLabel(surface_definition, "play", "Play"));
      ApplySurfaceButtonPresentation(play_button, surface_definition, "play",
                                     package_root);
    }
    return;
  }
  int next = selected + 1;
  if (next > maximum) {
    next = static_cast<int>(
        std::lround(maximum * static_cast<double>(loop_start_percent) / 100.0));
  }
  size_t source_index = 0;
  StartFrameTime(TimeForSlider(next, &source_index), next, source_index);
}

void PortableEnvironmentHost::Impl::UpdateAnimationTimer() {
  const bool animate = std::any_of(
      field_groups.begin(), field_groups.end(), [](const auto& group) {
        return group.display.particles && group.visible &&
               group.visible->GetValue();
      });
  if (animate && !animation_timer.IsRunning())
    animation_timer.Start(120);
  else if (!animate)
    animation_timer.Stop();
}

void PortableEnvironmentHost::Impl::OnAnimationTimer(wxTimerEvent&) {
  if (top_frame::Get()) top_frame::Get()->RefreshAllCanvas(false);
}

void PortableEnvironmentHost::Impl::Cancel() {
  if (!process || process_id <= 0) return;
  wxProcess::Kill(static_cast<int>(process_id), wxSIGTERM, wxKILL_CHILDREN);
  data_status->SetLabel("Cancellation requested…");
}

void PortableEnvironmentHost::Impl::OnProcessEnded(wxProcessEvent& event) {
  const Operation completed = operation;
  const wxString completed_result = result_path;
  operation = Operation::None;
  result_path.clear();
  process_id = 0;
  delete process;
  process = nullptr;
#if defined(_WIN32)
  if (windows_helper_job) {
    CloseHandle(windows_helper_job);
    windows_helper_job = nullptr;
  }
#endif
  SetBusy(false, wxEmptyString);
  if (stopped) return;

  wxJSONValue value;
  wxString error;
  if (!ReadJson(completed_result, &value, &error)) {
    if (completed == Operation::Inspect) {
      pending_file.clear();
      pending_display_name.clear();
    }
    if (completed == Operation::PrefetchFrame) {
      pending_prefetch_time.clear();
      wxLogWarning("Environmental frame prefetch failed (exit %d): %s",
                   event.GetExitCode(), error);
    } else {
      data_status->SetLabel("Failed: " + error);
      wxLogError("Portable environmental helper failed (exit %d): %s",
                 event.GetExitCode(), error);
    }
    wxRemoveFile(completed_result);
    if (completed == Operation::PrefetchFrame && !queued_frame_time.empty()) {
      const wxString queued_time = queued_frame_time;
      const int queued_slider = queued_frame_slider;
      const size_t queued_source = queued_frame_source;
      queued_frame_time.clear();
      CallAfter([this, queued_time, queued_slider, queued_source] {
        if (!stopped)
          StartFrameTime(queued_time, queued_slider, queued_source);
      });
    }
    return;
  }
  wxRemoveFile(completed_result);
  if (event.GetExitCode() != 0) {
    if (completed == Operation::Inspect) {
      pending_file.clear();
      pending_display_name.clear();
    }
    if (completed == Operation::PrefetchFrame) {
      pending_prefetch_time.clear();
      wxLogWarning("Environmental frame prefetch exited with status %d",
                   event.GetExitCode());
    } else {
      data_status->SetLabel(
          wxString::Format("Helper exited with status %d", event.GetExitCode()));
    }
    if (completed == Operation::PrefetchFrame && !queued_frame_time.empty()) {
      const wxString queued_time = queued_frame_time;
      const int queued_slider = queued_frame_slider;
      const size_t queued_source = queued_frame_source;
      queued_frame_time.clear();
      CallAfter([this, queued_time, queued_slider, queued_source] {
        if (!stopped)
          StartFrameTime(queued_time, queued_slider, queued_source);
      });
    }
    return;
  }
  switch (completed) {
    case Operation::Inspect:
      HandleInspect(value);
      break;
    case Operation::DecodeFrame:
      HandleFrame(value);
      break;
    case Operation::PrefetchFrame:
      HandlePrefetchFrame(value);
      break;
    case Operation::WeatherTable:
      HandleWeatherTable(value);
      break;
    case Operation::Generate:
      HandleGenerate(value);
      break;
    default:
      break;
  }
  if (completed == Operation::PrefetchFrame && !queued_frame_time.empty()) {
    const wxString queued_time = queued_frame_time;
    const int queued_slider = queued_frame_slider;
    const size_t queued_source = queued_frame_source;
    queued_frame_time.clear();
    CallAfter([this, queued_time, queued_slider, queued_source] {
      if (!stopped) StartFrameTime(queued_time, queued_slider, queued_source);
    });
  }
}

void PortableEnvironmentHost::Impl::HandleInspect(wxJSONValue& value) {
  if (!value["times"].IsArray() || !value["fields"].IsArray()) {
    data_status->SetLabel("Decoder returned an incompatible metadata schema");
    pending_file.clear();
    pending_display_name.clear();
    return;
  }
  {
    std::lock_guard<std::mutex> decode_lock(routing_decode_mutex);
    std::lock_guard<std::mutex> field_lock(field_mutex);
    routing_frames.clear();
    routing_frame_lru.clear();
    routing_frame_bytes.clear();
    routing_frame_cache_bytes = 0;
    fields.clear();
    field_units.clear();
    field_source_times.clear();
    times.clear();
    displayed_time.clear();
    selected_file = pending_file;
    selected_display_name = pending_display_name;
    ++dataset_revision;
  }
  pending_file.clear();
  pending_display_name.clear();
  std::vector<wxString> decoded_times;
  timeline->Clear();
  for (int i = 0; i < value["times"].Size(); ++i) {
    if (!value["times"][i].IsString()) continue;
    decoded_times.push_back(value["times"][i].AsString());
    timeline->Append(FormatGribTime(decoded_times.back()));
  }
  if (time_slider) {
    const int slices = interpolate_timeline ? interpolation_slices : 1;
    time_slider->SetRange(0,
                          std::max<int>(0, decoded_times.size() - 1) * slices);
    time_slider->SetValue(0);
  }
  {
    std::lock_guard<std::mutex> decode_lock(routing_decode_mutex);
    std::lock_guard<std::mutex> field_lock(field_mutex);
    times = decoded_times;
  }
  file_label->SetLabel("File: " + selected_display_name);
  const wxString opened_directory = wxFileName(selected_file).GetPath();
  if (wxDirExists(opened_directory) && opened_directory != private_directory &&
      opened_directory != last_grib_directory) {
    last_grib_directory = opened_directory;
    SaveSettings();
  }
  data_status->SetLabel(wxString::Format(
      "%d GRIB messages; %zu forecast times; decoded out of process",
      value["messageCount"].AsInt(), decoded_times.size()));
  wxString controller_error;
  if (!NotifySurfaceEvent(
          "dataset",
          wxString::Format("{\"revision\":%llu,\"forecastTimes\":%zu}",
                           static_cast<unsigned long long>(dataset_revision),
                           decoded_times.size()),
          &controller_error))
    wxLogWarning("Portable environmental controller rejected dataset: %s",
                 controller_error);
  if (!decoded_times.empty()) StartFrame(0);
}

void PortableEnvironmentHost::Impl::HandleFrame(wxJSONValue& value) {
  DecodedEnvironmentFrame decoded;
  wxString error;
  if (!ParseDecodedFrame(value, &decoded, &error)) {
    data_status->SetLabel(error);
    return;
  }
  const wxString requested_time = value["time"].AsString();
  ApplyDisplayedFrame(requested_time.empty() ? pending_frame_time
                                             : requested_time,
                      decoded, value["sampleCount"].AsInt(), false);
  const int slider_value = time_slider ? time_slider->GetValue() : 0;
  CallAfter([this, slider_value] {
    if (!stopped) PrefetchNextFrame(slider_value);
  });
}

void PortableEnvironmentHost::Impl::ApplyDisplayedFrame(
    const wxString& requested_time, const DecodedEnvironmentFrame& decoded,
    size_t sample_count, bool cached) {
  std::lock_guard<std::mutex> decode_lock(routing_decode_mutex);
  std::lock_guard<std::mutex> lock(field_mutex);
  fields = decoded.fields;
  field_units = decoded.units;
  field_source_times = decoded.source_times;
  displayed_time = requested_time;
  pending_frame_time.clear();
  CacheRoutingFrame(selected_file, requested_time, decoded);
  wxString source_note;
  const auto wave_source = field_source_times.find("wave-height");
  if (wave_source != field_source_times.end() &&
      wave_source->second != requested_time)
    source_note = " · waves sampled at " + FormatGribTime(wave_source->second);
  data_status->SetLabel(
      wxString::Format(
          "%s — %zu samples retained by OpenCPN%s",
          FormatGribTime(requested_time),
          sample_count ? sample_count :
                         [&decoded] {
                           size_t total = 0;
                           for (const auto& [name, samples] : decoded.fields)
                             total += samples.size();
                           return total;
                         }(),
          cached ? " (cached)" : "") +
      source_note);
  UpdateCursorStatus();
  if (top_frame::Get()) top_frame::Get()->RefreshAllCanvas(false);
}

void PortableEnvironmentHost::Impl::HandlePrefetchFrame(wxJSONValue& value) {
  DecodedEnvironmentFrame decoded;
  wxString error;
  if (!ParseDecodedFrame(value, &decoded, &error)) {
    wxLogWarning("Environmental frame prefetch was discarded: %s", error);
    pending_prefetch_time.clear();
    return;
  }
  const wxString requested_time = value["time"].IsString()
                                      ? value["time"].AsString()
                                      : pending_prefetch_time;
  {
    std::lock_guard<std::mutex> decode_lock(routing_decode_mutex);
    CacheRoutingFrame(selected_file, requested_time, decoded);
  }
  pending_prefetch_time.clear();
}

void PortableEnvironmentHost::Impl::HandleWeatherTable(wxJSONValue& value) {
  if (!value["rows"].IsArray()) {
    data_status->SetLabel("Decoder returned an incompatible weather table");
    return;
  }
  wxDialog dialog(frame, wxID_ANY, surface_title + " — weather table",
                  wxDefaultPosition, wxSize(1180, 600),
                  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* root = new wxBoxSizer(wxVERTICAL);
  auto* position = new wxStaticText(
      &dialog, wxID_ANY,
      wxString::Format("Nearest model samples to %.4f°, %.4f° · model output; "
                       "not navigation-authoritative",
                       value["latitude"].AsDouble(),
                       value["longitude"].AsDouble()));
  root->Add(position, 0, wxEXPAND | wxALL, 8);
  auto* table =
      new wxListCtrl(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxLC_REPORT | wxLC_HRULES | wxLC_VRULES);
  table->InsertColumn(0, "Forecast time", wxLIST_FORMAT_LEFT, 190);
  for (size_t column = 0; column < field_groups.size(); ++column)
    table->InsertColumn(static_cast<int>(column + 1),
                        field_groups[column].label, wxLIST_FORMAT_RIGHT, 120);
  const bool marine_position = IsMarinePoint(value["latitude"].AsDouble(),
                                             value["longitude"].AsDouble());
  for (int row_index = 0; row_index < value["rows"].Size(); ++row_index) {
    wxJSONValue row = value["rows"][row_index];
    wxJSONValue row_fields = row["fields"];
    const long item =
        table->InsertItem(row_index, FormatGribTime(row["time"].AsString()));
    auto read = [&](const wxString& id, double* output,
                    wxString* unit = nullptr) {
      if (!row_fields.IsObject() || !row_fields.HasMember(id) ||
          !(row_fields[id]["value"].IsDouble() ||
            row_fields[id]["value"].IsInt() ||
            row_fields[id]["value"].IsUInt()))
        return false;
      *output = row_fields[id]["value"].AsDouble();
      if (unit) *unit = row_fields[id]["unit"].AsString();
      return std::isfinite(*output);
    };
    for (size_t column = 0; column < field_groups.size(); ++column) {
      const auto& group = field_groups[column];
      wxString text = "N/A";
      if (!group.marine || marine_position) {
        double first = 0.0;
        double second = 0.0;
        wxString unit;
        if (group.kind == "vector" && group.components.size() >= 2 &&
            read(ActiveFieldId(group.id, group.components[0]), &first) &&
            read(ActiveFieldId(group.id, group.components[1]), &second)) {
          const double speed = std::hypot(first, second);
          text = wxString::Format("%.1f kt", speed * 1.94384449);
        } else {
          wxString id = ActiveFieldId(group.id);
          if (group.id == "wave") id = "wave-height";
          if (read(id, &first, &unit)) {
            if (group.id == "pressure")
              text = wxString::Format("%.1f hPa", PressureHpa(first));
            else if (group.id == "air-temperature" ||
                     group.id == "sea-temperature")
              text = wxString::Format("%.1f °C", TemperatureCelsius(first));
            else if (group.id == "wind-gust")
              text = wxString::Format("%.1f kt", first * 1.94384449);
            else
              text = wxString::Format("%.2f", first) +
                     (unit.empty() ? wxString() : wxString(" ") + unit);
          }
        }
      }
      table->SetItem(item, static_cast<int>(column + 1), text);
    }
  }
  root->Add(table, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
  root->Add(dialog.CreateSeparatedButtonSizer(wxCLOSE), 0, wxEXPAND | wxALL, 8);
  dialog.SetSizer(root);
  dialog.SetMinSize(wxSize(760, 420));
  dialog.ShowModal();
}

void PortableEnvironmentHost::Impl::CacheRoutingFrame(
    const wxString& source, const wxString& time,
    const DecodedEnvironmentFrame& decoded) const {
  if (source.empty() || time.empty()) return;
  const wxString key = FrameCacheKey(source, time);
  const auto old_bytes = routing_frame_bytes.find(key);
  if (old_bytes != routing_frame_bytes.end())
    routing_frame_cache_bytes -= old_bytes->second;
  routing_frames[key] = decoded;
  const size_t bytes = EstimatedFrameBytes(decoded);
  routing_frame_bytes[key] = bytes;
  routing_frame_cache_bytes += bytes;
  routing_frame_lru.remove(key);
  routing_frame_lru.push_front(key);
  TrimFrameCache();
}

wxString PortableEnvironmentHost::Impl::FrameCacheKey(
    const wxString& source, const wxString& time) const {
  return source + "\n" + time + (high_definition ? "\nhd" : "\nnormal");
}

bool PortableEnvironmentHost::Impl::FindCachedFrame(
    const wxString& source, const wxString& time,
    DecodedEnvironmentFrame* decoded) const {
  if (!decoded || source.empty() || time.empty()) return false;
  const wxString key = FrameCacheKey(source, time);
  const auto found = routing_frames.find(key);
  if (found == routing_frames.end()) return false;
  *decoded = found->second;
  routing_frame_lru.remove(key);
  routing_frame_lru.push_front(key);
  return true;
}

void PortableEnvironmentHost::Impl::TrimFrameCache() const {
  // A frame larger than the configured budget is still useful and is never
  // rejected.  It becomes the sole cached frame; the byte budget governs
  // retention only, not the geographic extent a plugin may open or sample.
  while (routing_frame_lru.size() > 1 &&
         routing_frame_cache_bytes > frame_cache_budget_bytes) {
    const wxString key = routing_frame_lru.back();
    const auto bytes = routing_frame_bytes.find(key);
    if (bytes != routing_frame_bytes.end()) {
      routing_frame_cache_bytes -= bytes->second;
      routing_frame_bytes.erase(bytes);
    }
    routing_frames.erase(key);
    routing_frame_lru.pop_back();
  }
}

bool PortableEnvironmentHost::Impl::IsMarinePoint(double latitude,
                                                  double longitude) const {
  if (!std::isfinite(latitude) || !std::isfinite(longitude)) return false;
  constexpr double kMaskPrecision = 100000.0;
  const auto key = std::make_pair(
      static_cast<int32_t>(std::lround(latitude * kMaskPrecision)),
      static_cast<int32_t>(std::lround(longitude * kMaskPrecision)));
  std::lock_guard<std::mutex> lock(land_mask_mutex);
  const auto cached = land_mask_cache.find(key);
  if (cached != land_mask_cache.end()) return !cached->second;
  const bool land = shapefileBasemapIsLand(latitude, longitude);
  land_mask_cache.emplace(key, land);
  return !land;
}

bool PortableEnvironmentHost::Impl::DecodeRoutingFrame(
    const wxString& source, const wxString& time,
    DecodedEnvironmentFrame* decoded, wxString* error) const {
  const wxString result = MakeResultPath(private_directory, "routing-frame");
  const auto arguments = DecoderCommand("frame", source, time, result);
  if (arguments.empty()) {
    HelperSupervisionAvailable(error);
    return false;
  }
  long exit_code = -1;
  if (!RunHeadlessProcess(arguments, &exit_code, error)) return false;
  wxJSONValue value;
  const bool read = ReadJson(result, &value, error);
  if (wxFileExists(result)) wxRemoveFile(result);
  if (exit_code != 0) {
    if (error && error->empty())
      *error = wxString::Format("supervised decoder exited with status %ld",
                                exit_code);
    return false;
  }
  return read && ParseDecodedFrame(value, decoded, error);
}

void PortableEnvironmentHost::Impl::HandleGenerate(wxJSONValue& value) {
  const wxString status = value["status"].AsString();
  const wxString output = generated_output_path;
  generated_output_path.clear();
  if (status == "complete") {
    data_status->SetLabel("Environmental GRIB generated successfully");
    if (!output.empty() && wxFileExists(output)) {
      wxArrayString source;
      source.Add(output);
      wxString staged;
      wxString stage_error;
      const wxString display_name = wxFileName(output).GetFullName();
      if (StageDataset(source, display_name, &staged, &stage_error) &&
          generated_open_after) {
        // Inspection is a separate ecCodes process from the generator. The
        // generated file becomes current only after that independent decoder
        // accepts the immutable snapshot.
        StartInspect(staged, display_name);
      } else if (!stage_error.empty()) {
        data_status->SetLabel("Generation output could not be staged: " +
                              stage_error);
      }
    }
  } else {
    wxString message = value["error"]["message"].AsString();
    if (message.empty()) message = "generator returned an incomplete result";
    data_status->SetLabel("Generation failed: " + message);
  }
}

void PortableEnvironmentHost::Impl::ShowGenerator() {
  if (process) return;
  if (!wxFileExists(GeneratorHelper())) {
    wxMessageBox("This package has no environmental generator helper for " +
                     HostHelperDirectory(),
                 surface_title, wxOK | wxICON_WARNING, frame);
    return;
  }

  wxJSONValue generator_definition = surface_definition["generator"];
  wxJSONValue credential_catalog = surface_definition["credential_catalog"];
  const wxArrayString credential_names = credential_catalog.GetMemberNames();
  const wxString credential_scheme =
      credential_names.empty() ? wxString() : credential_names[0];
  wxJSONValue credential_definition =
      credential_scheme.empty() ? wxJSONValue()
                                : credential_catalog[credential_scheme];
  const wxString credential_service =
      "OpenCPN portable " + plugin_id + " " +
      credential_definition["service_suffix"].AsString();
  wxString secret_store_error;
  bool secret_store_available = false;
  wxString stored_username;
  wxSecretValue stored_password;
  bool have_stored_credentials = false;
#if wxUSE_SECRETSTORE
  wxSecretStore secret_store = wxSecretStore::GetDefault();
  secret_store_available =
      credential_access && secret_store.IsOk(&secret_store_error);
  have_stored_credentials =
      secret_store_available && !credential_scheme.empty() &&
      secret_store.Load(credential_service, stored_username, stored_password);
#else
  secret_store_error =
      "this wxWidgets build has no operating-system secret-store support; "
      "the login will be held in memory for this generation only";
#endif

  const wxString generator_title =
      generator_definition["title"].IsString()
          ? generator_definition["title"].AsString()
          : "Environmental GRIB Generator";
  wxDialog dialog(frame, wxID_ANY, generator_title, wxDefaultPosition,
                  wxSize(820, 760), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
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
  const auto area_presets = SurfaceAreaPresets(surface_definition);
  wxArrayString area_labels;
  for (const auto& item : area_presets) area_labels.Add(item.label);
  auto* area_preset = new wxChoice(form, wxID_ANY, wxDefaultPosition,
                                   wxDefaultSize, area_labels);
  area_preset->SetSelection(0);
  auto* start = new wxTextCtrl(
      form, wxID_ANY, wxDateTime::Now().ToUTC().Format("%Y-%m-%dT%H:00:00Z"));
  const int maximum_hours =
      std::clamp(generator_definition["maximum_hours"].AsInt(), 1, 24 * 31);
  auto* hours = new wxSpinCtrl(form, wxID_ANY, "72", wxDefaultPosition,
                               wxDefaultSize, wxSP_ARROW_KEYS, 1, maximum_hours,
                               std::min(72, maximum_hours));
  const int default_step =
      std::clamp(generator_definition["default_step_hours"].AsInt(), 1, 24);
  auto* step = new wxSpinCtrl(form, wxID_ANY,
                              wxString::Format("%d", default_step),
                              wxDefaultPosition, wxDefaultSize,
                              wxSP_ARROW_KEYS, 1, 24, default_step);
  const auto weather_providers =
      SurfaceProviderOptions(surface_definition, "weather");
  const auto wave_providers =
      SurfaceProviderOptions(surface_definition, "waves");
  const auto current_sources =
      SurfaceProviderOptions(surface_definition, "current");
  const auto fallback_weather_providers =
      FallbackProviderOptions(weather_providers);
  const auto fallback_wave_providers = FallbackProviderOptions(wave_providers);
  const auto fallback_current_sources =
      FallbackProviderOptions(current_sources);
  auto* provider =
      new wxChoice(form, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   ProviderLabels(weather_providers));
  provider->SetSelection(DefaultProviderIndex(weather_providers));
  auto* include_weather =
      new wxCheckBox(form, wxID_ANY, "Generate/include weather");
  include_weather->SetValue(true);
  const auto weather_presets = SurfaceWeatherPresets(surface_definition);
  wxArrayString presets;
  int default_weather_preset = 0;
  for (size_t index = 0; index < weather_presets.size(); ++index) {
    presets.Add(weather_presets[index].label);
    if (weather_presets[index].selected_by_default)
      default_weather_preset = static_cast<int>(index);
  }
  auto* preset =
      new wxChoice(form, wxID_ANY, wxDefaultPosition, wxDefaultSize, presets);
  preset->SetSelection(default_weather_preset);
  auto* include_waves = new wxCheckBox(form, wxID_ANY, "Include wave fields");
  include_waves->SetValue(true);
  auto* wave_provider =
      new wxChoice(form, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   ProviderLabels(wave_providers));
  wave_provider->SetSelection(DefaultProviderIndex(wave_providers));
  auto* weather_note = new wxStaticText(form, wxID_ANY, wxEmptyString);
  auto* wave_note = new wxStaticText(form, wxID_ANY, wxEmptyString);
  weather_note->Wrap(520);
  wave_note->Wrap(520);
  auto* extend_forecast =
      new wxCheckBox(form, wxID_ANY, "Use selected long-range fallbacks");
  auto* fallback_weather =
      new wxChoice(form, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   ProviderLabels(fallback_weather_providers));
  fallback_weather->SetSelection(
      DefaultProviderIndex(fallback_weather_providers));
  auto* fallback_waves =
      new wxChoice(form, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   ProviderLabels(fallback_wave_providers));
  fallback_waves->SetSelection(DefaultProviderIndex(fallback_wave_providers));
  auto* fallback_current =
      new wxChoice(form, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   ProviderLabels(fallback_current_sources));
  fallback_current->SetSelection(
      DefaultProviderIndex(fallback_current_sources));
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
  auto* current_source =
      new wxChoice(form, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   ProviderLabels(current_sources));
  current_source->SetSelection(DefaultProviderIndex(current_sources));
  auto* include_current =
      new wxCheckBox(form, wxID_ANY, "Generate/include currents");
  include_current->SetValue(true);
  auto* current_note = new wxStaticText(form, wxID_ANY, wxEmptyString);
  current_note->Wrap(520);
  wxTextCtrl* local_current = nullptr;
  wxButton* browse_current = nullptr;
  auto* current_input_panel =
      make_input_picker(&local_current, &browse_current);
  wxTextCtrl* fallback_current_data = nullptr;
  wxButton* browse_fallback_current = nullptr;
  auto* fallback_current_input_panel =
      make_input_picker(&fallback_current_data, &browse_fallback_current);
  auto* provider_mode =
      new wxChoice(form, wxID_ANY, wxDefaultPosition, wxDefaultSize);
  auto* account = new wxHyperlinkCtrl(
      form, wxID_ANY,
      credential_definition["account_label"].IsString()
          ? credential_definition["account_label"].AsString()
          : "Open provider account page",
      credential_definition["account_url"].AsString());
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
  const wxString default_output_name = wxDateTime::Now().ToUTC().Format(
      generator_definition["output_basename"].AsString() + "_%Y%m%d_%H%M.grb");
  auto* output_directory_panel = new wxPanel(form);
  auto* output_directory_sizer = new wxBoxSizer(wxHORIZONTAL);
  auto* output_directory =
      new wxTextCtrl(output_directory_panel, wxID_ANY, default_output_directory);
  auto* browse_output_directory =
      new wxButton(output_directory_panel, wxID_ANY, "Browse…");
  output_directory_sizer->Add(output_directory, 1, wxEXPAND | wxRIGHT, 6);
  output_directory_sizer->Add(browse_output_directory, 0, wxEXPAND);
  output_directory_panel->SetSizer(output_directory_sizer);
  auto* output_name_panel = new wxPanel(form);
  auto* output_name_sizer = new wxBoxSizer(wxHORIZONTAL);
  auto* output_name =
      new wxTextCtrl(output_name_panel, wxID_ANY, default_output_name);
  auto* browse_output = new wxButton(output_name_panel, wxID_ANY, "Browse…");
  output_name_sizer->Add(output_name, 1, wxEXPAND | wxRIGHT, 6);
  output_name_sizer->Add(browse_output, 0, wxEXPAND);
  output_name_panel->SetSizer(output_name_sizer);
  auto* open_after = new wxCheckBox(
      form, wxID_ANY, "Open generated GRIB after creation");
  open_after->SetValue(true);
  AddRow(grid, form, "Generator executable", executable);
  AddRow(grid, form, "West longitude", west);
  AddRow(grid, form, "South latitude", south);
  AddRow(grid, form, "East longitude", east);
  AddRow(grid, form, "North latitude", north);
  AddRow(grid, form, "Area preset", area_preset);
  AddRow(grid, form, "Start UTC", start);
  AddRow(
      grid, form,
      wxString::Format("Forecast duration hours (maximum %d)", maximum_hours),
      hours);
  AddRow(grid, form, "Step hours", step);
  AddRow(grid, form, "Forecast extension", extend_forecast);
  AddRow(grid, form, "Weather", include_weather);
  AddRow(grid, form, "Weather provider", provider);
  AddRow(grid, form, "Weather-source details", weather_note);
  AddRow(grid, form, "Weather preset", preset);
  AddRow(grid, form, "Waves", include_waves);
  AddRow(grid, form, "Wave provider", wave_provider);
  AddRow(grid, form, "Wave-source details", wave_note);
  AddRow(grid, form, "Weather GRIB file", weather_input_panel);
  AddRow(grid, form, "Currents", include_current);
  AddRow(grid, form, "Current source", current_source);
  AddRow(grid, form, "Current-source details", current_note);
  AddRow(grid, form, "Current source data", current_input_panel);
  AddRow(grid, form, "Provider mode", provider_mode);
  AddRow(grid, form, "Weather fallback", fallback_weather);
  AddRow(grid, form, "Wave fallback", fallback_waves);
  AddRow(grid, form, "Current fallback", fallback_current);
  AddRow(grid, form, "Current fallback data", fallback_current_input_panel);
  AddRow(grid, form, credential_definition["label"].AsString() + " account",
         account);
  AddRow(grid, form, credential_definition["username_label"].AsString(),
         username);
  AddRow(grid, form, credential_definition["secret_label"].AsString(),
         password);
  AddRow(grid, form, "Credential storage", remember);
  AddRow(grid, form, "Saved credentials", forget);
  AddRow(grid, form, "Output directory", output_directory_panel);
  AddRow(grid, form, "Output filename", output_name_panel);
  AddRow(grid, form, "Generated file", open_after);
  form->SetSizer(grid);
  form->SetScrollRate(0, 12);
  root->Add(form, 1, wxEXPAND | wxALL, 10);
  root->Add(new wxStaticText(
                &dialog, wxID_ANY,
                generator_definition["safety_notice"].IsString()
                    ? generator_definition["safety_notice"].AsString()
                    : "Generated environmental data is model output; it is "
                      "not an official navigation product."),
            0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
  auto* buttons = dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL);
  if (auto* generate = wxDynamicCast(dialog.FindWindow(wxID_OK), wxButton))
    generate->SetLabel("Generate GRIB");
  root->Add(buttons, 0, wxEXPAND | wxALL, 10);
  dialog.SetSizer(root);

  auto selected_provider =
      [](const wxChoice* choice,
         const auto& providers) -> const EnvironmentalProviderOption& {
    return providers.at(static_cast<size_t>(choice->GetSelection()));
  };
  auto provider_details = [](const EnvironmentalProviderOption& selected) {
    wxString details = selected.status;
    if (!selected.coverage.empty())
      details += "; coverage: " + selected.coverage;
    if (!selected.requirements.empty())
      details += "; requires: " + selected.requirements;
    if (!selected.limitations.empty())
      details += "; limitations: " + selected.limitations;
    if (!selected.credential.empty() && selected.credential != "none" &&
        selected.credential != "file" && selected.credential != "directory" &&
        selected.credential != "conditional")
      details += "; requires a " + selected.credential + " account";
    if (selected.input_kind == "file")
      details += "; select the required local source file";
    else if (selected.input_kind == "directory")
      details += "; select the local source directory";
    return details;
  };
  auto credential_selected = [&]() {
    return !credential_scheme.empty() &&
           ((include_waves->GetValue() &&
             selected_provider(wave_provider, wave_providers).credential ==
                 credential_scheme) ||
            (include_current->GetValue() &&
             (selected_provider(current_source, current_sources).credential ==
                  credential_scheme ||
              selected_provider(current_source, current_sources).credential ==
                  "conditional")));
  };
  auto update_credential_controls = [&]() {
    const bool selected = credential_access && credential_selected();
    account->Enable(selected);
    username->Enable(selected);
    password->Enable(selected);
    remember->Enable(selected && secret_store_available);
    forget->Enable(selected && have_stored_credentials);
  };
  auto update_weather_controls = [&]() {
    const auto& selected = selected_provider(provider, weather_providers);
    const bool enabled = include_weather->GetValue();
    const bool local = enabled && !selected.input_kind.empty();
    provider->Enable(enabled);
    local_weather->Enable(local);
    browse_weather->Enable(local);
    preset->Enable(enabled && !local && !selected.disabled);
    include_waves->Enable(enabled && !local);
    wave_provider->Enable(enabled && include_waves->GetValue() && !local);
    if (local)
      wave_provider->SetToolTip(
          "Wave records already present in the local file are preserved");
    else
      wave_provider->UnsetToolTip();
    weather_note->SetLabel(provider_details(selected));
    weather_note->Wrap(520);
    update_credential_controls();
    dialog.Layout();
  };
  auto update_wave_controls = [&]() {
    const auto& selected = selected_provider(wave_provider, wave_providers);
    wave_provider->Enable(include_weather->GetValue() &&
                          include_waves->GetValue() &&
                          selected_provider(provider, weather_providers)
                              .input_kind.empty());
    wave_note->SetLabel(provider_details(selected));
    wave_note->Wrap(520);
    update_credential_controls();
    dialog.Layout();
  };
  auto update_current_controls = [&]() {
    const auto& selected = selected_provider(current_source, current_sources);
    const bool enabled = include_current->GetValue();
    const bool needs_source = enabled && !selected.input_kind.empty();
    current_source->Enable(enabled);
    local_current->Enable(needs_source);
    browse_current->Enable(needs_source);
    provider_mode->Clear();
    for (const auto& mode : selected.mode_options)
      provider_mode->Append(mode.first);
    if (!selected.mode_options.empty()) provider_mode->SetSelection(0);
    provider_mode->Enable(enabled && !selected.mode_options.empty());
    current_note->SetLabel(provider_details(selected));
    current_note->Wrap(520);
    update_credential_controls();
    dialog.Layout();
  };
  auto update_extension_controls = [&]() {
    const bool enabled = extend_forecast->GetValue();
    fallback_weather->Enable(enabled);
    fallback_waves->Enable(enabled);
    fallback_current->Enable(enabled);
    const bool needs_data =
        enabled && !fallback_current_sources.empty() &&
        !selected_provider(fallback_current, fallback_current_sources)
             .input_kind.empty() &&
        selected_provider(current_source, current_sources).id !=
            selected_provider(fallback_current, fallback_current_sources).id;
    fallback_current_data->Enable(needs_data);
    browse_fallback_current->Enable(needs_data);
  };
  bool applying_area_preset = false;
  area_preset->Bind(wxEVT_CHOICE, [&](wxCommandEvent&) {
    const int selection = area_preset->GetSelection();
    if (selection < 0 ||
        static_cast<size_t>(selection) >= area_presets.size())
      return;
    const auto& selected = area_presets[static_cast<size_t>(selection)];
    if (selected.kind == "custom") return;
    double preset_west = selected.west;
    double preset_south = selected.south;
    double preset_east = selected.east;
    double preset_north = selected.north;
    if (selected.kind == "current-view") {
      auto* canvas = top_frame::Get()
                         ? dynamic_cast<ChartCanvas*>(
                               top_frame::Get()->GetAbstractFocusCanvas())
                         : nullptr;
      if (!canvas || !canvas->GetVP().GetBBox().GetValid()) {
        wxMessageBox(
            "The current chart area is not available yet. Pan or zoom the "
            "chart, then try again.",
            "Current chart area unavailable", wxOK | wxICON_INFORMATION,
            &dialog);
        area_preset->SetSelection(0);
        return;
      }
      const auto& bbox = canvas->GetVP().GetBBox();
      preset_west = bbox.GetMinLon();
      preset_south = bbox.GetMinLat();
      preset_east = bbox.GetMaxLon();
      preset_north = bbox.GetMaxLat();
      if (!(preset_west < preset_east && preset_south < preset_north) ||
          preset_west <= -180.0 || preset_east >= 180.0) {
        wxMessageBox(
            "The current chart area crosses a longitude boundary which "
            "cannot be represented by one west/south/east/north box.",
            "Current chart area unavailable", wxOK | wxICON_INFORMATION,
            &dialog);
        area_preset->SetSelection(0);
        return;
      }
    }
    applying_area_preset = true;
    west->SetValue(wxString::Format("%.6f", preset_west));
    south->SetValue(wxString::Format("%.6f", preset_south));
    east->SetValue(wxString::Format("%.6f", preset_east));
    north->SetValue(wxString::Format("%.6f", preset_north));
    applying_area_preset = false;
    if (!selected.current_provider.empty()) {
      include_current->SetValue(true);
      current_source->SetSelection(ProviderIndex(
          current_sources, selected.current_provider,
          current_source->GetSelection() == wxNOT_FOUND
              ? 0
              : current_source->GetSelection()));
      update_current_controls();
    }
    if (selected.kind == "current-view") area_preset->SetSelection(0);
  });
  auto custom_bbox_changed = [&](wxCommandEvent&) {
    if (!applying_area_preset) area_preset->SetSelection(0);
  };
  west->Bind(wxEVT_TEXT, custom_bbox_changed);
  south->Bind(wxEVT_TEXT, custom_bbox_changed);
  east->Bind(wxEVT_TEXT, custom_bbox_changed);
  north->Bind(wxEVT_TEXT, custom_bbox_changed);
  current_source->Bind(wxEVT_CHOICE, [&](wxCommandEvent&) {
    update_current_controls();
    update_extension_controls();
  });
  provider->Bind(wxEVT_CHOICE,
                 [&](wxCommandEvent&) { update_weather_controls(); });
  include_weather->Bind(wxEVT_CHECKBOX, [&](wxCommandEvent&) {
    update_weather_controls();
    update_wave_controls();
  });
  wave_provider->Bind(wxEVT_CHOICE,
                      [&](wxCommandEvent&) { update_wave_controls(); });
  include_waves->Bind(wxEVT_CHECKBOX,
                      [&](wxCommandEvent&) { update_wave_controls(); });
  include_current->Bind(wxEVT_CHECKBOX,
                        [&](wxCommandEvent&) { update_current_controls(); });
  extend_forecast->Bind(wxEVT_CHECKBOX,
                        [&](wxCommandEvent&) { update_extension_controls(); });
  fallback_current->Bind(wxEVT_CHOICE,
                         [&](wxCommandEvent&) { update_extension_controls(); });
  auto bind_input_picker =
      [&](wxButton* button, wxTextCtrl* path, const wxString& title,
          const std::vector<EnvironmentalProviderOption>* options,
          const wxChoice* choice) {
        button->Bind(
            wxEVT_BUTTON, [&, path, title, options, choice](wxCommandEvent&) {
              const auto& selected =
                  options->at(static_cast<size_t>(choice->GetSelection()));
              wxFileDialog input_dialog(
                  &dialog, title, wxEmptyString, wxEmptyString,
                  selected.input_filter.empty() ? "Environmental data (*.*)|*.*"
                                                : selected.input_filter,
                  wxFD_OPEN | wxFD_FILE_MUST_EXIST);
              if (input_dialog.ShowModal() == wxID_OK)
                path->SetValue(input_dialog.GetPath());
            });
      };
  bind_input_picker(browse_weather, local_weather,
                    "Select a local weather GRIB to merge", &weather_providers,
                    provider);
  browse_current->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    const auto& selected = selected_provider(current_source, current_sources);
    if (selected.input_kind == "directory") {
      wxDirDialog input_dialog(&dialog,
                               selected.input_label.empty()
                                   ? "Select environmental source directory"
                                   : "Select " + selected.input_label);
      if (input_dialog.ShowModal() == wxID_OK)
        local_current->SetValue(input_dialog.GetPath());
      return;
    }
    wxFileDialog input_dialog(
        &dialog,
        selected.input_label.empty() ? "Select local environmental source"
                                     : "Select " + selected.input_label,
        wxEmptyString, wxEmptyString,
        selected.input_filter.empty() ? "Environmental data (*.*)|*.*"
                                      : selected.input_filter,
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (input_dialog.ShowModal() == wxID_OK)
      local_current->SetValue(input_dialog.GetPath());
  });
  browse_fallback_current->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    const auto& selected =
        selected_provider(fallback_current, fallback_current_sources);
    wxFileDialog input_dialog(
        &dialog,
        selected.input_label.empty() ? "Select fallback environmental data"
                                     : "Select " + selected.input_label,
        wxEmptyString, wxEmptyString,
        selected.input_filter.empty() ? "Environmental data (*.*)|*.*"
                                      : selected.input_filter,
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (input_dialog.ShowModal() == wxID_OK)
      fallback_current_data->SetValue(input_dialog.GetPath());
  });
  forget->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
#if wxUSE_SECRETSTORE
    if (have_stored_credentials && secret_store.Delete(credential_service)) {
      have_stored_credentials = false;
      stored_username.clear();
      username->Clear();
      password->Clear();
      remember->SetValue(false);
      forget->Enable(false);
    }
#else
    // The button is disabled when wxWidgets has no secret-store backend.
    wxUnusedVar(have_stored_credentials);
#endif
  });
  browse_output_directory->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    wxString directory = output_directory->GetValue();
    if (!wxDirExists(directory)) directory = wxGetHomeDir();
    wxDirDialog directory_dialog(&dialog, "Choose GRIB output directory",
                                 directory);
    if (directory_dialog.ShowModal() == wxID_OK)
      output_directory->SetValue(directory_dialog.GetPath());
  });
  browse_output->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    wxString directory = output_directory->GetValue();
    if (!wxDirExists(directory)) directory = wxGetHomeDir();
    wxFileDialog output_dialog(
        &dialog, "Choose where to save the generated environmental GRIB",
        directory, output_name->GetValue(),
        "GRIB files (*.grb)|*.grb|All files (*.*)|*.*",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (output_dialog.ShowModal() == wxID_OK) {
      output_directory->SetValue(output_dialog.GetDirectory());
      output_name->SetValue(output_dialog.GetFilename());
    }
  });
  update_weather_controls();
  update_wave_controls();
  update_current_controls();
  update_extension_controls();
  if (dialog.ShowModal() != wxID_OK) return;

  if (hours->GetValue() % step->GetValue() != 0) {
    wxMessageBox(
        "Forecast duration must be evenly divisible by the step interval. "
        "For example, use 1 hour with a 1-hour step or 72 hours with a "
        "3-hour step.",
        surface_title, wxOK | wxICON_ERROR, frame);
    return;
  }

  wxString requested_output_directory = output_directory->GetValue();
  requested_output_directory.Trim(true).Trim(false);
  wxString requested_output_name = output_name->GetValue();
  requested_output_name.Trim(true).Trim(false);
  wxFileName name_only(requested_output_name);
  if (requested_output_name.empty() || name_only.GetFullName() !=
                                           requested_output_name) {
    wxMessageBox("Output filename must be a filename, not a path",
                 surface_title, wxOK | wxICON_ERROR, frame);
    return;
  }
  wxString requested_output =
      wxFileName(requested_output_directory, requested_output_name)
          .GetFullPath();
  wxFileName output_filename(requested_output);
  if (requested_output.empty() || !output_filename.IsAbsolute()) {
    wxMessageBox("Choose an absolute output path for the generated GRIB",
                 surface_title, wxOK | wxICON_ERROR, frame);
    return;
  }
  if (output_filename.GetExt().empty()) output_filename.SetExt("grb");
  output_filename.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
  const wxString normalized_output_directory = output_filename.GetPath();
  if (!wxDirExists(normalized_output_directory) ||
      !wxFileName::IsDirWritable(normalized_output_directory)) {
    wxMessageBox(
        "The selected output directory does not exist or is not "
        "writable",
        surface_title, wxOK | wxICON_ERROR, frame);
    return;
  }
  if (output_filename.FileExists() &&
      wxMessageBox(
          "Replace the existing GRIB file?\n" + output_filename.GetFullPath(),
          surface_title, wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
          frame) != wxYES)
    return;

  auto validate_input = [&](wxTextCtrl* control, const wxString& label,
                            bool required, bool directory, wxString* value) {
    *value = control->GetValue();
    value->Trim(true).Trim(false);
    if (value->empty()) {
      if (required) {
        wxMessageBox("Choose " + label + " or select an online source",
                     surface_title, wxOK | wxICON_ERROR, frame);
        return false;
      }
      return true;
    }
    wxFileName file(*value);
    const bool exists = directory ? wxDirExists(*value) : file.FileExists();
    if (!file.IsAbsolute() || !exists) {
      wxMessageBox(label + (directory ? " must be an existing directory"
                                      : " must be an existing file"),
                   surface_title, wxOK | wxICON_ERROR, frame);
      return false;
    }
    file.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
    *value = file.GetFullPath();
    return true;
  };
  wxString weather_input;
  wxString current_input;
  wxString fallback_current_input;
  const auto& selected_weather = selected_provider(provider, weather_providers);
  const auto& selected_waves = selected_provider(wave_provider, wave_providers);
  const auto& selected_current =
      selected_provider(current_source, current_sources);
  const bool weather_enabled = include_weather->GetValue();
  const bool waves_enabled = weather_enabled && include_waves->GetValue();
  const bool current_enabled = include_current->GetValue();
  const bool local_weather_selected =
      weather_enabled && !selected_weather.input_kind.empty();
  const bool local_current_selected =
      current_enabled && !selected_current.input_kind.empty();
  if ((local_weather_selected &&
       !validate_input(local_weather, "a local weather GRIB", true,
                       selected_weather.input_kind == "directory",
                       &weather_input)) ||
      (local_current_selected &&
       !validate_input(local_current, "local current source data", true,
                       selected_current.input_kind == "directory",
                       &current_input)))
    return;
  const auto& selected_current_fallback =
      selected_provider(fallback_current, fallback_current_sources);
  if (extend_forecast->GetValue() &&
      !selected_current_fallback.input_kind.empty() &&
      selected_current_fallback.id != selected_current.id &&
      !validate_input(fallback_current_data, "offline tidal fallback data",
                      true, false, &fallback_current_input))
    return;

  const bool provider_needs_credentials = credential_selected();
  if (provider_needs_credentials && !credential_access) {
    wxMessageBox(
        "The selected source requires credentials, but this package "
        "was not granted credential access.",
        surface_title, wxOK | wxICON_ERROR, frame);
    return;
  }
  const bool use_provider_credentials =
      credential_access && provider_needs_credentials;
  wxString provider_username = username->GetValue();
  provider_username.Trim(true).Trim(false);
  wxSecretString provider_password(password->GetValue());
  wxSecretValue provider_secret;
  if (use_provider_credentials) {
    if (provider_username.empty() || provider_password.empty()) {
      wxMessageBox(
          "The selected environmental source requires a provider username and "
          "password.",
          surface_title, wxOK | wxICON_ERROR, frame);
      return;
    }
    provider_secret = wxSecretValue(provider_password);
#if wxUSE_SECRETSTORE
    if (remember->GetValue() &&
        !secret_store.Save(credential_service, provider_username,
                           provider_secret)) {
      wxMessageBox(
          "The provider login could not be saved securely. It will be "
          "used for this generation only.",
          surface_title, wxOK | wxICON_WARNING, frame);
    }
#endif
  }
  password->Clear();

  double west_value = 0, south_value = 0, east_value = 0, north_value = 0;
  if (!west->GetValue().ToDouble(&west_value) ||
      !south->GetValue().ToDouble(&south_value) ||
      !east->GetValue().ToDouble(&east_value) ||
      !north->GetValue().ToDouble(&north_value)) {
    wxMessageBox("Bounding coordinates must be numbers", surface_title,
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
  const bool sandboxed_generator = UsesFilesystemSandbox();
  request["weatherProvider"] =
      weather_enabled ? selected_weather.id : wxString("none");
  if (local_weather_selected && !selected_weather.input_request_key.empty())
    request[selected_weather.input_request_key] =
        sandboxed_generator ? wxString("/inputs/weather.grb") : weather_input;
  const int weather_preset_selection = preset->GetSelection();
  request["weatherPreset"] =
      weather_preset_selection >= 0 &&
              static_cast<size_t>(weather_preset_selection) <
                  weather_presets.size()
          ? weather_presets[static_cast<size_t>(weather_preset_selection)].id
          : wxString("routing");
  // A user-selected weather file is copied as one validated stream, including
  // any wave records it already contains.  Do not unexpectedly contact an
  // online wave provider while performing an otherwise local merge.
  request["includeWaves"] = waves_enabled && !local_weather_selected &&
                            !selected_waves.disabled;
  request["waveProvider"] =
      waves_enabled ? selected_waves.id : wxString("none");
  request["currentSource"] =
      current_enabled ? selected_current.id : wxString("none");
  if (local_current_selected && !selected_current.input_request_key.empty()) {
    request[selected_current.input_request_key] =
        sandboxed_generator ? wxString("/inputs/current-source")
                            : current_input;
  }
  if (!selected_current.mode_request_key.empty() &&
      provider_mode->GetSelection() != wxNOT_FOUND &&
      static_cast<size_t>(provider_mode->GetSelection()) <
          selected_current.mode_options.size()) {
    request[selected_current.mode_request_key] =
        selected_current
            .mode_options[static_cast<size_t>(provider_mode->GetSelection())]
            .second;
  }
  request["extendForecast"] = extend_forecast->GetValue();
  if (extend_forecast->GetValue()) {
    request["fallbackWeatherProvider"] =
        selected_provider(fallback_weather, fallback_weather_providers).id;
    request["fallbackWaveProvider"] =
        selected_provider(fallback_waves, fallback_wave_providers).id;
    request["fallbackCurrentSource"] =
        selected_provider(fallback_current, fallback_current_sources).id;
    if (!fallback_current_input.empty() &&
        !selected_current_fallback.input_request_key.empty())
      request[selected_current_fallback.input_request_key] =
          sandboxed_generator ? wxString("/inputs/fallback-current-source")
                              : fallback_current_input;
  }
  if (use_provider_credentials) {
    request[credential_definition["request_username_key"].AsString()] =
        provider_username;
    job["credentials"]
       [credential_definition["job_environment_key"].AsString()] =
           credential_environment;
  }
  request["output"] = sandboxed_generator
                          ? "/output/" + output_filename.GetFullName()
                          : output_filename.GetFullPath();
  request["overwrite"] = true;

  wxString controller_error;
  const wxString generation_state = wxString::Format(
      "{\"weatherProvider\":\"%s\",\"waveProvider\":\"%s\","
      "\"currentProvider\":\"%s\"}",
      weather_enabled ? selected_weather.id : wxString("none"),
      waves_enabled ? selected_waves.id : wxString("none"),
      current_enabled ? selected_current.id : wxString("none"));
  if (!NotifySurfaceEvent("generation-request", generation_state,
                          &controller_error)) {
    wxMessageBox("The portable package rejected the generation request: " +
                     controller_error,
                 surface_title, wxOK | wxICON_ERROR, frame);
    return;
  }

  const wxString job_path = MakeResultPath(private_directory, "generate-job");
  const wxString result = MakeResultPath(private_directory, "generate-result");
  wxFileOutputStream job_output(job_path);
  if (!job_output.IsOk()) {
    wxMessageBox("Could not create the private generator request",
                 surface_title, wxOK | wxICON_ERROR, frame);
    return;
  }
  wxJSONWriter writer(wxJSONWRITER_STYLED);
  writer.Write(job, job_output);
  job_output.Close();
  generated_output_path = output_filename.GetFullPath();
  generated_open_after = open_after->GetValue();
  Launch(GeneratorCommand(job_path, result, generated_output_path,
                          weather_input, current_input, fallback_current_input),
         Operation::Generate, result,
         "Generating environmental GRIB in supervised helper…",
         use_provider_credentials ? &provider_secret : nullptr);
}

void PortableEnvironmentHost::Impl::SetCursorPosition(double latitude,
                                                      double longitude) {
  if (!std::isfinite(latitude) || !std::isfinite(longitude)) return;
  cursor_latitude = latitude;
  cursor_longitude = longitude;
  have_cursor = true;
  UpdateCursorStatus();
}

void PortableEnvironmentHost::Impl::UpdateCursorStatus() {
  if (!cursor_status || !have_cursor || fields.empty()) return;
  auto nearest = [&](const wxString& kind, double* value,
                     bool marine_only = false) {
    const auto found = fields.find(kind);
    if (found == fields.end() || found->second.empty()) return false;
    const double longitude_scale =
        std::max(0.1, std::cos(cursor_latitude * kPi / 180.0));
    double best_distance = std::numeric_limits<double>::max();
    for (const auto& sample : found->second) {
      if (marine_only && !IsMarinePoint(sample.latitude, sample.longitude))
        continue;
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
  for (auto& group : field_groups)
    if (group.value) group.value->SetLabel("N/A");
  const bool marine_cursor = IsMarinePoint(cursor_latitude, cursor_longitude);
  double u = 0.0, v = 0.0, value = 0.0;
  if (nearest(ActiveFieldId("wind", "u"), &u) &&
      nearest(ActiveFieldId("wind", "v"), &v)) {
    double from = std::atan2(-u, -v) * 180.0 / kPi;
    if (from < 0.0) from += 360.0;
    wind_value->SetLabel(speed_value(std::hypot(u, v), wind_display.units) +
                         wxString::Format("  %03.0f° from", from));
  }
  if (nearest(ActiveFieldId("pressure"), &value)) {
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
  if (marine_cursor && nearest("wave-height", &value, true)) {
    wxString label = wave_display.units == 1
                         ? wxString::Format("%.1f ft", value * 3.2808399)
                         : wxString::Format("%.2f m", value);
    double period = 0.0;
    double direction = 0.0;
    if (nearest("wave-period", &period, true))
      label += wxString::Format("  %.1f s", period);
    if (nearest("wave-direction", &direction, true))
      label += wxString::Format("  %03.0f° from", direction);
    wave_value->SetLabel(label);
  }
  if (marine_cursor && nearest("current-u", &u, true) &&
      nearest("current-v", &v, true) && PlausibleCurrent(u, v)) {
    double toward = std::atan2(u, v) * 180.0 / kPi;
    if (toward < 0.0) toward += 360.0;
    current_value->SetLabel(
        speed_value(std::hypot(u, v), current_display.units) +
        wxString::Format("  %03.0f° toward", toward));
  }
  if (nearest(ActiveFieldId("air-temperature"), &value)) {
    const double celsius = TemperatureCelsius(value);
    temperature_value->SetLabel(
        temperature_display.units == 1
            ? wxString::Format("%.1f °F", celsius * 9.0 / 5.0 + 32.0)
            : wxString::Format("%.1f °C", celsius));
  }
  for (auto& group : field_groups) {
    if (!group.value || group.id == "wind" || group.id == "pressure" ||
        group.id == "wave" || group.id == "current" ||
        group.id == "air-temperature")
      continue;
    if (group.marine && !marine_cursor) continue;
    if (!nearest(ActiveFieldId(group.id), &value, group.marine)) continue;
    wxString label;
    if (group.id == "wind-gust") {
      label = speed_value(value, group.display.units);
    } else if (group.id == "sea-temperature") {
      const double celsius = TemperatureCelsius(value);
      label = group.display.units == 1
                  ? wxString::Format("%.1f °F", celsius * 9.0 / 5.0 + 32.0)
                  : wxString::Format("%.1f °C", celsius);
    } else if (group.id == "cloud" || group.id == "relative-humidity") {
      label = wxString::Format("%.0f %%", value);
    } else if (group.id == "precipitation") {
      label = wxString::Format("%.2f %s", value,
                               field_units[ActiveFieldId(group.id)]);
    } else if (group.id == "cape") {
      label = wxString::Format("%.0f J/kg", value);
    } else if (group.id == "composite-reflectivity") {
      label = wxString::Format("%.1f dBZ", value);
    } else if (group.id == "geopotential-height") {
      label = group.display.units == 1
                  ? wxString::Format("%.0f ft", value * 3.2808399)
                  : wxString::Format("%.0f m", value);
    } else {
      const wxString unit = field_units[ActiveFieldId(group.id)];
      label = wxString::Format("%.2f", value) +
              (unit.empty() ? wxString() : wxString(" ") + unit);
    }
    group.value->SetLabel(label);
  }
  cursor_status->SetLabel(wxString::Format(
      "Cursor %.4f° %c  %.4f° %c", std::abs(cursor_latitude),
      cursor_latitude >= 0 ? 'N' : 'S', std::abs(cursor_longitude),
      cursor_longitude >= 0 ? 'E' : 'W'));
  if (frame) frame->Layout();
}

bool PortableEnvironmentHost::Impl::Render(ocpnDC& dc,
                                           const ViewPort& viewport) {
  std::lock_guard<std::mutex> lock(field_mutex);
  if (fields.empty()) return false;
  bool rendered = false;
  ViewPort projection = viewport;

  auto vector_samples = [&](const wxString& u_name, const wxString& v_name,
                            bool marine_only) {
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
          std::abs(us.longitude - vs.longitude) <= 0.001 &&
          (!marine_only || IsMarinePoint(us.latitude, us.longitude)) &&
          (!marine_only || PlausibleCurrent(us.value, vs.value)))
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
                          bool meteorological, bool marine_only) {
    if (!enabled) return;
    const auto u = fields.find(u_name);
    const auto v = fields.find(v_name);
    if (u == fields.end() || v == fields.end()) return;
    const size_t count = std::min(u->second.size(), v->second.size());
    dc.SetPen(wxPen(colour, 2, wxPENSTYLE_SOLID));
    std::set<std::pair<int, int>> occupied;
    std::map<std::pair<int, int>, std::vector<wxPoint>> neighbours;
    auto accept_point = [&](const wxPoint& point) {
      const auto cell = std::make_pair(point.x / settings.vector_spacing,
                                       point.y / settings.vector_spacing);
      if (settings.vector_fixed_spacing) return occupied.insert(cell).second;
      for (int x = cell.first - 1; x <= cell.first + 1; ++x) {
        for (int y = cell.second - 1; y <= cell.second + 1; ++y) {
          const auto found = neighbours.find({x, y});
          if (found == neighbours.end()) continue;
          for (const auto& existing : found->second) {
            const long dx = point.x - existing.x;
            const long dy = point.y - existing.y;
            if (dx * dx + dy * dy <
                settings.vector_spacing * settings.vector_spacing)
              return false;
          }
        }
      }
      neighbours[cell].push_back(point);
      return true;
    };
    for (size_t i = 0; i < count; ++i) {
      const auto& us = u->second[i];
      const auto& vs = v->second[i];
      if (std::abs(us.latitude - vs.latitude) > 0.001 ||
          std::abs(us.longitude - vs.longitude) > 0.001)
        continue;
      if (marine_only && !IsMarinePoint(us.latitude, us.longitude)) continue;
      const double magnitude = std::hypot(us.value, vs.value);
      if (magnitude < 0.01 ||
          (marine_only && !PlausibleCurrent(us.value, vs.value)))
        continue;
      const wxPoint origin = projection.GetPixFromLL(us.latitude, us.longitude);
      if (origin.x < 0 || origin.y < 0 || origin.x >= viewport.pix_width ||
          origin.y >= viewport.pix_height)
        continue;
      if (!accept_point(origin)) continue;
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

  auto draw_particles = [&](const wxString& u_name, const wxString& v_name,
                            bool enabled, const LayerDisplaySettings& settings,
                            bool marine_only) {
    if (!enabled) return;
    const auto u = fields.find(u_name);
    const auto v = fields.find(v_name);
    if (u == fields.end() || v == fields.end()) return;
    const size_t count = std::min(u->second.size(), v->second.size());
    const int spacing = std::clamp(
        settings.vector_spacing + (3 - settings.particle_density) * 8, 16, 140);
    const double phase =
        std::fmod(static_cast<double>(wxGetUTCTimeMillis().GetValue()),
                  1800.0) /
        1800.0;
    dc.SetPen(wxPen(settings.colour, 2, wxPENSTYLE_SOLID));
    std::set<std::pair<int, int>> occupied;
    for (size_t i = 0; i < count; ++i) {
      const auto& us = u->second[i];
      const auto& vs = v->second[i];
      if (std::abs(us.latitude - vs.latitude) > 0.001 ||
          std::abs(us.longitude - vs.longitude) > 0.001 ||
          (marine_only && !IsMarinePoint(us.latitude, us.longitude)) ||
          (marine_only && !PlausibleCurrent(us.value, vs.value)))
        continue;
      const double magnitude = std::hypot(us.value, vs.value);
      if (magnitude < 0.01) continue;
      const wxPoint base = projection.GetPixFromLL(us.latitude, us.longitude);
      if (base.x < 0 || base.y < 0 || base.x >= viewport.pix_width ||
          base.y >= viewport.pix_height)
        continue;
      const auto cell = std::make_pair(base.x / spacing, base.y / spacing);
      if (!occupied.insert(cell).second) continue;
      const double dx = us.value / magnitude;
      const double dy = -vs.value / magnitude;
      const double offset = phase * 14.0;
      const wxPoint head(base.x + static_cast<int>(std::lround(dx * offset)),
                         base.y + static_cast<int>(std::lround(dy * offset)));
      dc.DrawLine(head.x - static_cast<int>(std::lround(dx * 5.0)),
                  head.y - static_cast<int>(std::lround(dy * 5.0)), head.x,
                  head.y);
      rendered = true;
    }
  };

  auto draw_wave_symbols = [&](bool enabled,
                               const LayerDisplaySettings& settings) {
    if (!enabled) return;
    const auto heights = fields.find("wave-height");
    const auto directions = fields.find("wave-direction");
    if (heights == fields.end() || directions == fields.end()) return;
    const size_t count =
        std::min(heights->second.size(), directions->second.size());
    const double size =
        std::clamp<double>(settings.proportional_base_size, 8.0, 40.0);
    dc.SetPen(wxPen(settings.colour, 2, wxPENSTYLE_SOLID));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    std::set<std::pair<int, int>> occupied;
    std::map<std::pair<int, int>, std::vector<wxPoint>> neighbours;
    auto accept_point = [&](const wxPoint& point) {
      const auto cell = std::make_pair(point.x / settings.vector_spacing,
                                       point.y / settings.vector_spacing);
      if (settings.vector_fixed_spacing) return occupied.insert(cell).second;
      for (int x = cell.first - 1; x <= cell.first + 1; ++x) {
        for (int y = cell.second - 1; y <= cell.second + 1; ++y) {
          const auto found = neighbours.find({x, y});
          if (found == neighbours.end()) continue;
          for (const auto& existing : found->second) {
            const long dx = point.x - existing.x;
            const long dy = point.y - existing.y;
            if (dx * dx + dy * dy <
                settings.vector_spacing * settings.vector_spacing)
              return false;
          }
        }
      }
      neighbours[cell].push_back(point);
      return true;
    };
    auto point = [](const wxPoint& origin, double along_x, double along_y,
                    double across_x, double across_y, double along,
                    double across) {
      return wxPoint(origin.x + static_cast<int>(std::lround(
                                    along_x * along + across_x * across)),
                     origin.y + static_cast<int>(std::lround(
                                    along_y * along + across_y * across)));
    };
    auto draw_line = [&](const wxPoint& first, const wxPoint& second) {
      dc.DrawLine(first.x, first.y, second.x, second.y);
    };
    for (size_t i = 0; i < count; ++i) {
      const auto& height = heights->second[i];
      const auto& direction = directions->second[i];
      if (std::abs(height.latitude - direction.latitude) > 0.001 ||
          std::abs(height.longitude - direction.longitude) > 0.001 ||
          !std::isfinite(height.value) || !std::isfinite(direction.value) ||
          height.value < 0.0 || height.value > 100.0 || direction.value < 0.0 ||
          direction.value > 360.0)
        continue;
      if (!IsMarinePoint(height.latitude, height.longitude)) continue;
      const wxPoint origin =
          projection.GetPixFromLL(height.latitude, height.longitude);
      if (origin.x < 0 || origin.y < 0 || origin.x >= viewport.pix_width ||
          origin.y >= viewport.pix_height)
        continue;
      if (!accept_point(origin)) continue;

      // dirpw is the direction waves come from. Symbols point in the
      // propagation direction and rotate with the chart viewport.
      const double travel_bearing =
          std::fmod(direction.value + 180.0, 360.0) * kPi / 180.0 +
          viewport.rotation;
      const double along_x = std::sin(travel_bearing);
      const double along_y = -std::cos(travel_bearing);
      const double across_x = -along_y;
      const double across_y = along_x;

      if (settings.vector_style == 2) {
        const double radius =
            std::clamp(size * 0.22 + height.value * 1.5, 4.0, size * 0.55);
        dc.DrawCircle(origin, static_cast<int>(std::lround(radius)));
        draw_line(origin, point(origin, along_x, along_y, across_x, across_y,
                                radius + size * 0.22, 0.0));
      } else if (settings.vector_style == 1) {
        const wxPoint tail = point(origin, along_x, along_y, across_x, across_y,
                                   -size * 0.45, 0.0);
        const wxPoint tip = point(origin, along_x, along_y, across_x, across_y,
                                  size * 0.45, 0.0);
        const wxPoint left = point(origin, along_x, along_y, across_x, across_y,
                                   size * 0.18, size * 0.20);
        const wxPoint right = point(origin, along_x, along_y, across_x,
                                    across_y, size * 0.18, -size * 0.20);
        draw_line(tail, tip);
        draw_line(tip, left);
        draw_line(tip, right);
      } else {
        // Two transverse crests plus a compact propagation marker are
        // visually distinct from both meteorological barbs and tidal arrows.
        draw_line(point(origin, along_x, along_y, across_x, across_y,
                        -size * 0.17, -size * 0.45),
                  point(origin, along_x, along_y, across_x, across_y,
                        -size * 0.17, size * 0.45));
        draw_line(point(origin, along_x, along_y, across_x, across_y,
                        size * 0.10, -size * 0.30),
                  point(origin, along_x, along_y, across_x, across_y,
                        size * 0.10, size * 0.30));
        const wxPoint marker_base = point(origin, along_x, along_y, across_x,
                                          across_y, size * 0.10, 0.0);
        const wxPoint marker_tip = point(origin, along_x, along_y, across_x,
                                         across_y, size * 0.48, 0.0);
        const wxPoint marker_left = point(origin, along_x, along_y, across_x,
                                          across_y, size * 0.28, size * 0.13);
        const wxPoint marker_right = point(origin, along_x, along_y, across_x,
                                           across_y, size * 0.28, -size * 0.13);
        draw_line(marker_base, marker_tip);
        draw_line(marker_tip, marker_left);
        draw_line(marker_tip, marker_right);
      }
      rendered = true;
    }
  };

  auto draw_scalar = [&](const std::vector<Sample>& samples, bool enabled,
                         const LayerDisplaySettings& settings) {
    if (!enabled || samples.empty()) return;
    const auto limits =
        std::minmax_element(samples.begin(), samples.end(),
                            [](const Sample& left, const Sample& right) {
                              return left.value < right.value;
                            });
    const double minimum = limits.first->value;
    const double span = std::max(1e-12, limits.second->value - minimum);
    dc.SetPen(wxPen(settings.colour, 1));
    std::set<std::pair<int, int>> occupied;
    const int scalar_cell = high_definition ? 12 : 18;
    const int scalar_radius = high_definition ? 8 : 10;
    for (const auto& sample : samples) {
      const double fraction =
          std::clamp((sample.value - minimum) / span, 0.0, 1.0);
      const wxColour colour = EnvironmentalPaletteColour(
          settings.overlay_palette, fraction, settings.colour, gradual_colours);
      dc.SetBrush(wxBrush(wxColour(colour.Red(), colour.Green(), colour.Blue(),
                                   overlay_opacity)));
      const wxPoint point =
          projection.GetPixFromLL(sample.latitude, sample.longitude);
      if (point.x < 0 || point.y < 0 || point.x >= viewport.pix_width ||
          point.y >= viewport.pix_height)
        continue;
      const auto cell =
          std::make_pair(point.x / scalar_cell, point.y / scalar_cell);
      if (!occupied.insert(cell).second) continue;
      dc.DrawCircle(point, scalar_radius);
    }
    rendered = true;
  };

  auto field_samples = [&](const wxString& name,
                           double (*convert)(double) = nullptr,
                           bool marine_only = false) {
    std::vector<Sample> output;
    const auto found = fields.find(name);
    if (found == fields.end()) return output;
    output = found->second;
    if (marine_only)
      output.erase(std::remove_if(output.begin(), output.end(),
                                  [&](const Sample& sample) {
                                    return !IsMarinePoint(sample.latitude,
                                                          sample.longitude);
                                  }),
                   output.end());
    if (convert)
      for (auto& sample : output) sample.value = convert(sample.value);
    return output;
  };
  const auto wind_speed = vector_samples(ActiveFieldId("wind", "u"),
                                         ActiveFieldId("wind", "v"), false);
  const auto current_speed = vector_samples("current-u", "current-v", true);
  const auto pressure = field_samples("pressure", PressureHpa);
  const auto waves = field_samples("wave-height", nullptr, true);
  const auto temperature =
      field_samples(ActiveFieldId("air-temperature"), TemperatureCelsius);
  std::map<wxString, std::vector<Sample>> additional_scalars;
  for (const auto& group : field_groups) {
    if (group.id == "wind" || group.id == "pressure" || group.id == "wave" ||
        group.id == "current" || group.id == "air-temperature")
      continue;
    double (*conversion)(double) = nullptr;
    if (group.id == "sea-temperature") conversion = TemperatureCelsius;
    additional_scalars[group.id] =
        field_samples(ActiveFieldId(group.id), conversion, group.marine);
  }
  draw_scalar(wind_speed,
              show_wind && show_wind->GetValue() && wind_display.overlay,
              wind_display);
  draw_scalar(
      pressure,
      show_pressure && show_pressure->GetValue() && pressure_display.overlay,
      pressure_display);
  draw_scalar(waves,
              show_waves && show_waves->GetValue() && wave_display.overlay,
              wave_display);
  draw_scalar(
      current_speed,
      show_current && show_current->GetValue() && current_display.overlay,
      current_display);
  draw_scalar(temperature,
              show_temperature && show_temperature->GetValue() &&
                  temperature_display.overlay,
              temperature_display);
  for (const auto& group : field_groups) {
    const auto samples = additional_scalars.find(group.id);
    if (samples == additional_scalars.end()) continue;
    draw_scalar(
        samples->second,
        group.visible && group.visible->GetValue() && group.display.overlay,
        group.display);
  }

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
    if (kind == 2 || kind == 6)
      return settings.units == 1 ? value * 3.2808399 : value;
    if (kind == 4 || kind == 7)
      return settings.units == 1 ? value * 9.0 / 5.0 + 32.0 : value;
    if (kind == 5) {
      if (settings.units == 1) return value;
      if (settings.units == 2) return value * 2.23693629;
      if (settings.units == 3) return value * 3.6;
      return value * 1.94384449;
    }
    return value;
  };
  auto draw_numbers = [&](const std::vector<Sample>& samples, bool enabled,
                          const LayerDisplaySettings& settings, int kind) {
    if (!enabled || samples.empty()) return;
    dc.SetTextForeground(settings.colour);
    dc.SetFont(*wxSMALL_FONT);
    std::set<std::pair<int, int>> occupied;
    std::map<std::pair<int, int>, std::vector<wxPoint>> neighbours;
    auto accept_point = [&](const wxPoint& point) {
      const auto cell = std::make_pair(point.x / settings.number_spacing,
                                       point.y / settings.number_spacing);
      if (settings.number_fixed_spacing) return occupied.insert(cell).second;
      for (int x = cell.first - 1; x <= cell.first + 1; ++x) {
        for (int y = cell.second - 1; y <= cell.second + 1; ++y) {
          const auto found = neighbours.find({x, y});
          if (found == neighbours.end()) continue;
          for (const auto& existing : found->second) {
            const long dx = point.x - existing.x;
            const long dy = point.y - existing.y;
            if (dx * dx + dy * dy <
                settings.number_spacing * settings.number_spacing)
              return false;
          }
        }
      }
      neighbours[cell].push_back(point);
      return true;
    };
    for (const auto& sample : samples) {
      const wxPoint point =
          projection.GetPixFromLL(sample.latitude, sample.longitude);
      if (point.x < 0 || point.y < 0 || point.x >= viewport.pix_width ||
          point.y >= viewport.pix_height)
        continue;
      if (!accept_point(point)) continue;
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
  for (const auto& group : field_groups) {
    const auto samples = additional_scalars.find(group.id);
    if (samples == additional_scalars.end()) continue;
    int kind = 8;
    if (group.id == "wind-gust") kind = 5;
    if (group.id == "geopotential-height") kind = 6;
    if (group.id == "sea-temperature") kind = 7;
    draw_numbers(
        samples->second,
        group.visible && group.visible->GetValue() && group.display.numbers,
        group.display, kind);
  }

  auto draw_contours = [&](const std::vector<Sample>& samples, bool enabled,
                           const LayerDisplaySettings& settings) {
    const double interval = settings.contour_spacing;
    const wxColour& colour = settings.colour;
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
            if (settings.contour_labels && !labelled && x > 1 && y > 1) {
              dc.SetTextForeground(colour);
              dc.SetFont(*wxSMALL_FONT);
              const wxString label = settings.contour_labels_abbreviated
                                         ? wxString::Format("%.3g", level)
                                         : wxString::Format("%.1f", level);
              dc.DrawText(label, crossings[0].x, crossings[0].y);
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
                wind_display);
  draw_contours(
      pressure,
      show_pressure && show_pressure->GetValue() && pressure_display.contours,
      pressure_display);
  draw_contours(temperature,
                show_temperature && show_temperature->GetValue() &&
                    temperature_display.contours,
                temperature_display);
  for (const auto& group : field_groups) {
    const auto samples = additional_scalars.find(group.id);
    if (samples == additional_scalars.end()) continue;
    draw_contours(
        samples->second,
        group.visible && group.visible->GetValue() && group.display.contours,
        group.display);
  }
  // Vectors are rendered last so translucent scalar maps cannot obscure them.
  draw_vectors(ActiveFieldId("wind", "u"), ActiveFieldId("wind", "v"),
               wind_display.colour,
               show_wind && show_wind->GetValue() && wind_display.vectors,
               wind_display, true, false);
  draw_wave_symbols(
      show_waves && show_waves->GetValue() && wave_display.vectors,
      wave_display);
  draw_vectors(
      "current-u", "current-v", current_display.colour,
      show_current && show_current->GetValue() && current_display.vectors,
      current_display, false, true);
  draw_particles(ActiveFieldId("wind", "u"), ActiveFieldId("wind", "v"),
                 show_wind && show_wind->GetValue() && wind_display.particles,
                 wind_display, false);
  draw_particles(
      "current-u", "current-v",
      show_current && show_current->GetValue() && current_display.particles,
      current_display, true);
  return rendered;
}

void PortableEnvironmentHost::Impl::Shutdown() {
  if (stopped) return;
  stopped = true;
  progress_timer.Stop();
  playback_timer.Stop();
  animation_timer.Stop();
  if (process && process_id > 0)
    wxProcess::Kill(static_cast<int>(process_id), wxSIGKILL, wxKILL_CHILDREN);
#if defined(_WIN32)
  if (windows_helper_job) {
    TerminateJobObject(windows_helper_job, 125);
    CloseHandle(windows_helper_job);
    windows_helper_job = nullptr;
  }
#endif
  if (process) {
    process->Detach();
    process = nullptr;
  }
  if (frame) {
    frame->Destroy();
    frame = nullptr;
  }
  std::lock_guard<std::mutex> decode_lock(routing_decode_mutex);
  std::lock_guard<std::mutex> lock(field_mutex);
  fields.clear();
  field_units.clear();
  field_source_times.clear();
  routing_frames.clear();
  routing_frame_lru.clear();
  routing_frame_bytes.clear();
  routing_frame_cache_bytes = 0;
  {
    std::lock_guard<std::mutex> land_lock(land_mask_mutex);
    land_mask_cache.clear();
  }
}

bool PortableEnvironmentHost::Impl::SampleBatch(
    const std::vector<PortableEnvironmentRequest>& requests,
    std::vector<PortableEnvironmentSample>* results, wxString* error) const {
  auto dataset = AcquireDataset(error);
  return dataset && SampleBatch(dataset, requests, results, error);
}

std::shared_ptr<const PortableEnvironmentDataset>
PortableEnvironmentHost::Impl::AcquireDataset(wxString* error) const {
  std::lock_guard<std::mutex> lock(field_mutex);
  if (selected_file.empty() || times.empty()) {
    if (error)
      *error =
          "The environmental provider has no time-indexed dataset; open "
          "or generate a GRIB first";
    return {};
  }
  auto dataset = std::make_shared<PortableEnvironmentDataset>();
  dataset->revision = dataset_revision;
  dataset->source = selected_file;
  dataset->display_name = selected_display_name;
  dataset->forecast_times = times;
  if (!SnapshotIdentity(dataset->source, &dataset->byte_size, &dataset->sha256,
                        error))
    return {};
  dataset->id = plugin_id + ":" + wxString::Format("%llu:", dataset_revision) +
                dataset->sha256.Left(16);
  return dataset;
}

bool PortableEnvironmentHost::Impl::SampleBatch(
    const std::shared_ptr<const PortableEnvironmentDataset>& dataset,
    const std::vector<PortableEnvironmentRequest>& requests,
    std::vector<PortableEnvironmentSample>* results, wxString* error) const {
  if (!results || requests.size() > 100000) {
    if (error) *error = "invalid environmental sample batch";
    return false;
  }
  if (!dataset || dataset->source.empty() || dataset->forecast_times.empty() ||
      dataset->sha256.empty() || dataset->byte_size == 0) {
    if (error) *error = "invalid or unavailable environmental dataset handle";
    return false;
  }
  const wxULongLong current_size = wxFileName(dataset->source).GetSize();
  if (current_size == wxInvalidSize ||
      current_size.GetValue() != dataset->byte_size) {
    if (error)
      *error = "immutable environmental dataset snapshot changed or vanished";
    return false;
  }
  std::lock_guard<std::mutex> decode_lock(routing_decode_mutex);
  const wxString source = dataset->source;
  const std::vector<wxString>& forecast_times = dataset->forecast_times;

  struct ForecastTime {
    wxString key;
    time_t epoch = 0;
  };
  std::vector<ForecastTime> parsed_times;
  parsed_times.reserve(forecast_times.size());
  for (const auto& key : forecast_times) {
    wxDateTime parsed;
    if (!ParseGribTime(key, &parsed)) continue;
    parsed.MakeFromTimezone(wxDateTime::UTC);
    parsed_times.push_back({key, parsed.GetTicks()});
  }
  if (parsed_times.empty()) {
    if (error)
      *error = "environmental forecast timeline contains no valid UTC times";
    return false;
  }
  std::sort(parsed_times.begin(), parsed_times.end(),
            [](const ForecastTime& left, const ForecastTime& right) {
              return left.epoch < right.epoch;
            });
  std::vector<wxString> request_times;
  request_times.reserve(requests.size());
  for (const auto& request : requests) {
    // The decoder performs bounded temporal interpolation per field. Passing
    // the actual route-state time avoids rejecting valid half-hour states or
    // silently snapping every state to a catalogue time. Three hours outside
    // the advertised union timeline is the decoder's explicit nearest-frame
    // limit; anything beyond it is unavailable rather than extrapolated.
    constexpr time_t kNearestLimit = 3 * 60 * 60;
    if (request.unix_time < parsed_times.front().epoch - kNearestLimit ||
        request.unix_time > parsed_times.back().epoch + kNearestLimit) {
      request_times.emplace_back();
      continue;
    }
    wxDateTime requested(static_cast<time_t>(request.unix_time));
    request_times.push_back(requested.ToUTC().Format("%Y%m%dT%H%MZ"));
  }

  auto nearest = [this](const DecodedEnvironmentFrame& frame,
                        const wxString& name, double latitude, double longitude,
                        double* output, bool marine_only = false) {
    const auto found = frame.fields.find(name);
    if (found == frame.fields.end() || found->second.empty()) return false;
    const Sample* best = nullptr;
    double best_distance = std::numeric_limits<double>::max();
    const double lon_scale = std::max(0.1, std::cos(latitude * kPi / 180.0));
    for (const auto& sample : found->second) {
      if (marine_only && !IsMarinePoint(sample.latitude, sample.longitude))
        continue;
      const double dy = sample.latitude - latitude;
      const double dx = (sample.longitude - longitude) * lon_scale;
      const double distance = dx * dx + dy * dy;
      if (distance < best_distance) {
        best_distance = distance;
        best = &sample;
      }
    }
    if (!best || best_distance > 4.0) return false;
    *output = best->value;
    return true;
  };
  auto nearest_vector = [this](const DecodedEnvironmentFrame& frame,
                               const wxString& u_name, const wxString& v_name,
                               double latitude, double longitude,
                               double* u_output, double* v_output,
                               bool marine_only) {
    const auto u_field = frame.fields.find(u_name);
    const auto v_field = frame.fields.find(v_name);
    if (u_field == frame.fields.end() || v_field == frame.fields.end())
      return false;
    const size_t count =
        std::min(u_field->second.size(), v_field->second.size());
    const double lon_scale = std::max(0.1, std::cos(latitude * kPi / 180.0));
    double best_distance = std::numeric_limits<double>::max();
    bool found = false;
    for (size_t index = 0; index < count; ++index) {
      const auto& u = u_field->second[index];
      const auto& v = v_field->second[index];
      if (std::abs(u.latitude - v.latitude) > 0.001 ||
          std::abs(u.longitude - v.longitude) > 0.001 ||
          (marine_only && !IsMarinePoint(u.latitude, u.longitude)) ||
          (marine_only && !PlausibleCurrent(u.value, v.value)))
        continue;
      const double dy = u.latitude - latitude;
      const double dx = (u.longitude - longitude) * lon_scale;
      const double distance = dx * dx + dy * dy;
      if (distance < best_distance) {
        best_distance = distance;
        *u_output = u.value;
        *v_output = v.value;
        found = true;
      }
    }
    return found && best_distance <= 4.0;
  };
  results->assign(requests.size(), PortableEnvironmentSample{});
  std::map<wxString, std::vector<size_t>> requests_by_time;
  for (size_t index = 0; index < request_times.size(); ++index)
    if (!request_times[index].empty())
      requests_by_time[request_times[index]].push_back(index);

  // Route-search frontiers often contain thousands of states at the same
  // forecast instant. Decode each distinct instant once, then satisfy every
  // request in that group while the immutable frame is hot in the cache.
  for (const auto& [request_time, indices] : requests_by_time) {
    const wxString cache_key = FrameCacheKey(source, request_time);
    auto cached = routing_frames.find(cache_key);
    if (cached == routing_frames.end()) {
      DecodedEnvironmentFrame decoded;
      if (!DecodeRoutingFrame(source, request_time, &decoded, error))
        return false;
      CacheRoutingFrame(source, request_time, decoded);
      cached = routing_frames.find(cache_key);
    }
    if (cached == routing_frames.end()) {
      if (error)
        *error = "environmental routing frame cache became inconsistent";
      return false;
    }
    routing_frame_lru.remove(cache_key);
    routing_frame_lru.push_front(cache_key);
    const auto& frame = cached->second;
    for (const size_t index : indices) {
      const auto& request = requests[index];
      auto& sample = (*results)[index];
      const bool marine = IsMarinePoint(request.latitude, request.longitude);
      double u = 0.0, v = 0.0;
      if (nearest_vector(frame, "wind-u", "wind-v", request.latitude,
                         request.longitude, &u, &v, false)) {
        sample.wind_u_knots = u * 1.94384449;
        sample.wind_v_knots = v * 1.94384449;
        sample.available |= 1;
      }
      if (marine &&
          nearest_vector(frame, "current-u", "current-v", request.latitude,
                         request.longitude, &u, &v, true)) {
        sample.current_u_knots = u * 1.94384449;
        sample.current_v_knots = v * 1.94384449;
        sample.available |= 2;
      }
      if (marine &&
          nearest(frame, "wave-height", request.latitude, request.longitude,
                  &sample.wave_height_metres, true))
        sample.available |= 4;
    }
  }
  return true;
}

wxString PortableEnvironmentHost::Impl::DatasetSummary() const {
  std::lock_guard<std::mutex> lock(field_mutex);
  if (selected_file.empty()) return "No environmental dataset is open";
  return wxString::Format("%s (%zu forecast times; %zu displayed fields)",
                          selected_display_name.empty()
                              ? wxFileName(selected_file).GetFullName()
                              : selected_display_name,
                          times.size(), fields.size());
}

bool PortableEnvironmentHost::Impl::DisplayedTime(int64_t* unix_time) const {
  if (!unix_time) return false;
  std::lock_guard<std::mutex> lock(field_mutex);
  wxDateTime parsed;
  if (!ParseGribTime(displayed_time, &parsed)) return false;
  *unix_time = parsed.GetTicks();
  return true;
}

PortableEnvironmentHost::PortableEnvironmentHost(
    wxWindow* parent, const wxString& plugin_id, const wxString& package_root,
    const wxString& surface_resource, bool credential_access,
    ocpn_portable_runtime* runtime, std::shared_ptr<std::mutex> runtime_mutex)
    : m_impl(std::make_unique<Impl>(parent, plugin_id, package_root,
                                    surface_resource, credential_access,
                                    runtime, std::move(runtime_mutex))) {}

PortableEnvironmentHost::~PortableEnvironmentHost() = default;

bool PortableEnvironmentHost::Show(wxString* error) {
  return m_impl->Show(error);
}

bool PortableEnvironmentHost::Render(ocpnDC& dc, const ViewPort& viewport) {
  return m_impl->Render(dc, viewport);
}

void PortableEnvironmentHost::SetCursorPosition(double latitude,
                                                double longitude) {
  m_impl->SetCursorPosition(latitude, longitude);
}

bool PortableEnvironmentHost::SampleBatch(
    const std::vector<PortableEnvironmentRequest>& requests,
    std::vector<PortableEnvironmentSample>* results, wxString* error) const {
  return m_impl->SampleBatch(requests, results, error);
}

std::shared_ptr<const PortableEnvironmentDataset>
PortableEnvironmentHost::AcquireDataset(wxString* error) const {
  return m_impl->AcquireDataset(error);
}

bool PortableEnvironmentHost::SampleBatch(
    const std::shared_ptr<const PortableEnvironmentDataset>& dataset,
    const std::vector<PortableEnvironmentRequest>& requests,
    std::vector<PortableEnvironmentSample>* results, wxString* error) const {
  return m_impl->SampleBatch(dataset, requests, results, error);
}

wxString PortableEnvironmentHost::DatasetSummary() const {
  return m_impl->DatasetSummary();
}

bool PortableEnvironmentHost::DisplayedTime(int64_t* unix_time) const {
  return m_impl->DisplayedTime(unix_time);
}

void PortableEnvironmentHost::Shutdown() { m_impl->Shutdown(); }
