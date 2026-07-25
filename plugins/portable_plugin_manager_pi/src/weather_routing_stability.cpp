#include "weather_routing_stability.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace ppm {
namespace {

constexpr double kEarthNauticalMilesPerDegree = 60.0;
constexpr std::size_t kResamplePoints = 64;

struct XYPoint {
  double x = 0.0;
  double y = 0.0;
};

struct Projection {
  explicit Projection(double latitude)
      : longitude_scale(
            kEarthNauticalMilesPerDegree *
            std::max(0.05,
                     std::cos(latitude * 3.14159265358979323846 / 180.0))) {}

  XYPoint ToXY(const StabilityPoint& point) const {
    return {point.longitude * longitude_scale,
            point.latitude * kEarthNauticalMilesPerDegree};
  }

  StabilityPoint ToGeo(double x, double y) const {
    return {y / kEarthNauticalMilesPerDegree, x / longitude_scale};
  }

  double longitude_scale;
};

double Distance(const XYPoint& first, const XYPoint& second) {
  return std::hypot(first.x - second.x, first.y - second.y);
}

std::vector<StabilityPoint> Resample(const StabilityRoute& route,
                                     const Projection& projection) {
  std::vector<StabilityPoint> result;
  if (route.points.size() < 2) return result;
  std::vector<double> cumulative(route.points.size(), 0.0);
  for (std::size_t index = 1; index < route.points.size(); ++index)
    cumulative[index] = cumulative[index - 1] +
                        Distance(projection.ToXY(route.points[index - 1]),
                                 projection.ToXY(route.points[index]));
  if (cumulative.back() <= 1e-9) return result;
  result.reserve(kResamplePoints);
  std::size_t segment = 1;
  for (std::size_t sample = 0; sample < kResamplePoints; ++sample) {
    const double target = cumulative.back() * static_cast<double>(sample) /
                          static_cast<double>(kResamplePoints - 1);
    while (segment + 1 < cumulative.size() && cumulative[segment] < target)
      ++segment;
    const double segment_length = cumulative[segment] - cumulative[segment - 1];
    const double fraction =
        segment_length > 1e-9
            ? (target - cumulative[segment - 1]) / segment_length
            : 0.0;
    const XYPoint first = projection.ToXY(route.points[segment - 1]);
    const XYPoint second = projection.ToXY(route.points[segment]);
    result.push_back(
        projection.ToGeo(first.x + fraction * (second.x - first.x),
                         first.y + fraction * (second.y - first.y)));
  }
  return result;
}

double AverageDistance(const std::vector<StabilityPoint>& first,
                       const std::vector<StabilityPoint>& second,
                       const Projection& projection) {
  if (first.size() != second.size() || first.empty())
    return std::numeric_limits<double>::infinity();
  double sum = 0.0;
  for (std::size_t index = 0; index < first.size(); ++index)
    sum +=
        Distance(projection.ToXY(first[index]), projection.ToXY(second[index]));
  return sum / static_cast<double>(first.size());
}

double PointSegmentDistance(const XYPoint& point, const XYPoint& first,
                            const XYPoint& second) {
  const double dx = second.x - first.x;
  const double dy = second.y - first.y;
  const double length_squared = dx * dx + dy * dy;
  if (length_squared <= 1e-12) return Distance(point, first);
  const double fraction = std::clamp(
      ((point.x - first.x) * dx + (point.y - first.y) * dy) / length_squared,
      0.0, 1.0);
  return std::hypot(point.x - (first.x + fraction * dx),
                    point.y - (first.y + fraction * dy));
}

using CellKey = std::pair<int, int>;

std::set<CellKey> Rasterize(const StabilityRoute& route,
                            const Projection& projection, double resolution,
                            double influence, std::size_t maximum_cells,
                            bool* exceeded) {
  std::set<CellKey> cells;
  if (exceeded) *exceeded = false;
  const int radius =
      std::max(1, static_cast<int>(std::ceil(influence / resolution)));
  for (std::size_t segment = 1; segment < route.points.size(); ++segment) {
    const XYPoint first = projection.ToXY(route.points[segment - 1]);
    const XYPoint second = projection.ToXY(route.points[segment]);
    const int minimum_x =
        static_cast<int>(std::floor(std::min(first.x, second.x) / resolution)) -
        radius;
    const int maximum_x =
        static_cast<int>(std::floor(std::max(first.x, second.x) / resolution)) +
        radius;
    const int minimum_y =
        static_cast<int>(std::floor(std::min(first.y, second.y) / resolution)) -
        radius;
    const int maximum_y =
        static_cast<int>(std::floor(std::max(first.y, second.y) / resolution)) +
        radius;
    for (int y = minimum_y; y <= maximum_y; ++y) {
      for (int x = minimum_x; x <= maximum_x; ++x) {
        const XYPoint centre{(x + 0.5) * resolution, (y + 0.5) * resolution};
        if (PointSegmentDistance(centre, first, second) >
            influence + resolution * 0.71)
          continue;
        cells.emplace(x, y);
        if (cells.size() > maximum_cells) {
          if (exceeded) *exceeded = true;
          return {};
        }
      }
    }
  }
  return cells;
}

StabilityCell MakeCell(const CellKey& key, const Projection& projection,
                       double resolution, double agreement) {
  const StabilityPoint southwest =
      projection.ToGeo(key.first * resolution, key.second * resolution);
  const StabilityPoint northeast = projection.ToGeo(
      (key.first + 1) * resolution, (key.second + 1) * resolution);
  return {key.first,
          key.second,
          std::min(southwest.latitude, northeast.latitude),
          std::min(southwest.longitude, northeast.longitude),
          std::max(southwest.latitude, northeast.latitude),
          std::max(southwest.longitude, northeast.longitude),
          agreement};
}

}  // namespace

