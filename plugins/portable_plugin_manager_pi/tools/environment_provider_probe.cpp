#include "environment_provider.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 7) {
    std::cerr
        << "usage: ppm_environment_provider_probe PACKAGE_ROOT PRIVATE_ROOT "
           "GRIB LATITUDE LONGITUDE UNIX_TIME\n";
    return 2;
  }
  char* latitude_end = nullptr;
  char* longitude_end = nullptr;
  char* time_end = nullptr;
  const double latitude = std::strtod(argv[4], &latitude_end);
  const double longitude = std::strtod(argv[5], &longitude_end);
  const auto unix_time = std::strtoll(argv[6], &time_end, 10);
  if (!latitude_end || *latitude_end || !longitude_end || *longitude_end ||
      !time_end || *time_end) {
    std::cerr << "latitude, longitude, and UNIX_TIME must be numbers\n";
    return 2;
  }
  auto provider = std::make_unique<ppm::EnvironmentProvider>(argv[1], argv[2]);
  std::string diagnostic;
  if (!provider->OpenDataset({argv[3]}, {}, &diagnostic)) {
    std::cerr << diagnostic << '\n';
    return 1;
  }
  const std::vector<ppm::EnvironmentRequest> requests = {
      {latitude, longitude, unix_time}};
  std::vector<ppm::EnvironmentSample> samples;
  if (!provider->SampleBatch(requests, &samples, {}, &diagnostic) ||
      samples.size() != 1) {
    std::cerr << diagnostic << '\n';
    return 1;
  }
  const auto first = samples.front();
  // The second call exercises the decoded-frame cache.
  if (!provider->SampleBatch(requests, &samples, {}, &diagnostic) ||
      samples.size() != 1) {
    std::cerr << diagnostic << '\n';
    return 1;
  }
  const auto second = samples.front();
  if (first.available != second.available ||
      first.wind_u_knots != second.wind_u_knots ||
      first.wind_v_knots != second.wind_v_knots ||
      first.current_u_knots != second.current_u_knots ||
      first.current_v_knots != second.current_v_knots ||
      first.wave_height_metres != second.wave_height_metres) {
    std::cerr << "cached sample differs from initial sample\n";
    return 1;
  }
  const std::string summary = provider->Summary();
  provider.reset();
  provider = std::make_unique<ppm::EnvironmentProvider>(argv[1], argv[2]);
  if (!provider->Available() || provider->Summary() != summary ||
      !provider->SampleBatch(requests, &samples, {}, &diagnostic) ||
      samples.size() != 1 || samples.front().available != first.available) {
    std::cerr << "restored provider snapshot failed: " << diagnostic << '\n';
    return 1;
  }
  std::cout << summary << '\n'
            << "available=" << first.available
            << " wind_u_knots=" << first.wind_u_knots
            << " wind_v_knots=" << first.wind_v_knots
            << " current_u_knots=" << first.current_u_knots
            << " current_v_knots=" << first.current_v_knots
            << " wave_height_metres=" << first.wave_height_metres << '\n';
  return 0;
}
