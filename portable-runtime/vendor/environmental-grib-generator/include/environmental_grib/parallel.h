#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace environmental_grib {

inline constexpr std::size_t kDefaultDownloadConcurrency = 4;

class DownloadConcurrencyBudget {
public:
  class Permit {
  public:
    Permit() = default;
    explicit Permit(DownloadConcurrencyBudget* budget) : budget_(budget) {}
    Permit(const Permit&) = delete;
    Permit& operator=(const Permit&) = delete;
    Permit(Permit&& other) noexcept
        : budget_(std::exchange(other.budget_, nullptr)) {}
    Permit& operator=(Permit&& other) noexcept {
      if (this != &other) {
        Release();
        budget_ = std::exchange(other.budget_, nullptr);
      }
      return *this;
    }
    ~Permit() { Release(); }

  private:
    void Release() {
      if (!budget_) return;
      budget_->Release();
      budget_ = nullptr;
    }
    DownloadConcurrencyBudget* budget_{};
  };

  explicit DownloadConcurrencyBudget(std::size_t maximum)
      : maximum_(std::max<std::size_t>(1, maximum)) {}

  Permit Acquire() {
    std::unique_lock lock(mutex_);
    available_.wait(lock, [&] { return active_ < maximum_; });
    ++active_;
    return Permit(this);
  }

  std::size_t maximum() const { return maximum_; }

private:
  void Release() {
    {
      std::lock_guard lock(mutex_);
      --active_;
    }
    available_.notify_one();
  }

  const std::size_t maximum_;
  std::size_t active_{};
  std::mutex mutex_;
  std::condition_variable available_;
};

inline std::mutex& NetcdfApiMutex() {
  static std::mutex mutex;
  return mutex;
}

// Run independent work concurrently while preserving the input order in the
// returned vector.  All workers are joined before an exception is rethrown, so
// callers can safely clean temporary files after a failed batch.
template <typename Input, typename Function>
auto ParallelMapOrdered(const std::vector<Input>& inputs,
                        std::size_t maximum_concurrency, Function function)
    -> std::vector<std::invoke_result_t<Function, const Input&>> {
  using Result = std::invoke_result_t<Function, const Input&>;
  static_assert(!std::is_void_v<Result>);
  if (inputs.empty()) return {};
  const std::size_t worker_count =
      std::max<std::size_t>(1, std::min(maximum_concurrency, inputs.size()));
  std::vector<std::optional<Result>> slots(inputs.size());
  std::atomic<std::size_t> next{0};
  std::atomic<bool> failed{false};
  std::exception_ptr failure;
  std::mutex failure_mutex;
  std::vector<std::jthread> workers;
  workers.reserve(worker_count);
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&] {
      while (!failed.load(std::memory_order_acquire)) {
        const std::size_t index = next.fetch_add(1);
        if (index >= inputs.size()) return;
        try {
          slots[index].emplace(function(inputs[index]));
        } catch (...) {
          {
            std::lock_guard lock(failure_mutex);
            if (!failure) failure = std::current_exception();
          }
          failed.store(true, std::memory_order_release);
          return;
        }
      }
    });
  }
  workers.clear();
  if (failure) std::rethrow_exception(failure);
  std::vector<Result> results;
  results.reserve(slots.size());
  for (auto& slot : slots) results.push_back(std::move(*slot));
  return results;
}

}  // namespace environmental_grib
