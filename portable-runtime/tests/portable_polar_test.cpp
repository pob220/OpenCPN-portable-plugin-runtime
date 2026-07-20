#include "portable_polar.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
bool Close(double first, double second) {
  return std::abs(first - second) < 1e-9;
}
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: portable_polar_test <Nicholson35.pol>\n";
    return 2;
  }
  PortablePolarSet direct;
  std::string error;
  if (!LoadPortablePolarSet(argv[1], &direct, &error)) {
    std::cerr << "direct polar failed: " << error << '\n';
    return 1;
  }
  if (direct.grids.size() != 1 ||
      direct.grids[0].true_wind_speeds_knots.size() != 15 ||
      direct.grids[0].true_wind_angles_degrees.size() != 16 ||
      direct.grids[0].boat_speeds_knots.size() != 240 ||
      !Close(direct.grids[0].boat_speeds_knots[5 * 16 + 3], 3.86)) {
    std::cerr << "Nicholson grid dimensions or orientation are incorrect\n";
    return 1;
  }

  const auto unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto root =
      std::filesystem::temp_directory_path() / ("portable-polar-test-" + unique);
  const auto boats = root / "boats";
  const auto polars = root / "polars";
  std::filesystem::create_directories(boats);
  std::filesystem::create_directories(polars);
  std::filesystem::copy_file(argv[1], polars / "Nicholson.pol");
  {
    std::ofstream xml(boats / "Nicholson.xml");
    xml << "<?xml version=\"1.0\"?>\n"
           "<OpenCPNWeatherRoutingBoat version=\"1.10\">\n"
           "  <Polar FileName=\"Nicholson.pol\" "
           "CrossOverPercentage=\"10\"/>\n"
           "</OpenCPNWeatherRoutingBoat>\n";
  }
  PortablePolarSet boat;
  if (!LoadPortablePolarSet(boats / "Nicholson.xml", &boat, &error) ||
      boat.grids.size() != 1 ||
      !Close(boat.grids[0].boat_speeds_knots[5 * 16 + 3], 3.86)) {
    std::cerr << "boat XML failed: " << error << '\n';
    std::filesystem::remove_all(root);
    return 1;
  }
  {
    std::ofstream bad(boats / "bad.pol");
    bad << "twa/tws;10;5\n40;3;2\n90;4;3\n";
  }
  PortablePolarSet rejected;
  error.clear();
  if (LoadPortablePolarSet(boats / "bad.pol", &rejected, &error) ||
      error.find("strictly increasing") == std::string::npos) {
    std::cerr << "invalid polar was not rejected: " << error << '\n';
    std::filesystem::remove_all(root);
    return 1;
  }
  std::filesystem::remove_all(root);
  std::cout << "portable polar parser test passed\n";
  return 0;
}
