#include "wind_barb_geometry.h"

#include <cmath>
#include <iostream>
#include <limits>

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "check failed at " << __FILE__ << ':' << __LINE__ << ": "   \
                << #condition << '\n';                                           \
      return 1;                                                                 \
    }                                                                           \
  } while (false)

int main() {
  const auto calm = ppm::BuildWindBarbGeometry(0.0, 0.0);
  CHECK(calm.visible);
  CHECK(calm.calm);
  CHECK(calm.lines.empty());
  CHECK(calm.pennants.empty());

  const auto just_calm = ppm::BuildWindBarbGeometry(2.49, 0.0);
  CHECK(just_calm.calm);
  const auto first_half = ppm::BuildWindBarbGeometry(2.5, 0.0);
  CHECK(!first_half.calm);
  CHECK(first_half.rounded_speed_knots == 5);
  CHECK(first_half.lines.size() == 2);

  const auto four = ppm::BuildWindBarbGeometry(4.0, 0.0);
  CHECK(!four.calm);
  CHECK(four.rounded_speed_knots == 5);
  CHECK(four.lines.size() == 2);  // Staff plus one half feather.
  CHECK(four.pennants.empty());

  const auto below_ten = ppm::BuildWindBarbGeometry(7.49, 0.0);
  CHECK(below_ten.rounded_speed_knots == 5);
  CHECK(below_ten.lines.size() == 2);
  const auto first_full = ppm::BuildWindBarbGeometry(7.5, 0.0);
  CHECK(first_full.rounded_speed_knots == 10);
  CHECK(first_full.lines.size() == 2);

  const auto eleven = ppm::BuildWindBarbGeometry(0.0, 11.0);
  CHECK(eleven.rounded_speed_knots == 10);
  CHECK(eleven.lines.size() == 2);  // Staff plus one full feather.
  CHECK(eleven.pennants.empty());

  const auto sixteen = ppm::BuildWindBarbGeometry(16.0, 0.0);
  CHECK(sixteen.rounded_speed_knots == 15);
  CHECK(sixteen.lines.size() == 3);  // Staff, full and half feather.
  CHECK(sixteen.pennants.empty());

  const auto below_fifty = ppm::BuildWindBarbGeometry(47.49, 0.0);
  CHECK(below_fifty.rounded_speed_knots == 45);
  CHECK(below_fifty.lines.size() == 6);  // Staff, four full and one half.
  CHECK(below_fifty.pennants.empty());
  const auto first_pennant = ppm::BuildWindBarbGeometry(47.5, 0.0);
  CHECK(first_pennant.rounded_speed_knots == 50);
  CHECK(first_pennant.lines.size() == 1);
  CHECK(first_pennant.pennants.size() == 1);

  const auto fifty_five = ppm::BuildWindBarbGeometry(0.0, -55.0);
  CHECK(fifty_five.rounded_speed_knots == 55);
  CHECK(fifty_five.lines.size() == 2);  // Staff plus one half feather.
  CHECK(fifty_five.pennants.size() == 1);

  const auto one_hundred = ppm::BuildWindBarbGeometry(100.0, 0.0);
  CHECK(one_hundred.rounded_speed_knots == 100);
  CHECK(one_hundred.lines.size() == 1);  // Staff only; two pennants.
  CHECK(one_hundred.pennants.size() == 2);

  const auto eastward = ppm::BuildWindBarbGeometry(10.0, 0.0);
  CHECK(std::abs(eastward.lines.front().end.x + 24.0) < 1e-9);
  CHECK(std::abs(eastward.lines.front().end.y) < 1e-9);

  const auto invalid = ppm::BuildWindBarbGeometry(
      std::numeric_limits<double>::quiet_NaN(), 0.0);
  CHECK(!invalid.visible);

  std::cout << "wind barb geometry test passed\n";
  return 0;
}
