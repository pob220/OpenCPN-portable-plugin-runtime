#ifndef PORTABLE_PLUGIN_MANAGER_CHART_SAFETY_SERVICE_H
#define PORTABLE_PLUGIN_MANAGER_CHART_SAFETY_SERVICE_H

#include <cstdint>
#include <string>
#include <vector>

#include "ocpn_portable_runtime.h"

namespace ppm {

struct ChartSafetyServiceOptions {
  double safety_margin_nautical_miles = 0.0;
  double minimum_depth_metres = 0.0;
  bool require_authoritative = true;
};

struct ChartSafetyServiceResult {
  std::uint32_t state = 3;
  std::uint32_t charts_considered = 0;
  std::uint32_t reason = 6;
  std::string diagnostic;
};

/**
 * Host-owned chart safety capability.
 *
 * The service is deliberately independent of chart rendering. Portable
 * components submit value geometry and receive value results; chart objects,
 * parsers and caches remain inside the native manager plugin.
 */
class ChartSafetyService {
public:
  static void ConfigureChartRoots(const std::vector<std::string>& roots);

  bool AuthoritativeAvailable() const;
  std::string Summary() const;

  std::vector<ChartSafetyServiceResult> QueryFinal(
      const std::vector<ocpn_portable_geo_segment>& segments,
      const ChartSafetyServiceOptions& options) const;
};

}  // namespace ppm

#endif
