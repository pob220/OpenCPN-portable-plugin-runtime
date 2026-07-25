#include "weather_routing_display.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace ppm {
namespace {

constexpr std::int64_t kMinute = 60;

std::optional<std::int64_t> ClosestTime(const std::set<std::int64_t>& times,
                                        std::optional<std::int64_t> requested) {
  if (!requested || times.empty()) return std::nullopt;
  std::int64_t closest = *times.begin();
  std::int64_t closest_difference = std::llabs(closest - *requested);
  for (const std::int64_t time : times) {
    const std::int64_t difference = std::llabs(time - *requested);
    if (difference < closest_difference ||
        (difference == closest_difference && time < closest)) {
      closest = time;
      closest_difference = difference;
    }
  }
  return closest;
}

bool IsMajorTime(std::int64_t time, std::int64_t departure, int major_minutes,
                 std::int64_t tolerance_seconds) {
  if (major_minutes <= 0 || time <= departure) return false;
  const std::int64_t interval =
      static_cast<std::int64_t>(major_minutes) * kMinute;
  const std::int64_t elapsed = time - departure;
  const std::int64_t remainder = elapsed % interval;
  return remainder <= tolerance_seconds ||
         interval - remainder <= tolerance_seconds;
}

}  // namespace

IsochroneDisplaySettings SettingsForPreset(IsochronePreset preset) {
  IsochroneDisplaySettings settings;
  settings.preset = preset;
  switch (preset) {
    case IsochronePreset::kAnalysis:
      settings.interval_minutes = -1;
      settings.major_interval_minutes = 360;
      settings.line_width = 1.0;
      settings.opacity_percent = 54;
      settings.colour_mode = IsochroneColourMode::kElapsedTime;
      settings.highlight_environment_time = true;
      settings.show_time_labels = true;
      settings.fade_during_cursor_inspection = false;
      settings.show_front_points = false;
      break;
    case IsochronePreset::kMinimal:
      settings.interval_minutes = 0;
      settings.major_interval_minutes = 0;
      settings.line_width = 1.5;
      settings.opacity_percent = 48;
      settings.colour_mode = IsochroneColourMode::kUniform;
      settings.highlight_environment_time = true;
      settings.show_time_labels = true;
      settings.fade_during_cursor_inspection = true;
      settings.show_front_points = false;
      break;
    case IsochronePreset::kCustom:
      settings.preset = IsochronePreset::kCustom;
      break;
    case IsochronePreset::kNavigation:
    default:
      settings.preset = IsochronePreset::kNavigation;
      break;
  }
  return settings;
}

int AutomaticIsochroneIntervalMinutes(std::int64_t passage_seconds,
                                      double projected_route_pixels) {
  const double hours = std::max<std::int64_t>(0, passage_seconds) / 3600.0;
  int minutes = hours <= 12.0    ? 60
                : hours <= 48.0  ? 120
                : hours <= 120.0 ? 180
                : hours <= 240.0 ? 360
                                 : 720;
  if (std::isfinite(projected_route_pixels)) {
    if (projected_route_pixels < 500.0)
      minutes = std::min(1440, minutes * 2);
    else if (projected_route_pixels > 1800.0)
      minutes = std::max(30, minutes / 2);
  }
  return minutes;
}

int AutomaticMajorIsochroneIntervalMinutes(std::int64_t passage_seconds) {
  const double hours = std::max<std::int64_t>(0, passage_seconds) / 3600.0;
  return hours <= 24.0 ? 360 : hours <= 72.0 ? 720 : 1440;
}

IsochroneDisplayPlan BuildIsochroneDisplayPlan(
    const std::vector<std::int64_t>& line_times,
    std::int64_t departure_unix_time, std::int64_t arrival_unix_time,
    const IsochroneDisplaySettings& settings,
    std::optional<std::int64_t> displayed_environment_time,
    std::optional<std::int64_t> cursor_trace_time,
    double projected_route_pixels) {
  IsochroneDisplayPlan plan;
  plan.lines.resize(line_times.size());
  if (line_times.empty()) return plan;

  std::set<std::int64_t> unique_times(line_times.begin(), line_times.end());
  const std::int64_t passage_seconds =
      std::max<std::int64_t>(0, arrival_unix_time - departure_unix_time);
  plan.resolved_interval_minutes =
      settings.interval_minutes < 0 ? -1
      : settings.interval_minutes > 0
          ? settings.interval_minutes
          : AutomaticIsochroneIntervalMinutes(passage_seconds,
                                              projected_route_pixels);
  plan.resolved_major_interval_minutes =
      settings.major_interval_minutes > 0
          ? settings.major_interval_minutes
          : AutomaticMajorIsochroneIntervalMinutes(passage_seconds);

  if (settings.highlight_environment_time)
    plan.highlighted_time =
        ClosestTime(unique_times, displayed_environment_time);
  const std::optional<std::int64_t> selected_cursor_time =
      ClosestTime(unique_times, cursor_trace_time);

  std::int64_t minimum_spacing = std::numeric_limits<std::int64_t>::max();
  std::optional<std::int64_t> previous_time;
  for (const std::int64_t time : unique_times) {
    if (previous_time)
      minimum_spacing = std::min(minimum_spacing, time - *previous_time);
    previous_time = time;
  }
  if (minimum_spacing == std::numeric_limits<std::int64_t>::max())
    minimum_spacing = kMinute;
  const std::int64_t major_tolerance =
      std::max<std::int64_t>(kMinute, minimum_spacing / 3);

  std::map<std::int64_t, bool> draw_time;
  std::map<std::int64_t, bool> major_time;
  std::optional<std::int64_t> last_drawn;
  for (const std::int64_t time : unique_times) {
    const bool first = time == *unique_times.begin();
    const bool final = time == *unique_times.rbegin();
    const bool highlighted = plan.highlighted_time == time;
    const bool cursor_selected = selected_cursor_time == time;
    const bool major =
        IsMajorTime(time, departure_unix_time,
                    plan.resolved_major_interval_minutes, major_tolerance);
    bool draw = plan.resolved_interval_minutes < 0 || first;
    if (!draw && (!last_drawn ||
                  time - *last_drawn >= static_cast<std::int64_t>(
                                            plan.resolved_interval_minutes) *
                                            kMinute))
      draw = true;
    draw = draw || final || major || highlighted || cursor_selected;
    if (settings.preset == IsochronePreset::kMinimal)
      draw = final || major || highlighted || cursor_selected;
    draw_time[time] = draw;
    major_time[time] = major;
    if (draw) last_drawn = time;
  }

  std::set<std::int64_t> labelled_times;
  const double duration =
      std::max<std::int64_t>(1, arrival_unix_time - departure_unix_time);
  for (std::size_t index = 0; index < line_times.size(); ++index) {
    const std::int64_t time = line_times[index];
    auto& style = plan.lines[index];
    style.draw = draw_time[time];
    style.major = major_time[time];
    style.highlighted = plan.highlighted_time == time;
    style.cursor_selected = selected_cursor_time == time;
    style.elapsed_fraction = std::clamp(
        static_cast<double>(time - departure_unix_time) / duration, 0.0, 1.0);
    style.label = style.draw && settings.show_time_labels &&
                  (style.major || style.highlighted || style.cursor_selected) &&
                  labelled_times.insert(time).second;
  }
  return plan;
}

}  // namespace ppm
