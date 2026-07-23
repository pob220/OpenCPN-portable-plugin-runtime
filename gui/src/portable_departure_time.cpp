#include "portable_departure_time.h"

#include <array>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <system_error>

#include <wx/utils.h>

namespace {

int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned adjusted_month =
      static_cast<unsigned>(static_cast<int>(month) + (month > 2 ? -3 : 9));
  const unsigned day_of_year = (153 * adjusted_month + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(day_of_era) -
         719468;
}

int64_t CivilSeconds(const std::tm& value) {
  return DaysFromCivil(value.tm_year + 1900,
                       static_cast<unsigned>(value.tm_mon + 1),
                       static_cast<unsigned>(value.tm_mday)) *
             86400 +
         static_cast<int64_t>(value.tm_hour) * 3600 +
         static_cast<int64_t>(value.tm_min) * 60 + value.tm_sec;
}

bool LocalTime(time_t value, std::tm* result) {
  if (!result) return false;
#ifdef _WIN32
  return localtime_s(result, &value) == 0;
#else
  return localtime_r(&value, result) != nullptr;
#endif
}

bool UtcTime(time_t value, std::tm* result) {
  if (!result) return false;
#ifdef _WIN32
  return gmtime_s(result, &value) == 0;
#else
  return gmtime_r(&value, result) != nullptr;
#endif
}

bool SameWallTime(const std::tm& value, int year, int month, int day, int hour,
                  int minute) {
  return value.tm_year == year - 1900 && value.tm_mon == month - 1 &&
         value.tm_mday == day && value.tm_hour == hour &&
         value.tm_min == minute;
}

int LocalOffsetMinutes(int64_t unix_time) {
  const time_t value = static_cast<time_t>(unix_time);
  std::tm local{};
  std::tm utc{};
  if (!LocalTime(value, &local) || !UtcTime(value, &utc)) return 0;
  const int64_t seconds = CivilSeconds(local) - CivilSeconds(utc);
  return static_cast<int>(seconds / 60);
}

wxString OffsetText(int offset_minutes, bool prefix_utc) {
  const char sign = offset_minutes < 0 ? '-' : '+';
  const int absolute = std::abs(offset_minutes);
  return wxString::Format("%s%c%02d:%02d", prefix_utc ? "UTC" : "", sign,
                          absolute / 60, absolute % 60);
}

bool ParseOffset(const wxString& setting, int* offset_minutes) {
  if (!offset_minutes || setting.length() != 6 ||
      (setting[0] != '+' && setting[0] != '-') || setting[3] != ':')
    return false;
  long hours = 0;
  long minutes = 0;
  if (!setting.Mid(1, 2).ToLong(&hours) ||
      !setting.Mid(4, 2).ToLong(&minutes) || hours > 14 || minutes > 59 ||
      (hours == 14 && minutes != 0))
    return false;
  const int sign = setting[0] == '-' ? -1 : 1;
  *offset_minutes =
      sign * (static_cast<int>(hours) * 60 + static_cast<int>(minutes));
  return true;
}

}  // namespace

bool operator==(const PortableDepartureZone& lhs,
                const PortableDepartureZone& rhs) {
  return lhs.kind == rhs.kind && lhs.offset_minutes == rhs.offset_minutes;
}

wxString PortableDepartureZoneSetting(const PortableDepartureZone& zone) {
  if (zone.kind == PortableDepartureZoneKind::kSystemLocal) return "Local Time";
  if (zone.kind == PortableDepartureZoneKind::kFixedOffset)
    return OffsetText(zone.offset_minutes, false);
  return "UTC";
}

bool ParsePortableDepartureZoneSetting(const wxString& setting,
                                       PortableDepartureZone* zone) {
  if (!zone) return false;
  if (setting == "UTC") {
    *zone = {};
    return true;
  }
  if (setting == "Local Time") {
    *zone = {PortableDepartureZoneKind::kSystemLocal, 0};
    return true;
  }
  int offset_minutes = 0;
  if (!ParseOffset(setting, &offset_minutes)) return false;
  *zone = {PortableDepartureZoneKind::kFixedOffset, offset_minutes};
  return true;
}

