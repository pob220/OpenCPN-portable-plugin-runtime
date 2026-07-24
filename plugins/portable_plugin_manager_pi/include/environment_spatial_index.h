#ifndef PORTABLE_PLUGIN_MANAGER_ENVIRONMENT_SPATIAL_INDEX_H
#define PORTABLE_PLUGIN_MANAGER_ENVIRONMENT_SPATIAL_INDEX_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace ppm {

/**
 * Deterministic bounded spatial index for the helper's sampled GRIB fields.
 *
 * The helper deliberately publishes a bounded sample rather than exposing
 * decoder-owned grid objects across the process boundary.  A fixed geographic
 * bin index preserves the existing exact nearest-sample result while avoiding
 * a complete field scan for every route state.
 */
class EnvironmentSpatialIndex {
public:
  static constexpr double kBinDegrees = 0.25;
  static constexpr double kMaximumSampleDistanceDegrees = 2.0;

  template <typename Samples>
  void Build(const Samples& samples) {
    bins_.clear();
    for (std::size_t index = 0; index < samples.size(); ++index) {
      const auto& sample = samples[index];
      bins_[Bin(sample.latitude, sample.longitude)].push_back(
          static_cast<std::uint32_t>(index));
    }
    plans_ = std::make_shared<PlanCache>();
  }

  template <typename Function>
  void ForEachCandidate(double latitude, double longitude,
                        Function&& function) const {
    if (bins_.empty() || !std::isfinite(latitude) ||
        !std::isfinite(longitude)) {
      return;
    }
    const BinKey query_bin = Bin(latitude, longitude);
    const auto candidates = CandidatePlan(query_bin);
    for (const std::uint32_t index : *candidates) function(index);
  }

  std::size_t EstimatedBytes() const {
    std::size_t bytes = sizeof(*this);
    for (const auto& [key, values] : bins_) {
      static_cast<void>(key);
      bytes += sizeof(key) + values.capacity() * sizeof(std::uint32_t);
    }
    if (plans_) {
      std::lock_guard<std::mutex> lock(plans_->mutex);
      for (const auto& [key, values] : plans_->values) {
        static_cast<void>(key);
        bytes += sizeof(key) + values->capacity() * sizeof(std::uint32_t);
      }
    }
    return bytes;
  }

private:
  using BinKey = std::pair<int, int>;

  struct PlanCache {
    std::mutex mutex;
    std::map<BinKey, std::shared_ptr<const std::vector<std::uint32_t>>> values;
    std::list<BinKey> lru;
  };

  std::shared_ptr<const std::vector<std::uint32_t>> CandidatePlan(
      const BinKey& query_bin) const {
    if (!plans_) plans_ = std::make_shared<PlanCache>();
    {
      std::lock_guard<std::mutex> lock(plans_->mutex);
      const auto found = plans_->values.find(query_bin);
      if (found != plans_->values.end()) {
        plans_->lru.remove(query_bin);
        plans_->lru.push_front(query_bin);
        return found->second;
      }
    }

    // Cache a conservative superset for the entire 0.25-degree query bin,
    // rather than for one floating-point position. The final nearest-sample
    // distance test remains unchanged, so sharing this plan cannot alter the
    // selected sample or admit data outside the existing two-degree limit.
    const double query_min_latitude =
        static_cast<double>(query_bin.first) * kBinDegrees - 90.0;
    const double query_max_latitude = query_min_latitude + kBinDegrees;
    const double query_min_longitude =
        static_cast<double>(query_bin.second) * kBinDegrees - 180.0;
    const double query_max_longitude = query_min_longitude + kBinDegrees;
    const double extreme_latitude =
        std::max(std::abs(query_min_latitude), std::abs(query_max_latitude));
    const double longitude_scale = std::max(
        0.1, std::cos(extreme_latitude * 3.14159265358979323846 / 180.0));
    const double longitude_radius =
        kMaximumSampleDistanceDegrees / longitude_scale;
    const int minimum_latitude_bin = LatitudeBin(
        std::max(-90.0, query_min_latitude - kMaximumSampleDistanceDegrees));
    const int maximum_latitude_bin = LatitudeBin(
        std::min(90.0, query_max_latitude + kMaximumSampleDistanceDegrees));
    const int minimum_longitude_bin =
        LongitudeBin(std::max(-180.0, query_min_longitude - longitude_radius));
    const int maximum_longitude_bin =
        LongitudeBin(std::min(180.0, query_max_longitude + longitude_radius));
    auto result = std::make_shared<std::vector<std::uint32_t>>();
    for (int latitude_bin = minimum_latitude_bin;
         latitude_bin <= maximum_latitude_bin; ++latitude_bin) {
      for (int longitude_bin = minimum_longitude_bin;
           longitude_bin <= maximum_longitude_bin; ++longitude_bin) {
        const auto found = bins_.find({latitude_bin, longitude_bin});
        if (found == bins_.end()) continue;
        result->insert(result->end(), found->second.begin(),
                       found->second.end());
      }
    }
    std::sort(result->begin(), result->end());

    std::lock_guard<std::mutex> lock(plans_->mutex);
    const auto [found, inserted] = plans_->values.emplace(query_bin, result);
    if (!inserted) return found->second;
    plans_->lru.push_front(query_bin);
    constexpr std::size_t kMaximumCachedPlans = 512;
    while (plans_->lru.size() > kMaximumCachedPlans) {
      const BinKey old = plans_->lru.back();
      plans_->lru.pop_back();
      plans_->values.erase(old);
    }
    return result;
  }

  static int LatitudeBin(double latitude) {
    return static_cast<int>(std::floor((latitude + 90.0) / kBinDegrees));
  }

  static int LongitudeBin(double longitude) {
    return static_cast<int>(std::floor((longitude + 180.0) / kBinDegrees));
  }

  static BinKey Bin(double latitude, double longitude) {
    return {LatitudeBin(latitude), LongitudeBin(longitude)};
  }

  std::map<BinKey, std::vector<std::uint32_t>> bins_;
  mutable std::shared_ptr<PlanCache> plans_ = std::make_shared<PlanCache>();
};

inline bool BetterEnvironmentSample(double distance, std::size_t index,
                                    double best_distance,
                                    std::size_t best_index) {
  return distance < best_distance ||
         (distance == best_distance && index < best_index);
}

}  // namespace ppm

#endif
