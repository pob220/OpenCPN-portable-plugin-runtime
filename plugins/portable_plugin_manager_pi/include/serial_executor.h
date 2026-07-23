#ifndef PORTABLE_PLUGIN_MANAGER_SERIAL_EXECUTOR_H
#define PORTABLE_PLUGIN_MANAGER_SERIAL_EXECUTOR_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace ppm {

class SerialExecutor {
 public:
  using Task = std::function<void(std::uint64_t)>;

  enum class PostResult {
    kAccepted,
    kStaleGeneration,
    kQueueFull,
    kStopped
  };

  explicit SerialExecutor(std::size_t queue_capacity = 128);
  ~SerialExecutor();

  SerialExecutor(const SerialExecutor&) = delete;
  SerialExecutor& operator=(const SerialExecutor&) = delete;

  std::uint64_t Generation() const;
  PostResult Post(std::uint64_t generation, Task task);

  // Invalidates queued work from the old generation and returns the new token.
  // The currently executing task is allowed to reach its mandatory runtime
  // deadline; callers use WaitIdle before destroying task-owned state.
  std::uint64_t AdvanceGeneration();
  bool WaitIdle(std::chrono::milliseconds timeout);
  void Shutdown();

  std::size_t Pending() const;
  bool IsWorkerThread() const;

 private:
  struct WorkItem {
    std::uint64_t generation = 0;
    Task task;
  };

  void Run();

  const std::size_t queue_capacity_;
  mutable std::mutex mutex_;
  std::condition_variable work_ready_;
  std::condition_variable idle_;
  std::deque<WorkItem> queue_;
  std::thread worker_;
  std::thread::id worker_id_;
  std::uint64_t generation_ = 1;
  bool active_ = false;
  bool stopped_ = false;
};

}  // namespace ppm

#endif
