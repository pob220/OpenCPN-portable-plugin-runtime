#include "weather_routing_departure_coordinator.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>

#include "portable_departure_time.h"

namespace ppm {
namespace {

bool Terminal(DepartureCandidateState state) {
  return state == DepartureCandidateState::kCompleted ||
         state == DepartureCandidateState::kFailed ||
         state == DepartureCandidateState::kCancelled;
}

DepartureSearchProgress Snapshot(
    const std::vector<DepartureCandidateResult>& candidates) {
  DepartureSearchProgress snapshot;
  snapshot.candidates.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    snapshot.candidates.push_back(candidate.summary);
    if (Terminal(candidate.summary.state))
      ++snapshot.completed;
    else if (candidate.summary.state == DepartureCandidateState::kRunning)
      ++snapshot.running;
    else
      ++snapshot.queued;
  }
  return snapshot;
}

bool ValidSuccessfulOutcome(const RoutingOutcome& outcome) {
  return outcome.points.size() >= 2 &&
         outcome.route_environment.size() == outcome.points.size() &&
         outcome.duration_seconds > 0 && outcome.validation_samples > 0;
}

}  // namespace

bool BetterDepartureCandidate(const DepartureCandidateResult& candidate,
                              const DepartureCandidateResult& incumbent,
                              std::int64_t nominal_departure_unix_time) {
  const auto& lhs = candidate.outcome;
  const auto& rhs = incumbent.outcome;
  if (lhs.duration_seconds != rhs.duration_seconds)
    return lhs.duration_seconds < rhs.duration_seconds;
  const std::int64_t lhs_eta =
      lhs.departure_unix_time + static_cast<std::int64_t>(lhs.duration_seconds);
  const std::int64_t rhs_eta =
      rhs.departure_unix_time + static_cast<std::int64_t>(rhs.duration_seconds);
  if (lhs_eta != rhs_eta) return lhs_eta < rhs_eta;
  if (lhs.motor_seconds != rhs.motor_seconds)
    return lhs.motor_seconds < rhs.motor_seconds;
  if (lhs.estimated_fuel_litres != rhs.estimated_fuel_litres)
    return lhs.estimated_fuel_litres < rhs.estimated_fuel_litres;
  const std::uint64_t lhs_manoeuvres =
      static_cast<std::uint64_t>(lhs.tacks) + lhs.propulsion_transitions;
  const std::uint64_t rhs_manoeuvres =
      static_cast<std::uint64_t>(rhs.tacks) + rhs.propulsion_transitions;
  if (lhs_manoeuvres != rhs_manoeuvres) return lhs_manoeuvres < rhs_manoeuvres;
  const std::int64_t lhs_offset = std::llabs(
      candidate.summary.departure_unix_time - nominal_departure_unix_time);
  const std::int64_t rhs_offset = std::llabs(
      incumbent.summary.departure_unix_time - nominal_departure_unix_time);
  if (lhs_offset != rhs_offset) return lhs_offset < rhs_offset;
  return candidate.summary.departure_unix_time <
         incumbent.summary.departure_unix_time;
}

