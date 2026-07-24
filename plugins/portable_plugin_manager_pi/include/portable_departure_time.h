#ifndef OCPN_PORTABLE_DEPARTURE_TIME_H
#define OCPN_PORTABLE_DEPARTURE_TIME_H

#include <cstdint>
#include <cstddef>
#include <vector>

#include <wx/datetime.h>
#include <wx/string.h>

enum class PortableDepartureZoneKind {
  kUtc,
  kSystemLocal,
  kFixedOffset,
};

struct PortableDepartureZone {
  PortableDepartureZoneKind kind = PortableDepartureZoneKind::kUtc;
  int offset_minutes = 0;
};

bool operator==(const PortableDepartureZone& lhs,
                const PortableDepartureZone& rhs);

wxString PortableDepartureZoneSetting(const PortableDepartureZone& zone);
bool ParsePortableDepartureZoneSetting(const wxString& setting,
                                       PortableDepartureZone* zone);

wxString PortableSystemTimeZoneName();
wxString PortableDepartureZoneLabel(const PortableDepartureZone& zone,
                                    int64_t unix_time);

bool PortableDepartureToUnix(const wxDateTime& date, int hour, int minute,
                             const PortableDepartureZone& zone,
                             int64_t* unix_time, wxString* error);

wxDateTime PortableDepartureWallTime(int64_t unix_time,
                                     const PortableDepartureZone& zone);

wxString FormatPortableDepartureTime(int64_t unix_time,
                                     const PortableDepartureZone& zone);

// Returns chronological offsets spanning the requested range on both sides of
// the nominal departure. Zero is always present.
std::vector<int64_t> PortableDepartureOffsetsSeconds(int range_hours,
                                                     int spacing_hours);

// Returns indices in calculation order: nominal first, then increasingly
// distant alternatives. Results can remain in chronological offset order.
std::vector<size_t> PortableDepartureExecutionOrder(
    const std::vector<int64_t>& offsets_seconds);

#endif  // OCPN_PORTABLE_DEPARTURE_TIME_H
