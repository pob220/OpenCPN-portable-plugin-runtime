#ifndef PORTABLE_PLUGIN_MANAGER_CM93_SEMANTIC_READER_H
#define PORTABLE_PLUGIN_MANAGER_CM93_SEMANTIC_READER_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ppm {

struct SemanticGeoPoint {
  double latitude = 0.0;
  double longitude = 0.0;
};

struct SemanticSegmentAssessment {
  enum class State { kSafe, kUnsafe, kMissingCoverage, kError };

  State state = State::kError;
  std::uint32_t charts_considered = 0;
  std::string diagnostic;
};

/**
 * Small, read-only CM93 semantic decoder used by the standard manager plugin.
 *
 * It deliberately does not use OpenCPN renderer or chart-object internals.
 * Decoded cells are immutable and cached by pathname and modification time.
 */
class Cm93SemanticReader {
public:
  Cm93SemanticReader();
  ~Cm93SemanticReader();

  Cm93SemanticReader(const Cm93SemanticReader&) = delete;
  Cm93SemanticReader& operator=(const Cm93SemanticReader&) = delete;

  void SetChartRoots(const std::vector<std::string>& roots);
  bool Available() const;
  std::string Summary() const;

  SemanticSegmentAssessment QuerySegment(const SemanticGeoPoint& start,
                                         const SemanticGeoPoint& end,
                                         double safety_margin_nautical_miles,
                                         double minimum_depth_metres) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ppm

#endif