wxString PortableSystemTimeZoneName() {
  static const wxString name = [] {
    wxString environment;
    wxGetEnv("TZ", &environment);
    if (!environment.empty() && !environment.StartsWith(":") &&
        environment.Find('/') != wxNOT_FOUND)
      return environment;
#ifndef _WIN32
    std::error_code error;
    const std::filesystem::path resolved =
        std::filesystem::canonical("/etc/localtime", error);
    if (!error) {
      const std::string path = resolved.generic_string();
      constexpr char marker[] = "/zoneinfo/";
      const size_t offset = path.find(marker);
      if (offset != std::string::npos)
        return wxString::FromUTF8(path.substr(offset + sizeof(marker) - 1));
    }
#endif
    const wxString abbreviation = wxDateTime::Now().Format("%Z");
    return abbreviation.empty() ? wxString("system timezone") : abbreviation;
  }();
  return name;
}

wxString PortableDepartureZoneLabel(const PortableDepartureZone& zone,
                                    int64_t unix_time) {
  if (zone.kind == PortableDepartureZoneKind::kUtc) return "UTC";
  if (zone.kind == PortableDepartureZoneKind::kFixedOffset)
    return OffsetText(zone.offset_minutes, true);
  return wxString::Format("%s (%s)", PortableSystemTimeZoneName(),
                          OffsetText(LocalOffsetMinutes(unix_time), true));
}

bool PortableDepartureToUnix(const wxDateTime& date, int hour, int minute,
                             const PortableDepartureZone& zone,
                             int64_t* unix_time, wxString* error) {
  if (!unix_time || !date.IsValid() || hour < 0 || hour > 23 || minute < 0 ||
      minute > 59) {
    if (error) *error = "Select a valid departure date and time";
    return false;
  }
  const int year = date.GetYear();
  const int month = static_cast<int>(date.GetMonth()) + 1;
  const int day = date.GetDay();
  const int64_t wall_seconds = DaysFromCivil(year, static_cast<unsigned>(month),
                                             static_cast<unsigned>(day)) *
                                   86400 +
                               static_cast<int64_t>(hour) * 3600 + minute * 60;
  if (zone.kind != PortableDepartureZoneKind::kSystemLocal) {
    *unix_time = wall_seconds - static_cast<int64_t>(
                                    zone.kind == PortableDepartureZoneKind::kUtc
                                        ? 0
                                        : zone.offset_minutes) *
                                    60;
    return true;
  }

  std::array<time_t, 2> candidates{};
  size_t candidate_count = 0;
  for (const int daylight : {0, 1}) {
    std::tm fields{};
    fields.tm_year = year - 1900;
    fields.tm_mon = month - 1;
    fields.tm_mday = day;
    fields.tm_hour = hour;
    fields.tm_min = minute;
    fields.tm_isdst = daylight;
    const time_t candidate = std::mktime(&fields);
    std::tm round_trip{};
    if (!LocalTime(candidate, &round_trip) ||
        !SameWallTime(round_trip, year, month, day, hour, minute))
      continue;
    if (candidate_count == 0 || candidates[0] != candidate)
      candidates[candidate_count++] = candidate;
  }
  if (candidate_count == 0) {
    if (error)
      *error =
          "That local clock time does not exist because the clocks change; "
          "choose another time or select UTC";
    return false;
  }
  if (candidate_count > 1) {
    if (error)
      *error =
          "That local clock time occurs twice because the clocks change; "
          "select UTC or a fixed offset to make it unambiguous";
    return false;
  }
  *unix_time = static_cast<int64_t>(candidates[0]);
  return true;
}

wxDateTime PortableDepartureWallTime(int64_t unix_time,
                                     const PortableDepartureZone& zone) {
  if (zone.kind == PortableDepartureZoneKind::kSystemLocal)
    return wxDateTime(static_cast<time_t>(unix_time));
  const int offset = zone.kind == PortableDepartureZoneKind::kFixedOffset
                         ? zone.offset_minutes
                         : 0;
  return wxDateTime(
             static_cast<time_t>(unix_time + static_cast<int64_t>(offset) * 60))
      .ToUTC();
}

wxString FormatPortableDepartureTime(int64_t unix_time,
                                     const PortableDepartureZone& zone) {
  const wxDateTime wall = PortableDepartureWallTime(unix_time, zone);
  return wxString::Format("%s %s", wall.Format("%d %b %Y %H:%M"),
                          PortableDepartureZoneLabel(zone, unix_time));
}
