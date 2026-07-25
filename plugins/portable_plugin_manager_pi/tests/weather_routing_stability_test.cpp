#include "weather_routing_stability.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {

void Expect(bool condition, const char* message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(1);
}

ppm::StabilityRoute Route(std::string id, double latitude,
                          std::uint64_t duration = 3600) {
  ppm::StabilityRoute route;
  route.id = std::move(id);
  route.departure_unix_time = 1'000'000;
  route.duration_seconds = duration;
  route.complete = true;
  route.independently_validated = true;
  route.points = {{latitude, 0.0}, {latitude, 0.1}, {latitude, 0.2}};
  return route;
}

}  // namespace

int main() {
  ppm::StabilityCorridorOptions options;
  auto fewer = ppm::BuildStabilityCorridor(
      {Route("one", 0.0), Route("two", 0.005)}, options);
  Expect(!fewer.success &&
             fewer.failure.find("fewer than three") != std::string::npos,
         "corridor was available with fewer than three routes");

  std::vector<ppm::StabilityRoute> routes{
      Route("a0", 0.000), Route("a1", 0.005), Route("a2", -0.005),
      Route("b0", 1.000), Route("b1", 1.005), Route("b2", 0.995)};
  options.maximum_elapsed_penalty_minutes = 30.0;
  std::size_t safety_batches = 0;
  const auto families = ppm::BuildStabilityCorridor(
      routes, options, [&](const std::vector<ppm::StabilityCell>& cells) {
        ++safety_batches;
        return std::vector<ppm::StabilityCellSafety>(
            cells.size(), ppm::StabilityCellSafety::kSafe);
      });
  Expect(families.success && families.families.size() == 2,
         "geometrically separate route families were merged");
  Expect(ppm::FindStabilityFamily(families, 0) == 0 &&
             ppm::FindStabilityFamily(families, 3) == 1,
         "selected routes did not resolve to their families");
  Expect(safety_batches == 2,
         "chart safety was not evaluated in one batch per family");
  for (const auto& family : families.families)
    Expect(!family.inner_cells.empty() && !family.outer_cells.empty() &&
               family.route_indices.size() == 3,
           "agreement bands omitted a valid family");
  const auto software = ppm::BuildStabilityRenderPlan(
      families, 0, ppm::StabilityRenderBackend::kSoftware);
  const auto opengl = ppm::BuildStabilityRenderPlan(
      families, 0, ppm::StabilityRenderBackend::kOpenGL);
  const auto vulkan = ppm::BuildStabilityRenderPlan(
      families, 0, ppm::StabilityRenderBackend::kVulkan);
  Expect(software.family && opengl.family && vulkan.family &&
             software.outer_cells->size() == opengl.outer_cells->size() &&
             software.outer_cells->size() == vulkan.outer_cells->size() &&
             software.inner_cells->size() == opengl.inner_cells->size() &&
             software.inner_cells->size() == vulkan.inner_cells->size() &&
             software.representative_route_index ==
                 vulkan.representative_route_index,
         "software, OpenGL and Vulkan presentation plans diverged");

  auto slow_routes = std::vector<ppm::StabilityRoute>{
      Route("fast0", 0.000), Route("fast1", 0.005), Route("fast2", -0.005),
      Route("slow", 0.002, 7200)};
  const auto elapsed = ppm::BuildStabilityCorridor(slow_routes, options);
  Expect(elapsed.success && elapsed.elapsed_excluded_routes == 1 &&
             elapsed.validated_routes == 3,
         "elapsed-time penalty did not exclude a slow candidate");

  const auto unsafe = ppm::BuildStabilityCorridor(
      {Route("safe0", 0.000), Route("safe1", 0.005), Route("safe2", -0.005)},
      options, [](const std::vector<ppm::StabilityCell>& cells) {
        std::vector<ppm::StabilityCellSafety> result(
            cells.size(), ppm::StabilityCellSafety::kSafe);
        for (std::size_t index = 0; index < result.size(); index += 2)
          result[index] = ppm::StabilityCellSafety::kUnsafe;
        return result;
      });
  Expect(unsafe.success && unsafe.unsafe_cells_excluded > 0,
         "unsafe agreement cells were not excluded");
  const auto& unsafe_family = unsafe.families.front();
  Expect(unsafe_family.outer_cells.size() + unsafe.unsafe_cells_excluded > 0,
         "safety filtering lost its accounting");

  const auto unresolved = ppm::BuildStabilityCorridor(
      {Route("m0", 0.000), Route("m1", 0.005), Route("m2", -0.005)}, options,
      [](const std::vector<ppm::StabilityCell>& cells) {
        return std::vector<ppm::StabilityCellSafety>(
            cells.size(), ppm::StabilityCellSafety::kMissing);
      });
  Expect(!unresolved.success && unresolved.unresolved_cells_excluded > 0,
         "unresolved chart cells did not fail closed");

  auto invalid = Route("invalid", 0.0);
  invalid.points[1].latitude = std::numeric_limits<double>::quiet_NaN();
  const auto invalid_geometry = ppm::BuildStabilityCorridor(
      {Route("v0", 0.000), Route("v1", 0.005), Route("v2", -0.005), invalid},
      options);
  Expect(invalid_geometry.success && invalid_geometry.excluded_routes == 1 &&
             invalid_geometry.validated_routes == 3,
         "invalid candidate geometry was not excluded before analysis");
  return 0;
}