DepartureSearchResult WeatherRoutingDepartureCoordinator::Run(
    const DepartureSearchPlan& plan, const DeparturePreflight& preflight,
    const DepartureCalculate& calculate,
    const DepartureProgressCallback& progress,
    const std::atomic<bool>& cancelled) const {
  DepartureSearchResult result;
  if (plan.offsets_seconds.empty() || !preflight || !calculate) return result;

  result.candidates.resize(plan.offsets_seconds.size());
  for (std::size_t index = 0; index < result.candidates.size(); ++index) {
    auto& candidate = result.candidates[index];
    candidate.summary.offset_seconds = plan.offsets_seconds[index];
    candidate.summary.departure_unix_time =
        plan.nominal_departure_unix_time + plan.offsets_seconds[index];
    candidate.summary.state = DepartureCandidateState::kPreflight;
    candidate.summary.stage = "Preflight";
  }

  std::mutex mutex;
  std::mutex progress_mutex;
  auto publish = [&] {
    if (!progress) return;
    std::lock_guard<std::mutex> progress_lock(progress_mutex);
    DepartureSearchProgress snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex);
      snapshot = Snapshot(result.candidates);
    }
    progress(snapshot);
  };
  publish();

  std::vector<std::int64_t> departure_times;
  departure_times.reserve(result.candidates.size());
  for (const auto& candidate : result.candidates)
    departure_times.push_back(candidate.summary.departure_unix_time);
  std::vector<std::uint8_t> availability;
  std::string preflight_error;
  if (!preflight(departure_times, &availability, &preflight_error) ||
      availability.size() != result.candidates.size()) {
    if (preflight_error.empty())
      preflight_error =
          "environmental provider returned the wrong preflight batch size";
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (auto& candidate : result.candidates) {
        candidate.summary.state = cancelled.load()
                                      ? DepartureCandidateState::kCancelled
                                      : DepartureCandidateState::kFailed;
        candidate.summary.stage =
            cancelled.load() ? "Cancelled" : "Preflight failed";
        candidate.summary.diagnostic = preflight_error;
      }
    }
    publish();
    return result;
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    for (std::size_t index = 0; index < result.candidates.size(); ++index) {
      auto& candidate = result.candidates[index];
      if ((availability[index] & 1U) != 0) {
        candidate.summary.state = DepartureCandidateState::kQueued;
        candidate.summary.stage = "Queued";
      } else {
        candidate.summary.state = DepartureCandidateState::kFailed;
        candidate.summary.stage = "Preflight failed";
        candidate.summary.diagnostic =
            "wind data is unavailable at the departure position and time";
      }
    }
  }
  publish();

  const std::vector<std::size_t> execution_order =
      PortableDepartureExecutionOrder(plan.offsets_seconds);
  std::vector<std::size_t> runnable;
  runnable.reserve(execution_order.size());
  for (const std::size_t index : execution_order)
    if (result.candidates[index].summary.state ==
        DepartureCandidateState::kQueued)
      runnable.push_back(index);
  if (runnable.empty()) return result;

  const unsigned worker_count = PortableDepartureWorkerCount(
      plan.requested_maximum_workers, static_cast<unsigned>(runnable.size()),
      plan.hardware_concurrency, plan.physical_memory_bytes);
  std::atomic<std::size_t> next_execution{worker_count};

  // Publish the nominal/nearest candidate as running before other workers are
  // released. Calculation remains parallel, but observable scheduling is
  // deterministic and nominal-first.
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto& first = result.candidates[runnable.front()].summary;
    first.state = DepartureCandidateState::kRunning;
    first.stage = "Starting";
  }
  publish();

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (unsigned worker_index = 0; worker_index < worker_count; ++worker_index) {
    workers.emplace_back([&, worker_index] {
      std::size_t execution_index = worker_index;
      while (!cancelled.load() && execution_index < runnable.size()) {
        const std::size_t candidate_index = runnable[execution_index];
        {
          std::lock_guard<std::mutex> lock(mutex);
          auto& summary = result.candidates[candidate_index].summary;
          if (summary.state != DepartureCandidateState::kRunning) {
            summary.state = DepartureCandidateState::kRunning;
            summary.stage = "Starting";
          }
        }
        publish();

        const DepartureCandidateProgress report =
            [&, candidate_index](unsigned percent, const std::string& stage) {
              bool changed = false;
              {
                std::lock_guard<std::mutex> lock(mutex);
                auto& summary = result.candidates[candidate_index].summary;
                const unsigned bounded = std::min(99U, percent);
                if (summary.stage != stage || bounded >= summary.percent + 2U) {
                  summary.percent = std::max(summary.percent, bounded);
                  summary.stage = stage.empty() ? "Calculating" : stage;
                  changed = true;
                }
              }
              if (changed) publish();
            };

        RoutingOutcome outcome;
        std::string diagnostic;
        bool okay = false;
        try {
          const auto& summary = result.candidates[candidate_index].summary;
          okay =
              calculate(candidate_index, summary.departure_unix_time,
                        summary.offset_seconds, report, &outcome, &diagnostic);
        } catch (const std::exception& error) {
          diagnostic = std::string("candidate failed safely: ") + error.what();
        } catch (...) {
          diagnostic = "candidate failed safely with an unknown exception";
        }

        {
          std::lock_guard<std::mutex> lock(mutex);
          auto& candidate = result.candidates[candidate_index];
          std::string lower_diagnostic = diagnostic;
          std::transform(lower_diagnostic.begin(), lower_diagnostic.end(),
                         lower_diagnostic.begin(), [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                         });
          if (cancelled.load() ||
              lower_diagnostic.find("cancel") != std::string::npos) {
            candidate.summary.state = DepartureCandidateState::kCancelled;
            candidate.summary.stage = "Cancelled";
            candidate.summary.diagnostic =
                diagnostic.empty() ? "Cancelled" : diagnostic;
          } else if (!okay || !ValidSuccessfulOutcome(outcome)) {
            candidate.summary.state = DepartureCandidateState::kFailed;
            candidate.summary.stage = "Failed";
            candidate.summary.diagnostic =
                diagnostic.empty()
                    ? "candidate returned an invalid or unvalidated passage"
                    : diagnostic;
          } else {
            candidate.outcome = std::move(outcome);
            candidate.success = true;
            candidate.summary.state = DepartureCandidateState::kCompleted;
            candidate.summary.percent = 100;
            candidate.summary.stage = "Complete";
            candidate.summary.diagnostic = diagnostic;
          }
        }
        publish();
        execution_index = next_execution.fetch_add(1);
      }
    });
  }
  for (auto& worker : workers) worker.join();

  if (cancelled.load()) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (auto& candidate : result.candidates) {
        if (Terminal(candidate.summary.state)) continue;
        candidate.summary.state = DepartureCandidateState::kCancelled;
        candidate.summary.stage = "Cancelled";
        candidate.summary.diagnostic = "Cancelled";
      }
    }
    publish();
  }

  for (std::size_t index = 0; index < result.candidates.size(); ++index) {
    const auto& candidate = result.candidates[index];
    if (!candidate.success) continue;
    if (!result.best_index ||
        BetterDepartureCandidate(candidate,
                                 result.candidates[*result.best_index],
                                 plan.nominal_departure_unix_time))
      result.best_index = index;
  }
  return result;
}

}  // namespace ppm