StabilityCorridorResult BuildStabilityCorridor(
    const std::vector<StabilityRoute>& routes,
    const StabilityCorridorOptions& requested,
    const StabilityCellSafetyBatch& safety) {
  StabilityCorridorResult result;
  result.input_routes = routes.size();
  StabilityCorridorOptions options = requested;
  options.minimum_routes = std::max<std::size_t>(3, options.minimum_routes);
  options.grid_resolution_nautical_miles =
      std::max(0.05, options.grid_resolution_nautical_miles);
  options.route_influence_nautical_miles =
      std::max(options.grid_resolution_nautical_miles,
               options.route_influence_nautical_miles);
  options.outer_agreement = std::clamp(options.outer_agreement, 0.0, 1.0);
  options.inner_agreement =
      std::clamp(options.inner_agreement, options.outer_agreement, 1.0);
  options.cluster_distance_nautical_miles =
      std::max(0.1, options.cluster_distance_nautical_miles);
  options.maximum_cells = std::max<std::size_t>(1000, options.maximum_cells);

  std::vector<std::size_t> valid;
  std::uint64_t best_duration = std::numeric_limits<std::uint64_t>::max();
  double latitude_sum = 0.0;
  std::size_t point_count = 0;
  for (std::size_t index = 0; index < routes.size(); ++index) {
    const auto& route = routes[index];
    const bool geometry_valid =
        route.points.size() >= 2 &&
        std::all_of(route.points.begin(), route.points.end(),
                    [](const StabilityPoint& point) {
                      return std::isfinite(point.latitude) &&
                             std::isfinite(point.longitude) &&
                             point.latitude >= -90.0 &&
                             point.latitude <= 90.0 &&
                             point.longitude >= -180.0 &&
                             point.longitude <= 180.0;
                    });
    if (!route.complete || !route.independently_validated || !geometry_valid ||
        route.duration_seconds == 0) {
      ++result.excluded_routes;
      continue;
    }
    best_duration = std::min(best_duration, route.duration_seconds);
    valid.push_back(index);
    for (const auto& point : route.points) {
      latitude_sum += point.latitude;
      ++point_count;
    }
  }

  if (best_duration != std::numeric_limits<std::uint64_t>::max() &&
      options.maximum_elapsed_penalty_minutes >= 0.0) {
    const auto maximum_duration =
        best_duration + static_cast<std::uint64_t>(
                            options.maximum_elapsed_penalty_minutes * 60.0);
    valid.erase(
        std::remove_if(valid.begin(), valid.end(),
                       [&](std::size_t index) {
                         if (routes[index].duration_seconds <= maximum_duration)
                           return false;
                         ++result.excluded_routes;
                         ++result.elapsed_excluded_routes;
                         return true;
                       }),
        valid.end());
  }
  result.validated_routes = valid.size();
  if (valid.size() < options.minimum_routes) {
    result.failure = "fewer than three comparable validated routes";
    return result;
  }

  const Projection projection(point_count ? latitude_sum / point_count : 0.0);
  std::vector<std::vector<StabilityPoint>> resampled(routes.size());
  for (const std::size_t index : valid)
    resampled[index] = Resample(routes[index], projection);

  std::map<std::pair<std::size_t, std::size_t>, double> distances;
  auto distance_for = [&](std::size_t first, std::size_t second) {
    if (first == second) return 0.0;
    if (first > second) std::swap(first, second);
    const auto key = std::make_pair(first, second);
    const auto existing = distances.find(key);
    if (existing != distances.end()) return existing->second;
    const double distance =
        AverageDistance(resampled[first], resampled[second], projection);
    distances.emplace(key, distance);
    return distance;
  };

  std::vector<std::vector<std::size_t>> clusters;
  for (const std::size_t route_index : valid) {
    bool assigned = false;
    for (auto& cluster : clusters) {
      const bool compatible =
          std::all_of(cluster.begin(), cluster.end(), [&](std::size_t member) {
            return distance_for(route_index, member) <=
                   options.cluster_distance_nautical_miles;
          });
      if (!compatible) continue;
      cluster.push_back(route_index);
      assigned = true;
      break;
    }
    if (!assigned) clusters.push_back({route_index});
  }

  for (const auto& cluster : clusters) {
    if (cluster.size() < options.minimum_routes) continue;
    StabilityRouteFamily family;
    family.id = result.families.size();
    family.route_indices = cluster;
    double best_aggregate = std::numeric_limits<double>::infinity();
    for (const std::size_t candidate : cluster) {
      double aggregate = 0.0;
      for (const std::size_t other : cluster)
        aggregate += distance_for(candidate, other);
      if (aggregate < best_aggregate) {
        best_aggregate = aggregate;
        family.representative_route_index = candidate;
      }
    }

    std::vector<double> widths;
    widths.reserve(kResamplePoints);
    for (std::size_t sample = 0; sample < kResamplePoints; ++sample) {
      double maximum = 0.0;
      for (std::size_t first = 0; first < cluster.size(); ++first)
        for (std::size_t second = first + 1; second < cluster.size(); ++second)
          maximum = std::max(
              maximum,
              Distance(projection.ToXY(resampled[cluster[first]][sample]),
                       projection.ToXY(resampled[cluster[second]][sample])));
      widths.push_back(maximum);
    }
    std::sort(widths.begin(), widths.end());
    family.median_width_nautical_miles = widths[widths.size() / 2];
    family.maximum_width_nautical_miles = widths.back();

    std::int64_t earliest_eta = std::numeric_limits<std::int64_t>::max();
    std::int64_t latest_eta = std::numeric_limits<std::int64_t>::min();
    for (const std::size_t index : cluster) {
      const std::int64_t eta =
          routes[index].departure_unix_time +
          static_cast<std::int64_t>(routes[index].duration_seconds);
      earliest_eta = std::min(earliest_eta, eta);
      latest_eta = std::max(latest_eta, eta);
    }
    family.eta_spread_minutes =
        static_cast<double>(latest_eta - earliest_eta) / 60.0;

    std::map<CellKey, std::size_t> usage;
    for (const std::size_t index : cluster) {
      bool exceeded = false;
      const auto cells = Rasterize(routes[index], projection,
                                   options.grid_resolution_nautical_miles,
                                   options.route_influence_nautical_miles,
                                   options.maximum_cells, &exceeded);
      if (exceeded) {
        result.failure = "stability corridor exceeded its bounded cell budget";
        return result;
      }
      for (const auto& cell : cells) {
        ++usage[cell];
        if (usage.size() > options.maximum_cells) {
          result.failure =
              "stability corridor exceeded its bounded cell budget";
          return result;
        }
      }
    }
    result.raster_cells_used += usage.size();
    std::vector<StabilityCell> candidate_cells;
    for (const auto& [key, count] : usage) {
      const double agreement =
          static_cast<double>(count) / static_cast<double>(cluster.size());
      if (agreement + 1e-12 < options.outer_agreement) continue;
      candidate_cells.push_back(MakeCell(
          key, projection, options.grid_resolution_nautical_miles, agreement));
    }

    std::vector<StabilityCellSafety> assessments(candidate_cells.size(),
                                                 StabilityCellSafety::kSafe);
    if (safety) assessments = safety(candidate_cells);
    if (assessments.size() != candidate_cells.size()) {
      result.failure = "chart-safety service returned the wrong cell count";
      return result;
    }
    for (std::size_t index = 0; index < candidate_cells.size(); ++index) {
      if (assessments[index] == StabilityCellSafety::kUnsafe) {
        ++result.unsafe_cells_excluded;
        continue;
      }
      if (assessments[index] != StabilityCellSafety::kSafe) {
        ++result.unresolved_cells_excluded;
        continue;
      }
      family.outer_cells.push_back(candidate_cells[index]);
      if (candidate_cells[index].agreement + 1e-12 >= options.inner_agreement)
        family.inner_cells.push_back(candidate_cells[index]);
    }
    if (!family.outer_cells.empty())
      result.families.push_back(std::move(family));
  }

  if (result.families.empty()) {
    result.failure =
        "no route family met the agreement and chart-safety criteria";
    return result;
  }
  result.success = true;
  return result;
}

std::optional<std::size_t> FindStabilityFamily(
    const StabilityCorridorResult& result, std::size_t route_index) {
  for (const auto& family : result.families)
    if (std::find(family.route_indices.begin(), family.route_indices.end(),
                  route_index) != family.route_indices.end())
      return family.id;
  return std::nullopt;
}

StabilityRenderPlan BuildStabilityRenderPlan(
    const StabilityCorridorResult& result, std::size_t selected_route_index,
    StabilityRenderBackend backend) {
  (void)backend;
  StabilityRenderPlan plan;
  const auto family_id = FindStabilityFamily(result, selected_route_index);
  if (!family_id) return plan;
  const auto family =
      std::find_if(result.families.begin(), result.families.end(),
                   [family_id](const StabilityRouteFamily& candidate) {
                     return candidate.id == *family_id;
                   });
  if (family == result.families.end()) return plan;
  plan.family = &*family;
  plan.outer_cells = &family->outer_cells;
  plan.inner_cells = &family->inner_cells;
  plan.representative_route_index = family->representative_route_index;
  return plan;
}

}  // namespace ppm
