#include "weather_routing_display.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void Expect(bool condition, const char* message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(1);
}

std::vector<std::int64_t> HourlyTimes(std::int64_t start, int hours) {
  std::vector<std::int64_t> result;
  for (int hour = 1; hour <= hours; ++hour)
    result.push_back(start + hour * 3600);
  return result;
}

}  // namespace

int main() {
  using ppm::BuildIsochroneDisplayPlan;
  using ppm::IsochronePreset;
  using ppm::SettingsForPreset;

  constexpr std::int64_t departure = 1'000'000;
  const auto times = HourlyTimes(departure, 48);

  auto navigation = SettingsForPreset(IsochronePreset::kNavigation);
  auto plan = BuildIsochroneDisplayPlan(
      times, departure, departure + 48 * 3600, navigation,
      departure + 17 * 3600 + 1200, std::nullopt, 900.0);
  Expect(plan.resolved_interval_minutes == 120,
         "48-hour navigation view should resolve to two-hour contours");
  Expect(plan.highlighted_time == departure + 17 * 3600,
         "environment time should select the nearest retained contour");
  Expect(plan.lines[16].highlighted && plan.lines[16].draw,
         "highlighted contour must remain visible after thinning");
  Expect(plan.lines.back().draw, "final contour must always be visible");

  auto analysis = SettingsForPreset(IsochronePreset::kAnalysis);
  const auto analysis_plan =
      BuildIsochroneDisplayPlan(times, departure, departure + 48 * 3600,
                                analysis, std::nullopt, std::nullopt, 900.0);
  for (const auto& line : analysis_plan.lines)
    Expect(line.draw, "analysis preset should draw every retained layer");

  auto minimal = SettingsForPreset(IsochronePreset::kMinimal);
  const auto minimal_plan =
      BuildIsochroneDisplayPlan(times, departure, departure + 48 * 3600,
                                minimal, std::nullopt, std::nullopt, 900.0);
  Expect(!minimal_plan.lines[0].draw,
         "minimal preset should suppress ordinary minor contours");
  Expect(minimal_plan.lines[11].major && minimal_plan.lines[11].draw,
         "minimal preset should retain the automatic 12-hour major contour");
  Expect(minimal_plan.lines.back().draw,
         "minimal preset should retain the final contour");

  const std::vector<std::int64_t> components{
      departure + 3600, departure + 3600, departure + 7200, departure + 7200};
  auto labelled = navigation;
  labelled.interval_minutes = -1;
  labelled.major_interval_minutes = 60;
  const auto component_plan = BuildIsochroneDisplayPlan(
      components, departure, departure + 7200, labelled, std::nullopt,
      departure + 3600, 900.0);
  Expect(component_plan.lines[0].cursor_selected &&
             component_plan.lines[1].cursor_selected,
         "every disconnected component at the cursor time should highlight");
  Expect(component_plan.lines[0].label != component_plan.lines[1].label,
         "only one component per time should own a label");

  Expect(ppm::AutomaticIsochroneIntervalMinutes(200 * 3600, 300.0) == 720,
         "zoomed-out long passages should use a sparse display interval");
  Expect(ppm::AutomaticIsochroneIntervalMinutes(10 * 3600, 2000.0) == 30,
         "zoomed-in short passages should expose additional detail");
  return 0;
}
