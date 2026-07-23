#include "serial_executor.h"

#include <exception>
#include <utility>

namespace ppm {

SerialExecutor::SerialExecutor(std::size_t queue_capacity)
    : queue_capacity_(queue_capacity == 0 ? 1 : queue_capacity),
      worker_([this]() { Run(); }) {}

SerialExecutor::~SerialExecutor() { Shutdown(); }

std::uint64_t SerialExecutor::Generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return generation_;
}

SerialExecutor::PostResult SerialExecutor::Post(std::uint64_t generation,
                                                Task task) {
  if (!task) return PostResult::kStopped;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return PostResult::kStopped;
    if (generation != generation_) return PostResult::kStaleGeneration;
    if (queue_.size() >= queue_capacity_) return PostResult::kQueueFull;
    queue_.push_back({generation, std::move(task)});
  }
  work_ready_.notify_one();
  return PostResult::kAccepted;
}

std::uint64_t SerialExecutor::AdvanceGeneration() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++generation_;
  if (generation_ == 0) ++generation_;
  queue_.clear();
  if (!active_) idle_.notify_all();
  return generation_;
}

bool SerialExecutor::WaitIdle(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return idle_.wait_for(lock, timeout,
                        [this]() { return queue_.empty() && !active_; });
}

void SerialExecutor::Shutdown() {
  bool notify = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopped_) {
      stopped_ = true;
      ++generation_;
      queue_.clear();
      notify = true;
    }
  }
  if (notify) work_ready_.notify_all();
  if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
    worker_.join();
}

std::size_t SerialExecutor::Pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size() + (active_ ? 1U : 0U);
}

bool SerialExecutor::IsWorkerThread() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return worker_id_ == std::this_thread::get_id();
}

void SerialExecutor::Run() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    worker_id_ = std::this_thread::get_id();
  }
  while (true) {
    WorkItem item;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_ready_.wait(lock,
                       [this]() { return stopped_ || !queue_.empty(); });
      if (stopped_ && queue_.empty()) break;
      item = std::move(queue_.front());
      queue_.pop_front();
      if (item.generation != generation_) {
        if (queue_.empty()) idle_.notify_all();
        continue;
      }
      active_ = true;
    }
    try {
      item.task(item.generation);
    } catch (...) {
      // Tasks cross a native plugin boundary. Exceptions must not terminate
      // the executor; the owner records task-specific diagnostics.
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_ = false;
      if (queue_.empty()) idle_.notify_all();
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    worker_id_ = {};
    idle_.notify_all();
  }
}

}  // namespace ppm
