#ifndef PORTABLE_PLUGIN_MANAGER_WIND_BARB_GEOMETRY_H
#define PORTABLE_PLUGIN_MANAGER_WIND_BARB_GEOMETRY_H

#include <array>
#include <vector>

namespace ppm {

struct WindBarbPoint {
  double x = 0.0;
  double y = 0.0;
};

struct WindBarbLine {
  WindBarbPoint start;
  WindBarbPoint end;
};

struct WindBarbGeometry {
  bool visible = false;
  bool calm = false;
  double calm_radius = 3.0;
  int rounded_speed_knots = 0;
  std::vector<WindBarbLine> lines;
  std::vector<std::array<WindBarbPoint, 3>> pennants;
};

/**
 * Build backend-neutral wind-barb geometry centred on (0, 0).
 *
 * east_knots and north_knots are the vector components towards which the
 * wind is moving. The staff points into the direction from which the wind
 * comes. Speed is rounded to the nearest five knots using conventional
 * 5-knot half feathers, 10-knot full feathers and 50-knot pennants.
 */
WindBarbGeometry BuildWindBarbGeometry(double east_knots,
                                       double north_knots);

}  // namespace ppm

#endif
