#ifndef OCPN_PORTABLE_POLAR_H
#define OCPN_PORTABLE_POLAR_H

#include <filesystem>
#include <string>
#include <vector>

struct PortablePolarGrid {
  std::string identity;
  std::vector<double> true_wind_speeds_knots;
  std::vector<double> true_wind_angles_degrees;
  // Wind-speed-major: wind index * angle count + angle index.
  std::vector<double> boat_speeds_knots;
};

struct PortablePolarSet {
  std::filesystem::path source_path;
  std::vector<PortablePolarGrid> grids;
};

// Loads an OpenCPN weather-routing .pol table or boat .xml manifest. XML polar
// references are restricted to the selected boat directory and its sibling
// "polars" directory. The returned model contains values only; no file or XML
// objects cross the portable-plugin boundary.
bool LoadPortablePolarSet(const std::filesystem::path& selected_path,
                          PortablePolarSet* result, std::string* error);

#endif
