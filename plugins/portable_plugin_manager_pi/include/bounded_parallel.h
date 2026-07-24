#ifndef PORTABLE_PLUGIN_MANAGER_BOUNDED_PARALLEL_H
#define PORTABLE_PLUGIN_MANAGER_BOUNDED_PARALLEL_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace ppm {

namespace bounded_parallel_detail {

inline unsigned TotalWorkerLimit() {
  const unsigned hardware = std::max(1U, std::thread::hardware_concurrency());
  unsigned memory_limit = 4;
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long page_size = sysconf(_SC_PAGESIZE);
  if (pages > 0 && page_size > 0) {
    const long double bytes =
        static_cast<long double>(pages) * static_cast<long double>(page_size);
    if (bytes <= 2.0L * 1024.0L * 1024.0L * 1024.0L)
      memory_limit = 2;
    else if (bytes <= 4.0L * 1024.0L * 1024.0L * 1024.0L)
      memory_limit = 3;
  }
#endif
  return std::max(1U, std::min(hardware, memory_limit));
}

inline std::atomic<unsigned>& ActiveHelpers() {
  static std::atomic<unsigned> helpers{0};
  return helpers;
}

inline std::atomic<unsigned>& ActiveCallers() {
  static std::atomic<unsigned> callers{0};
  return callers;
}

class CallerLease {
public:
  CallerLease() { ActiveCallers().fetch_add(1, std::memory_order_acq_rel); }
  CallerLease(const CallerLease&) = delete;
  CallerLease& operator=(const CallerLease&) = delete;
  ~CallerLease() { ActiveCallers().fetch_sub(1, std::memory_order_release); }
};

class HelperLease {
public:
  HelperLease() = default;
  HelperLease(const HelperLease&) = delete;
  HelperLease& operator=(const HelperLease&) = delete;
  HelperLease(HelperLease&& other) noexcept : acquired_(other.acquired_) {
    other.acquired_ = false;
  }
  ~HelperLease() {
    if (acquired_) ActiveHelpers().fetch_sub(1, std::memory_order_release);
  }

  static HelperLease TryAcquire() {
    HelperLease lease;
    const unsigned limit = TotalWorkerLimit();
    unsigned active = ActiveHelpers().load(std::memory_order_relaxed);
    while (ActiveCallers().load(std::memory_order_relaxed) + active < limit) {
      if (ActiveHelpers().compare_exchange_weak(active, active + 1,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
        lease.acquired_ = true;
        break;
      }
    }
    return lease;
  }

  bool acquired() const { return acquired_; }

private:
  bool acquired_ = false;
};

}  // namespace bounded_parallel_detail

/**
 * Execute independent indexed work with deterministic output slots.
 *
 * Helper threads are globally bounded across simultaneous departure searches,
 * and each concurrent caller reduces helper availability. A 1–2 GiB computer
 * uses at most one helper for a lone caller; larger systems use at most four
 * caller/helper slots in the normal case. Callers always participate, so
 * saturation degrades to serial work instead of creating an unbounded queue
 * or thread count.
 */
template <typename Function>
void BoundedParallelFor(std::size_t count, std::size_t minimum_items_per_worker,
                        Function&& function) {
  if (count == 0) return;
  bounded_parallel_detail::CallerLease caller;
  const std::size_t useful_workers = std::max<std::size_t>(
      1, (count + minimum_items_per_worker - 1) / minimum_items_per_worker);
  const unsigned desired = static_cast<unsigned>(std::min<std::size_t>(
      bounded_parallel_detail::TotalWorkerLimit(), useful_workers));
  std::vector<bounded_parallel_detail::HelperLease> leases;
  leases.reserve(desired > 0 ? desired - 1 : 0);
  while (leases.size() + 1 < desired) {
    auto lease = bounded_parallel_detail::HelperLease::TryAcquire();
    if (!lease.acquired()) break;
    leases.push_back(std::move(lease));
  }
  if (leases.empty()) {
    for (std::size_t index = 0; index < count; ++index) function(index);
    return;
  }

  std::atomic<std::size_t> next{0};
  std::atomic_bool failed{false};
  std::exception_ptr failure;
  std::mutex failure_mutex;
  auto work = [&]() {
    try {
      while (!failed.load(std::memory_order_relaxed)) {
        const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= count) break;
        function(index);
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(failure_mutex);
      if (!failure) failure = std::current_exception();
      failed.store(true, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> helpers;
  helpers.reserve(leases.size());
  for (std::size_t index = 0; index < leases.size(); ++index)
    helpers.emplace_back(work);
  work();
  for (auto& helper : helpers) helper.join();
  if (failure) std::rethrow_exception(failure);
}

}  // namespace ppm

#endif
