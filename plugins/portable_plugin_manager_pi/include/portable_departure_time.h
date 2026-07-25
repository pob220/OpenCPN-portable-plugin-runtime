#ifndef OCPN_PORTABLE_DEPARTURE_TIME_H
#define OCPN_PORTABLE_DEPARTURE_TIME_H

#include <cstddef>
#include <cstdint>
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

constexpr size_t kPortableMaximumDepartureCandidates = 73;
constexpr int kPortableMinimumRoutingTimeStepSeconds = 5 * 60;
constexpr int kPortableMaximumRoutingTimeStepSeconds = 6 * 60 * 60;

struct PortableRoutingTimeStep {
  int hours = 1;
  int minutes = 0;
};

// Converts the legacy seconds setting into the hours-and-minutes form used by
// the routing UI. Values are rounded to the nearest minute and kept within the
// solver's supported five-minute to six-hour range.
PortableRoutingTimeStep PortableRoutingTimeStepFromSeconds(int seconds);

// Converts the UI representation back to the canonical seconds value used by
// the solver and persisted settings. Returns zero for an invalid duration.
int PortableRoutingTimeStepSeconds(int hours, int minutes);

// Returns chronological offsets spanning the requested minute range on both
// sides of the nominal departure. Zero is always present. An empty result means
// that the requested range and spacing exceed the candidate cap.
std::vector<int64_t> PortableDepartureOffsetsSeconds(
    int range_minutes, int spacing_minutes,
    size_t maximum_candidates = kPortableMaximumDepartureCandidates);

// Returns indices in calculation order: nominal first, then increasingly
// distant alternatives. Results can remain in chronological offset order.
std::vector<size_t> PortableDepartureExecutionOrder(
    const std::vector<int64_t>& offsets_seconds);

// Selects a resource-safe number of independent passage workers. A requested
// maximum of zero means automatic. The optional hardware and available-memory
// arguments make the policy deterministic in tests; zero asks the
// implementation to inspect the current computer.
unsigned PortableDepartureWorkerCount(unsigned requested_maximum,
                                      unsigned candidate_count,
                                      unsigned hardware_concurrency = 0,
                                      uint64_t physical_memory_bytes = 0);

#endif  // OCPN_PORTABLE_DEPARTURE_TIME_H
