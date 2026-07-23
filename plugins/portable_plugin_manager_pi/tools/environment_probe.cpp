#include "environment_service.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: ppm_environment_probe FRAME_BIN LATITUDE LONGITUDE\n";
    return 2;
  }
  char* latitude_end = nullptr;
  char* longitude_end = nullptr;
  const double latitude = std::strtod(argv[2], &latitude_end);
  const double longitude = std::strtod(argv[3], &longitude_end);
  if (!latitude_end || *latitude_end || !longitude_end || *longitude_end) {
    std::cerr << "latitude and longitude must be numbers\n";
    return 2;
  }
  std::vector<ppm::EnvironmentFrame> frames;
  std::string diagnostic;
  if (!ppm::ReadEnvironmentFrames(argv[1], &frames, &diagnostic)) {
    std::cerr << diagnostic << '\n';
    return 1;
  }
  std::cout << std::fixed << std::setprecision(6);
  for (const auto& frame : frames) {
    const auto sample =
        ppm::SampleEnvironmentFrame(frame, latitude, longitude);
    std::cout << "{\"time\":\"" << frame.time << "\",\"available\":"
              << sample.available << ",\"wind_u_knots\":"
              << sample.wind_u_knots << ",\"wind_v_knots\":"
              << sample.wind_v_knots << ",\"current_u_knots\":"
              << sample.current_u_knots << ",\"current_v_knots\":"
              << sample.current_v_knots << ",\"wave_height_metres\":"
              << sample.wave_height_metres << "}\n";
  }
  return 0;
}
