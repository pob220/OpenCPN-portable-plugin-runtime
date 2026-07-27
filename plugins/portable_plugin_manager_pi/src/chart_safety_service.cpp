#include "chart_safety_service.h"

#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "bounded_parallel.h"
#include "cm93_semantic_reader.h"
#include "ocpn_plugin.h"

namespace ppm {

namespace {

bool ValidPoint(const ocpn_portable_geo_point& point) {
  return std::isfinite(point.latitude) && std::isfinite(point.longitude) &&
         point.latitude >= -90.0 && point.latitude <= 90.0 &&
         point.longitude >= -180.0 && point.longitude <= 180.0;
}

Cm93SemanticReader& Cm93Reader() {
  static Cm93SemanticReader reader;
  return reader;
}

struct SemanticCacheKey {
  std::uint64_t start_latitude = 0;
  std::uint64_t start_longitude = 0;
  std::uint64_t end_latitude = 0;
  std::uint64_t end_longitude = 0;
  std::uint64_t margin = 0;
  std::uint64_t depth = 0;
  bool authoritative = false;

  bool operator==(const SemanticCacheKey& other) const {
    return start_latitude == other.start_latitude &&
           start_longitude == other.start_longitude &&
           end_latitude == other.end_latitude &&
           end_longitude == other.end_longitude &&
           margin == other.margin && depth == other.depth &&
           authoritative == other.authoritative;
  }
};

struct SemanticCacheKeyHash {
  std::size_t operator()(const SemanticCacheKey& key) const {
    std::size_t value = static_cast<std::size_t>(key.start_latitude);
    auto combine = [&value](const std::uint64_t part) {
      value ^= static_cast<std::size_t>(part) + 0x9e3779b97f4a7c15ULL +
               (value << 6U) + (value >> 2U);
    };
    combine(key.start_longitude);
    combine(key.end_latitude);
    combine(key.end_longitude);
    combine(key.margin);
    combine(key.depth);
    combine(key.authoritative ? 1U : 0U);
    return value;
  }
};

std::uint64_t DoubleBits(const double value) {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "unexpected double size");
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

SemanticCacheKey MakeSemanticCacheKey(
    const ocpn_portable_geo_segment& segment,
    const ChartSafetyServiceOptions& options) {
  const ocpn_portable_geo_point* start = &segment.start;
  const ocpn_portable_geo_point* end = &segment.end;
  if (std::tie(end->latitude, end->longitude) <
      std::tie(start->latitude, start->longitude))
    std::swap(start, end);
  return {DoubleBits(start->latitude),
          DoubleBits(start->longitude),
          DoubleBits(end->latitude),
          DoubleBits(end->longitude),
          DoubleBits(options.safety_margin_nautical_miles),
          DoubleBits(options.minimum_depth_metres),
          options.require_authoritative};
}

class SemanticResultCache {
public:
  // Results are immutable for the configured chart snapshot and are cleared
  // by ConfigureChartRoots before a replacement snapshot becomes visible.
  bool Get(const SemanticCacheKey& key, ChartSafetyServiceResult* result) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end()) return false;
    *result = found->second;
    return true;
  }

  void Put(const SemanticCacheKey& key,
           const ChartSafetyServiceResult& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(key);
    if (found != entries_.end()) {
      found->second = result;
      return;
    }
    while (entries_.size() >= kMaximumEntries && !insertion_order_.empty()) {
      entries_.erase(insertion_order_.front());
      insertion_order_.pop_front();
    }
    entries_.emplace(key, result);
    insertion_order_.push_back(key);
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    insertion_order_.clear();
  }

private:
  static constexpr std::size_t kMaximumEntries = 65536;

  std::mutex mutex_;
  std::unordered_map<SemanticCacheKey, ChartSafetyServiceResult,
                     SemanticCacheKeyHash>
      entries_;
  std::deque<SemanticCacheKey> insertion_order_;
};

SemanticResultCache& ResultCache() {
  static SemanticResultCache cache;
  return cache;
}

