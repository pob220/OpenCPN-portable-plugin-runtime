#ifndef PORTABLE_PLUGIN_MANAGER_ENVIRONMENT_SERVICE_H
#define PORTABLE_PLUGIN_MANAGER_ENVIRONMENT_SERVICE_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "environment_spatial_index.h"

namespace ppm {

struct EnvironmentRequest {
  double latitude = 0.0;
  double longitude = 0.0;
  std::int64_t unix_time = 0;
};

struct EnvironmentSample {
  double wind_u_knots = 0.0;
  double wind_v_knots = 0.0;
  double current_u_knots = 0.0;
  double current_v_knots = 0.0;
  double wave_height_metres = 0.0;
  unsigned available = 0;
};

struct EnvironmentGridSample {
  double latitude = 0.0;
  double longitude = 0.0;
  double value = 0.0;
};

struct EnvironmentFrame {
  std::string time;
  std::size_t sample_count = 0;
  std::map<std::string, std::vector<EnvironmentGridSample>> fields;
  std::map<std::string, EnvironmentSpatialIndex> spatial_indices;
  std::map<std::string, std::string> units;
  std::map<std::string, std::string> source_times;
};

/**
 * Reads the helper-owned bounded OCPNFRM1 format. The parser rejects trailing
 * data, duplicate fields, implausible physical values, and oversized frames.
 */
bool ReadEnvironmentFrames(const std::string& path,
                           std::vector<EnvironmentFrame>* frames,
                           std::string* diagnostic);

std::shared_ptr<const EnvironmentFrame> InterpolateEnvironmentFrames(
    const std::shared_ptr<const EnvironmentFrame>& first,
    const std::shared_ptr<const EnvironmentFrame>& second,
    const std::string& requested_time, double factor);

/**
 * Samples one decoded frame at the nearest valid grid location. Vector
 * components must have matching coordinates and values farther than two
 * degrees from the request are considered unavailable.
 */
EnvironmentSample SampleEnvironmentFrame(const EnvironmentFrame& frame,
                                         double latitude, double longitude);

}  // namespace ppm

#endif
