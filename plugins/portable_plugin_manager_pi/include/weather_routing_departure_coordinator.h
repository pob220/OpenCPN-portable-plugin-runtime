#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "routing_service.h"

namespace ppm {

enum class DepartureCandidateState {
  kQueued,
  kPreflight,
  kRunning,
  kCompleted,
  kFailed,
  kCancelled,
};

struct DepartureCandidateSummary {
  std::int64_t offset_seconds = 0;
  std::int64_t departure_unix_time = 0;
  DepartureCandidateState state = DepartureCandidateState::kQueued;
  unsigned percent = 0;
  std::string stage = "Queued";
  std::string diagnostic;
};

struct DepartureCandidateResult {
  DepartureCandidateSummary summary;
  RoutingOutcome outcome;
  bool success = false;
};

struct DepartureSearchProgress {
  std::vector<DepartureCandidateSummary> candidates;
  unsigned completed = 0;
  unsigned running = 0;
  unsigned queued = 0;
};

struct DepartureSearchPlan {
  std::int64_t nominal_departure_unix_time = 0;
  std::vector<std::int64_t> offsets_seconds;
  unsigned requested_maximum_workers = 0;
  unsigned hardware_concurrency = 0;
  std::uint64_t physical_memory_bytes = 0;
};

struct DepartureSearchResult {
  std::vector<DepartureCandidateResult> candidates;
  std::optional<std::size_t> best_index;
};

using DeparturePreflight =
    std::function<bool(const std::vector<std::int64_t>&,
                       std::vector<std::uint8_t>*, std::string*)>;
using DepartureCandidateProgress =
    std::function<void(unsigned, const std::string&)>;
using DepartureCalculate = std::function<bool(
    std::size_t, std::int64_t, std::int64_t, const DepartureCandidateProgress&,
    RoutingOutcome*, std::string*)>;
using DepartureProgressCallback =
    std::function<void(const DepartureSearchProgress&)>;

bool BetterDepartureCandidate(const DepartureCandidateResult& candidate,
                              const DepartureCandidateResult& incumbent,
                              std::int64_t nominal_departure_unix_time);

class WeatherRoutingDepartureCoordinator {
public:
  DepartureSearchResult Run(const DepartureSearchPlan& plan,
                            const DeparturePreflight& preflight,
                            const DepartureCalculate& calculate,
                            const DepartureProgressCallback& progress,
                            const std::atomic<bool>& cancelled) const;
};

}  // namespace ppm