std::uint32_t ReasonFor(const SemanticSegmentAssessment& assessment) {
  if (assessment.state == SemanticSegmentAssessment::State::kSafe) return 0U;
  if (assessment.diagnostic.find("LNDARE") != std::string::npos) return 1U;
  if (assessment.diagnostic.find("DRGARE") != std::string::npos ||
      assessment.diagnostic.find("ITDARE") != std::string::npos)
    return 2U;
  if (assessment.diagnostic.find("below required") != std::string::npos)
    return 3U;
  if (assessment.diagnostic.find("minimum depth") != std::string::npos)
    return 4U;
  if (assessment.state == SemanticSegmentAssessment::State::kMissingCoverage)
    return 5U;
  return 6U;
}

}  // namespace

void ChartSafetyService::ConfigureChartRoots(
    const std::vector<std::string>& roots) {
  ResultCache().Clear();
  Cm93Reader().SetChartRoots(roots);
}

bool ChartSafetyService::AuthoritativeAvailable() const {
  return Cm93Reader().Available();
}

std::string ChartSafetyService::Summary() const {
  return AuthoritativeAvailable()
             ? Cm93Reader().Summary()
             : "Authoritative chart-object/depth safety is not yet indexed; "
               "only advisory GSHHS coastline checks are available";
}

std::vector<ChartSafetyServiceResult> ChartSafetyService::QuerySemantic(
    const std::vector<ocpn_portable_geo_segment>& segments,
    const ChartSafetyServiceOptions& options) const {
  std::vector<ChartSafetyServiceResult> results(segments.size());
  const bool authoritative = AuthoritativeAvailable();
  BoundedParallelFor(segments.size(), 128, [&](const std::size_t index) {
    const auto& segment = segments[index];
    if (!ValidPoint(segment.start) || !ValidPoint(segment.end) ||
        !std::isfinite(options.safety_margin_nautical_miles) ||
        !std::isfinite(options.minimum_depth_metres) ||
        options.safety_margin_nautical_miles < 0.0 ||
        options.minimum_depth_metres < 0.0) {
      results[index] = ChartSafetyServiceResult{
          3U, 0U, 6U, "invalid final chart-safety geometry or options"};
      return;
    }
    if (authoritative) {
      const auto cache_key = MakeSemanticCacheKey(segment, options);
      if (ResultCache().Get(cache_key, &results[index])) return;
      const auto assessment = Cm93Reader().QuerySegment(
          {segment.start.latitude, segment.start.longitude},
          {segment.end.latitude, segment.end.longitude},
          options.safety_margin_nautical_miles, options.minimum_depth_metres);
      std::uint32_t state = 3U;
      switch (assessment.state) {
        case SemanticSegmentAssessment::State::kSafe:
          state = 0U;
          break;
        case SemanticSegmentAssessment::State::kUnsafe:
          state = 1U;
          break;
        case SemanticSegmentAssessment::State::kMissingCoverage:
          state = 2U;
          break;
        case SemanticSegmentAssessment::State::kError:
          state = 3U;
          break;
      }
      const ChartSafetyServiceResult service_result{
          state, assessment.charts_considered, ReasonFor(assessment),
          assessment.diagnostic};
      ResultCache().Put(cache_key, service_result);
      results[index] = service_result;
      return;
    }
    results[index] = ChartSafetyServiceResult{
        2U, 0U, 5U,
        "authoritative semantic chart-object and depth safety is unavailable"};
  });
  return results;
}

std::vector<ChartSafetyServiceResult>
ChartSafetyService::QueryAdvisoryCoastline(
    const std::vector<ocpn_portable_geo_segment>& segments) const {
  std::vector<ChartSafetyServiceResult> results;
  results.reserve(segments.size());
  for (const auto& segment : segments) {
    if (!ValidPoint(segment.start) || !ValidPoint(segment.end)) {
      results.push_back({3U, 0U, 6U, "invalid advisory chart-safety geometry"});
      continue;
    }
    const bool crosses_land = PlugIn_GSHHS_CrossesLand(
        segment.start.latitude, segment.start.longitude, segment.end.latitude,
        segment.end.longitude);
    results.push_back(
        {crosses_land ? 1U : 0U, 0U, crosses_land ? 1U : 0U,
         crosses_land
             ? "advisory GSHHS coastline intersection"
             : "advisory GSHHS coastline fallback found no intersection"});
  }
  return results;
}

}  // namespace ppm
