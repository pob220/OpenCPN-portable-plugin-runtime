#include "cm93_semantic_reader.h"

#include <cassert>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
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

  // A safe tile may be certified only after an exact query has already
  // loaded every CM93 cell touching it. The second query exercises the
  // no-extra-decode fast path; land/depth queries above must never use it.
  const ppm::SemanticGeoPoint open_sea_west{53.4500, -5.5800};
  const ppm::SemanticGeoPoint open_sea_east{53.4600, -5.5200};
  const auto first_open_sea =
      reader.QuerySegment(open_sea_west, open_sea_east, 0.0, 0.0);
  const auto cached_open_sea =
      reader.QuerySegment(open_sea_west, open_sea_east, 0.0, 0.0);
  if (first_open_sea.state != ppm::SemanticSegmentAssessment::State::kSafe ||
      cached_open_sea.state !=
          ppm::SemanticSegmentAssessment::State::kSafe ||
      cached_open_sea.diagnostic.find("tile certificate") ==
          std::string::npos) {
    std::cerr << "Expected repeat open-sea query to use a safe certificate: "
              << first_open_sea.diagnostic << " / "
              << cached_open_sea.diagnostic << '\n';
    return 1;
  }

  // Departure-time optimisation may run several route searches in parallel.
  // Exercise the shared immutable dictionary and decoded-cell cache under the
  // same concurrent read pattern.
  std::atomic_bool concurrent_queries_passed{true};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&] {
      for (int query = 0; query < 4; ++query) {
        const auto land = reader.QuerySegment(west_of_south_stack,
                                              holyhead_harbour, 0.0, 0.0);
        const auto safe = reader.QuerySegment(holyhead_offshore,
                                              dun_laoghaire_offshore, 0.0, 2.0);
        if (land.state != ppm::SemanticSegmentAssessment::State::kUnsafe ||
            safe.state != ppm::SemanticSegmentAssessment::State::kSafe) {
          concurrent_queries_passed = false;
        }
      }
    });
  }
  for (auto& worker : workers) worker.join();
  if (!concurrent_queries_passed) {
    std::cerr << "Concurrent CM93 semantic queries were inconsistent\n";
    return 1;
  }
  return 0;
}
