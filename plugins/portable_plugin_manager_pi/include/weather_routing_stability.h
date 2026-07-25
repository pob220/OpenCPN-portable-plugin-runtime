#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ppm {

struct StabilityPoint {
  double latitude = 0.0;
  double longitude = 0.0;
};

struct StabilityRoute {
  std::string id;
  std::int64_t departure_unix_time = 0;
  std::uint64_t duration_seconds = 0;
  bool complete = false;
  bool independently_validated = false;
  std::vector<StabilityPoint> points;
};

struct StabilityCorridorOptions {
  std::size_t minimum_routes = 3;
  double maximum_elapsed_penalty_minutes = 120.0;
  double grid_resolution_nautical_miles = 0.5;
  double inner_agreement = 0.7;
  double outer_agreement = 0.4;
  double cluster_distance_nautical_miles = 2.5;
  double route_influence_nautical_miles = 0.75;
  std::size_t maximum_cells = 100000;
};

struct StabilityCell {
  int x = 0;
  int y = 0;
  double minimum_latitude = 0.0;
  double minimum_longitude = 0.0;
  double maximum_latitude = 0.0;
  double maximum_longitude = 0.0;
  double agreement = 0.0;
};

enum class StabilityCellSafety {
  kSafe,
  kUnsafe,
  kMissing,
  kError,
};

using StabilityCellSafetyBatch = std::function<std::vector<StabilityCellSafety>(
    const std::vector<StabilityCell>&)>;

struct StabilityRouteFamily {
  std::size_t id = 0;
  std::vector<std::size_t> route_indices;
  std::size_t representative_route_index = 0;
  std::vector<StabilityCell> inner_cells;
  std::vector<StabilityCell> outer_cells;
  double median_width_nautical_miles = 0.0;
  double maximum_width_nautical_miles = 0.0;
  double eta_spread_minutes = 0.0;
};

struct StabilityCorridorResult {
  bool success = false;
  std::size_t input_routes = 0;
  std::size_t validated_routes = 0;
  std::size_t excluded_routes = 0;
  std::size_t elapsed_excluded_routes = 0;
  std::size_t unsafe_cells_excluded = 0;
  std::size_t unresolved_cells_excluded = 0;
  std::size_t raster_cells_used = 0;
  std::string failure;
  std::vector<StabilityRouteFamily> families;
};

enum class StabilityRenderBackend {
  kSoftware,
  kOpenGL,
  kVulkan,
};

// Renderer-neutral view of one selected route family. All backends consume
// the same validated cells and representative-route index.
struct StabilityRenderPlan {
  const StabilityRouteFamily* family = nullptr;
  const std::vector<StabilityCell>* outer_cells = nullptr;
  const std::vector<StabilityCell>* inner_cells = nullptr;
  std::size_t representative_route_index = 0;
};

StabilityCorridorResult BuildStabilityCorridor(
    const std::vector<StabilityRoute>& routes,
    const StabilityCorridorOptions& options,
    const StabilityCellSafetyBatch& safety = {});

std::optional<std::size_t> FindStabilityFamily(
    const StabilityCorridorResult& result, std::size_t route_index);

StabilityRenderPlan BuildStabilityRenderPlan(
    const StabilityCorridorResult& result, std::size_t selected_route_index,
    StabilityRenderBackend backend);

}  // namespace ppm
