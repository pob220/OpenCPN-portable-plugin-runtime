#include "serial_executor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#define CHECK(expression)                                                   \
  do {                                                                      \
    if (!(expression)) {                                                    \
      std::cerr << "check failed at " << __FILE__ << ':' << __LINE__       \
                << ": " #expression "\n";                                  \
      return 1;                                                             \
    }                                                                       \
  } while (false)

using namespace std::chrono_literals;

int main() {
  ppm::SerialExecutor executor(3);
  const std::uint64_t first = executor.Generation();
  std::mutex mutex;
  std::condition_variable started;
  std::condition_variable release;
  bool first_started = false;
  bool allow_finish = false;
  std::vector<int> observed;
  std::atomic_bool task_checks{true};

  CHECK(executor.Post(first, [&](std::uint64_t generation) {
          if (generation != first) task_checks = false;
          {
            std::lock_guard<std::mutex> lock(mutex);
            first_started = true;
            observed.push_back(1);
          }
          started.notify_one();
          std::unique_lock<std::mutex> lock(mutex);
          release.wait(lock, [&]() { return allow_finish; });
        }) == ppm::SerialExecutor::PostResult::kAccepted);
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(started.wait_for(lock, 2s, [&]() { return first_started; }));
  }
  CHECK(executor.Post(first, [&](std::uint64_t) {
          std::lock_guard<std::mutex> lock(mutex);
          observed.push_back(2);
        }) == ppm::SerialExecutor::PostResult::kAccepted);

  const std::uint64_t second = executor.AdvanceGeneration();
  CHECK(second != first);
  CHECK(executor.Post(first, [](std::uint64_t) {}) ==
        ppm::SerialExecutor::PostResult::kStaleGeneration);
  CHECK(executor.Post(second, [&](std::uint64_t generation) {
          if (generation != second) task_checks = false;
          std::lock_guard<std::mutex> lock(mutex);
          observed.push_back(3);
        }) == ppm::SerialExecutor::PostResult::kAccepted);
  {
    std::lock_guard<std::mutex> lock(mutex);
    allow_finish = true;
  }
  release.notify_one();
  CHECK(executor.WaitIdle(2s));
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK((observed == std::vector<int>{1, 3}));
  }

  bool worker_thread = false;
  CHECK(executor.Post(second, [&](std::uint64_t) {
          worker_thread = executor.IsWorkerThread();
          throw 7;
        }) == ppm::SerialExecutor::PostResult::kAccepted);
  CHECK(executor.WaitIdle(2s));
  CHECK(worker_thread);
  CHECK(task_checks);

  executor.Shutdown();
  CHECK(executor.Post(second, [](std::uint64_t) {}) ==
        ppm::SerialExecutor::PostResult::kStopped);
  return 0;
}
