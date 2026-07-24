#include "portable_departure_time.h"

#include <cstdlib>
#include <iostream>

namespace {

bool Check(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

}  // namespace

int main() {
  PortableDepartureZone zone;
  if (!Check(ParsePortableDepartureZoneSetting("UTC", &zone) &&
                 zone.kind == PortableDepartureZoneKind::kUtc,
             "UTC setting was not parsed") ||
      !Check(ParsePortableDepartureZoneSetting("+05:45", &zone) &&
                 zone.kind == PortableDepartureZoneKind::kFixedOffset &&
                 zone.offset_minutes == 345,
             "quarter-hour fixed offset was not parsed") ||
      !Check(PortableDepartureZoneSetting(zone) == "+05:45",
             "fixed offset did not round trip") ||
      !Check(!ParsePortableDepartureZoneSetting("+14:30", &zone),
             "invalid offset beyond UTC+14 was accepted"))
    return 1;

  wxDateTime date(23, wxDateTime::Jul, 2026);
  int64_t unix_time = 0;
  wxString error;
  const PortableDepartureZone utc{};
  if (!Check(PortableDepartureToUnix(date, 10, 0, utc, &unix_time, &error),
             "UTC departure did not convert") ||
      !Check(unix_time == 1784800800, "UTC departure converted incorrectly") ||
      !Check(FormatPortableDepartureTime(unix_time, utc) ==
                 "23 Jul 2026 10:00 UTC",
             "UTC departure did not format clearly"))
    return 1;

  const PortableDepartureZone nepal{PortableDepartureZoneKind::kFixedOffset,
                                    345};
  if (!Check(PortableDepartureToUnix(date, 15, 45, nepal, &unix_time, &error),
             "fixed-offset departure did not convert") ||
      !Check(unix_time == 1784800800,
             "fixed-offset departure converted incorrectly") ||
      !Check(FormatPortableDepartureTime(unix_time, nepal) ==
                 "23 Jul 2026 15:45 UTC+05:45",
             "fixed-offset departure did not format clearly"))
    return 1;

  const std::vector<int64_t> offsets = PortableDepartureOffsetsSeconds(6, 2);
  const std::vector<int64_t> expected_offsets = {-21600, -14400, -7200, 0,
                                                 7200,   14400,  21600};
  const std::vector<size_t> expected_order = {3, 2, 4, 1, 5, 0, 6};
  if (!Check(offsets == expected_offsets,
             "symmetric departure offsets were not generated") ||
      !Check(PortableDepartureExecutionOrder(offsets) == expected_order,
             "departure execution did not start nominal then expand") ||
      !Check(PortableDepartureOffsetsSeconds(5, 2) ==
                 std::vector<int64_t>({-14400, -7200, 0, 7200, 14400}),
             "a partial range produced asymmetric departure offsets") ||
      !Check(PortableDepartureOffsetsSeconds(0, 2) == std::vector<int64_t>({0}),
             "disabled departure comparison did not retain nominal time"))
    return 1;

#ifndef _WIN32
  setenv("TZ", "Europe/London", 1);
  tzset();
  const PortableDepartureZone local{PortableDepartureZoneKind::kSystemLocal, 0};
  if (!Check(PortableDepartureToUnix(date, 10, 0, local, &unix_time, &error),
             "DST-aware local departure did not convert") ||
      !Check(unix_time == 1784797200,
             "British summer departure did not convert to UTC") ||
      !Check(FormatPortableDepartureTime(unix_time, local)
                 .Contains("10:00 Europe/London (UTC+01:00)"),
             "system timezone was not displayed with its effective offset")) {
    unsetenv("TZ");
    tzset();
    return 1;
  }

  wxDateTime skipped_date(29, wxDateTime::Mar, 2026);
  if (!Check(!PortableDepartureToUnix(skipped_date, 1, 30, local, &unix_time,
                                      &error) &&
                 error.Contains("does not exist"),
             "nonexistent DST wall time was not rejected")) {
    unsetenv("TZ");
    tzset();
    return 1;
  }
  wxDateTime repeated_date(25, wxDateTime::Oct, 2026);
  if (!Check(!PortableDepartureToUnix(repeated_date, 1, 30, local, &unix_time,
                                      &error) &&
                 error.Contains("occurs twice"),
             "ambiguous DST wall time was not rejected")) {
    unsetenv("TZ");
    tzset();
    return 1;
  }
  unsetenv("TZ");
  tzset();
#endif

  std::cout << "portable departure time test passed\n";
  return 0;
}
