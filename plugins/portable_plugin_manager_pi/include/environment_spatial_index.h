#ifndef PORTABLE_PLUGIN_MANAGER_ENVIRONMENT_SPATIAL_INDEX_H
#define PORTABLE_PLUGIN_MANAGER_ENVIRONMENT_SPATIAL_INDEX_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
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
  }

  template <typename Function>
  void ForEachCandidate(double latitude, double longitude,
                        Function&& function) const {
    if (bins_.empty() || !std::isfinite(latitude) ||
        !std::isfinite(longitude)) {
      return;
    }
    const double longitude_scale =
        std::max(0.1, std::cos(latitude * 3.14159265358979323846 / 180.0));
    const double longitude_radius =
        kMaximumSampleDistanceDegrees / longitude_scale;
    const int minimum_latitude_bin =
        LatitudeBin(std::max(-90.0, latitude - kMaximumSampleDistanceDegrees));
    const int maximum_latitude_bin =
        LatitudeBin(std::min(90.0, latitude + kMaximumSampleDistanceDegrees));
    const int minimum_longitude_bin =
        LongitudeBin(std::max(-180.0, longitude - longitude_radius));
    const int maximum_longitude_bin =
        LongitudeBin(std::min(180.0, longitude + longitude_radius));
    for (int latitude_bin = minimum_latitude_bin;
         latitude_bin <= maximum_latitude_bin; ++latitude_bin) {
      for (int longitude_bin = minimum_longitude_bin;
           longitude_bin <= maximum_longitude_bin; ++longitude_bin) {
        const auto found = bins_.find({latitude_bin, longitude_bin});
        if (found == bins_.end()) continue;
        for (const std::uint32_t index : found->second) function(index);
      }
    }
  }

  std::size_t EstimatedBytes() const {
    std::size_t bytes = sizeof(*this);
    for (const auto& [key, values] : bins_) {
      static_cast<void>(key);
      bytes += sizeof(key) + values.capacity() * sizeof(std::uint32_t);
    }
    return bytes;
  }

private:
  using BinKey = std::pair<int, int>;

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
};

inline bool BetterEnvironmentSample(double distance, std::size_t index,
                                    double best_distance,
                                    std::size_t best_index) {
  return distance < best_distance ||
         (distance == best_distance && index < best_index);
}

}  // namespace ppm

#endif
