#include "cm93_semantic_reader.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main() {
  ppm::Cm93SemanticReader reader;
  assert(!reader.Available());

  const char* configured_root = std::getenv("PPM_TEST_CM93_ROOT");
  std::filesystem::path root =
      configured_root ? configured_root : "/home/paul/Charts/cm93_2015";
  if (!std::filesystem::is_regular_file(root / "CM93OBJ.DIC") &&
      !std::filesystem::is_regular_file(root / "cm93obj.dic")) {
    std::cout << "CM93 fixture is not installed; decoder smoke test skipped\n";
    return 0;
  }

  reader.SetChartRoots({root.string()});
  assert(reader.Available());

  // This crosses Holy Island immediately east of South Stack. It is a
  // deterministic regression for the land crossing which the public crude
  // GSHHS service failed to reject in iWeatherRouting.
  const ppm::SemanticGeoPoint west_of_south_stack{53.3020, -4.7350};
  const ppm::SemanticGeoPoint holyhead_harbour{53.3120, -4.6150};
  const auto assessment =
      reader.QuerySegment(west_of_south_stack, holyhead_harbour, 0.0, 0.0);
  if (assessment.state != ppm::SemanticSegmentAssessment::State::kUnsafe) {
    std::cerr << "Expected Holy Island crossing to be unsafe, got: "
              << assessment.diagnostic << " (charts "
              << assessment.charts_considered << ")\n";
    return 1;
  }
  std::cout << assessment.diagnostic << '\n';

  const ppm::SemanticGeoPoint holyhead_offshore{53.341000, -4.620887};
  const ppm::SemanticGeoPoint dun_laoghaire_offshore{53.311102, -6.122185};
  const auto reported_route =
      reader.QuerySegment(holyhead_offshore, dun_laoghaire_offshore, 0.0, 2.0);
  if (reported_route.state != ppm::SemanticSegmentAssessment::State::kSafe) {
    std::cerr << "Expected the direct offshore route to be chart-safe, got: "
              << reported_route.diagnostic << " (charts "
              << reported_route.charts_considered << ")\n";
    return 1;
  }
  std::cout << reported_route.diagnostic << '\n';
  return 0;
}
