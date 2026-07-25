#include "weather_routing_departure_coordinator.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void Expect(bool condition, const char* message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(1);
}

ppm::RoutingOutcome Outcome(std::int64_t departure, std::uint64_t duration,
                            double longitude = 0.1) {
  ppm::RoutingOutcome outcome;
  outcome.departure_unix_time = departure;
  outcome.duration_seconds = duration;
  outcome.validation_samples = 8;
  outcome.points = {
      {0.0, 0.0, departure},
      {0.0, longitude, departure + static_cast<std::int64_t>(duration)}};
  outcome.route_environment.resize(2);
  outcome.route_environment[0].latitude = 0.0;
  outcome.route_environment[0].longitude = 0.0;
  outcome.route_environment[0].unix_time = departure;
  outcome.route_environment[1].latitude = 0.0;
  outcome.route_environment[1].longitude = longitude;
  outcome.route_environment[1].unix_time =
      departure + static_cast<std::int64_t>(duration);
  return outcome;
}

ppm::DeparturePreflight Available() {
  return [](const std::vector<std::int64_t>& times,
            std::vector<std::uint8_t>* availability, std::string*) {
    availability->assign(times.size(), 1U);
    return true;
  };
}

}  // namespace

int main() {
  using ppm::DepartureCandidateState;
  using ppm::DepartureSearchPlan;
  using ppm::WeatherRoutingDepartureCoordinator;

  WeatherRoutingDepartureCoordinator coordinator;
  std::atomic<bool> cancelled{false};
  DepartureSearchPlan plan;
  plan.nominal_departure_unix_time = 1'000'000;
  plan.offsets_seconds = {-3600, 0, 3600};
  plan.requested_maximum_workers = 3;
  plan.hardware_concurrency = 8;
  plan.physical_memory_bytes = 16ULL * 1024 * 1024 * 1024;

  std::mutex starts_mutex;
  std::vector<std::int64_t> starts;
  bool nominal_observed_running_first = false;
  const auto result = coordinator.Run(
      plan, Available(),
      [&](std::size_t, std::int64_t departure, std::int64_t offset,
          const ppm::DepartureCandidateProgress& progress,
          ppm::RoutingOutcome* outcome, std::string* error) {
        {
          std::lock_guard<std::mutex> lock(starts_mutex);
          starts.push_back(offset);
        }
        progress(20, "Forward isochrone");
        if (offset == 0) {
          *error = "nominal candidate fixture failure";
          return false;
        }
        *outcome = Outcome(departure, offset < 0 ? 7200 : 5400);
        return true;
      },
      [&](const ppm::DepartureSearchProgress& progress) {
        if (!progress.candidates.empty() &&
            progress.candidates[1].state == DepartureCandidateState::kRunning &&
            progress.running == 1)
          nominal_observed_running_first = true;
      },
      cancelled);
  Expect(nominal_observed_running_first,
         "nominal candidate was not observably scheduled first");
  Expect(result.candidates.size() == 3,
         "coordinator changed chronological candidate order");
  Expect(!result.candidates[1].success && result.candidates[1].summary.state ==
                                              DepartureCandidateState::kFailed,
         "candidate failure was not isolated");
  Expect(result.best_index && *result.best_index == 2,
         "later successful candidate was not selected");

  ppm::DepartureCandidateResult economical;
  economical.success = true;
  economical.summary.departure_unix_time = plan.nominal_departure_unix_time;
  economical.outcome = Outcome(plan.nominal_departure_unix_time, 3600);
  economical.outcome.motor_seconds = 600;
  economical.outcome.estimated_fuel_litres = 2.0;
  ppm::DepartureCandidateResult expensive = economical;
  expensive.outcome.motor_seconds = 900;
  Expect(ppm::BetterDepartureCandidate(economical, expensive,
                                       plan.nominal_departure_unix_time) &&
             !ppm::BetterDepartureCandidate(expensive, economical,
                                            plan.nominal_departure_unix_time),
         "deterministic resource tie-breaking was not applied");

  auto serial_plan = plan;
  serial_plan.requested_maximum_workers = 1;
  auto calculate_deterministic =
      [](std::size_t, std::int64_t departure, std::int64_t offset,
         const ppm::DepartureCandidateProgress&, ppm::RoutingOutcome* outcome,
         std::string*) {
        *outcome = Outcome(
            departure, static_cast<std::uint64_t>(7200 + std::llabs(offset)));
        outcome->passage_legs = {
            {0, 1, 0, 2, departure, departure + 3600, 5.0, 100},
            {1, 2, 1, 1, departure + 3600,
             departure + static_cast<std::int64_t>(outcome->duration_seconds),
             5.0, 100}};
        return true;
      };
  const auto serial = coordinator.Run(serial_plan, Available(),
                                      calculate_deterministic, {}, cancelled);
  const auto parallel = coordinator.Run(plan, Available(),
                                        calculate_deterministic, {}, cancelled);
  Expect(serial.best_index == parallel.best_index,
         "serial and parallel selection differed");
  for (std::size_t index = 0; index < serial.candidates.size(); ++index) {
    Expect(serial.candidates[index].summary.state ==
                   parallel.candidates[index].summary.state &&
               serial.candidates[index].outcome.duration_seconds ==
                   parallel.candidates[index].outcome.duration_seconds,
           "serial and parallel candidate outcomes differed");
  }
  Expect(
      parallel.candidates[1].outcome.passage_legs.size() == 2 &&
          parallel.candidates[1].outcome.passage_legs[1].departure_unix_time ==
              parallel.candidates[1].outcome.passage_legs[0].arrival_unix_time,
      "continuous multi-waypoint seam state was not retained");

  auto trap_plan = plan;
  trap_plan.offsets_seconds = {-1800, 0, 1800};
  const auto trapped = coordinator.Run(
      trap_plan, Available(),
      [](std::size_t, std::int64_t departure, std::int64_t offset,
         const ppm::DepartureCandidateProgress&, ppm::RoutingOutcome* outcome,
         std::string*) {
        if (offset == 0) throw std::runtime_error("simulated Wasm trap");
        *outcome = Outcome(departure, 3600 + std::llabs(offset));
        return true;
      },
      {}, cancelled);
  Expect(!trapped.candidates[1].success && trapped.candidates[0].success &&
             trapped.candidates[2].success,
         "one trapped candidate contaminated other candidates");

  std::atomic<bool> cancel_all{false};
  const auto cancelled_result = coordinator.Run(
      plan, Available(),
      [&](std::size_t, std::int64_t departure, std::int64_t,
          const ppm::DepartureCandidateProgress&, ppm::RoutingOutcome* outcome,
          std::string*) {
        cancel_all.store(true);
        *outcome = Outcome(departure, 3600);
        return true;
      },
      {}, cancel_all);
  Expect(!cancelled_result.best_index,
         "cancelled departure run selected a route");
  for (const auto& candidate : cancelled_result.candidates)
    Expect(candidate.summary.state == DepartureCandidateState::kCancelled,
           "cancellation left an ambiguous candidate state");

  std::atomic<unsigned> active{0};
  std::atomic<unsigned> maximum_active{0};
  auto low_memory_plan = plan;
  low_memory_plan.offsets_seconds = {-7200, -3600, 0, 3600, 7200};
  low_memory_plan.physical_memory_bytes = 1024ULL * 1024 * 1024;
  const auto low_memory = coordinator.Run(
      low_memory_plan, Available(),
      [&](std::size_t, std::int64_t departure, std::int64_t,
          const ppm::DepartureCandidateProgress&, ppm::RoutingOutcome* outcome,
          std::string*) {
        const unsigned now = active.fetch_add(1) + 1;
        unsigned observed = maximum_active.load();
        while (now > observed &&
               !maximum_active.compare_exchange_weak(observed, now)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        active.fetch_sub(1);
        *outcome = Outcome(departure, 3600);
        return true;
      },
      {}, cancelled);
  Expect(low_memory.best_index.has_value() && maximum_active.load() == 1,
         "low-memory coordinator exceeded one worker");
  return 0;
}
