#include "chart_safety_service.h"

#include <cmath>

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
      results[index] = ChartSafetyServiceResult{
          state, assessment.charts_considered, ReasonFor(assessment),
          assessment.diagnostic};
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
