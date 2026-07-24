#include "wind_barb_geometry.h"

#include <cmath>

namespace ppm {

WindBarbGeometry BuildWindBarbGeometry(double east_knots,
                                       double north_knots) {
  WindBarbGeometry geometry;
  const double speed = std::hypot(east_knots, north_knots);
  if (!std::isfinite(speed)) return geometry;

  geometry.visible = true;
  if (speed < 2.5) {
    geometry.calm = true;
    return geometry;
  }

  const double staff_x = -east_knots / speed;
  const double staff_y = north_knots / speed;
  const double perpendicular_x = -staff_y;
  const double perpendicular_y = staff_x;
  constexpr double kStaffLength = 24.0;
  const WindBarbPoint tip{staff_x * kStaffLength, staff_y * kStaffLength};
  geometry.lines.push_back({{}, tip});

  int remaining = static_cast<int>(std::floor((speed + 2.5) / 5.0)) * 5;
  geometry.rounded_speed_knots = remaining;
  double offset = 0.0;
  while (remaining >= 50) {
    const WindBarbPoint first{tip.x - staff_x * offset,
                              tip.y - staff_y * offset};
    const WindBarbPoint second{tip.x - staff_x * (offset + 5.0),
                               tip.y - staff_y * (offset + 5.0)};
    const WindBarbPoint outer{first.x + perpendicular_x * 10.0 + staff_x * 3.0,
                              first.y + perpendicular_y * 10.0 +
                                  staff_y * 3.0};
    geometry.pennants.push_back({first, second, outer});
    remaining -= 50;
    offset += 7.0;
  }
  while (remaining >= 10) {
    const WindBarbPoint base{tip.x - staff_x * offset,
                             tip.y - staff_y * offset};
    const WindBarbPoint end{base.x + perpendicular_x * 10.0 + staff_x * 3.0,
                            base.y + perpendicular_y * 10.0 + staff_y * 3.0};
    geometry.lines.push_back({base, end});
    remaining -= 10;
    offset += 5.0;
  }
  if (remaining >= 5) {
    const WindBarbPoint base{tip.x - staff_x * (offset + 1.5),
                             tip.y - staff_y * (offset + 1.5)};
    const WindBarbPoint end{base.x + perpendicular_x * 6.0 + staff_x * 2.0,
                            base.y + perpendicular_y * 6.0 + staff_y * 2.0};
    geometry.lines.push_back({base, end});
  }
  return geometry;
}

}  // namespace ppm
