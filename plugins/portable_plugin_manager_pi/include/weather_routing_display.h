#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace ppm {

enum class IsochronePreset {
  kNavigation = 0,
  kAnalysis = 1,
  kMinimal = 2,
  kCustom = 3,
};

enum class IsochroneColourMode {
  kUniform = 0,
  kElapsedTime = 1,
};

struct IsochroneDisplaySettings {
  IsochronePreset preset = IsochronePreset::kNavigation;
  // -1 draws every retained solver layer; zero selects an automatic display
  // interval; positive values are display minutes and never affect solving.
  int interval_minutes = 0;
  // Zero selects an automatic major-contour interval.
  int major_interval_minutes = 0;
  double line_width = 1.0;
  int opacity_percent = 36;
  IsochroneColourMode colour_mode = IsochroneColourMode::kUniform;
  bool highlight_environment_time = true;
  bool show_time_labels = true;
  bool fade_during_cursor_inspection = true;
  bool show_front_points = false;
};

struct IsochroneLineDisplay {
  bool draw = false;
  bool major = false;
  bool highlighted = false;
  bool cursor_selected = false;
  bool label = false;
  double elapsed_fraction = 0.0;
};

struct IsochroneDisplayPlan {
  std::vector<IsochroneLineDisplay> lines;
  int resolved_interval_minutes = 0;
  int resolved_major_interval_minutes = 0;
  std::optional<std::int64_t> highlighted_time;
};

IsochroneDisplaySettings SettingsForPreset(IsochronePreset preset);

int AutomaticIsochroneIntervalMinutes(std::int64_t passage_seconds,
                                      double projected_route_pixels);

int AutomaticMajorIsochroneIntervalMinutes(std::int64_t passage_seconds);

IsochroneDisplayPlan BuildIsochroneDisplayPlan(
    const std::vector<std::int64_t>& line_times,
    std::int64_t departure_unix_time, std::int64_t arrival_unix_time,
    const IsochroneDisplaySettings& settings,
    std::optional<std::int64_t> displayed_environment_time,
    std::optional<std::int64_t> cursor_trace_time,
    double projected_route_pixels);

}  // namespace ppm
