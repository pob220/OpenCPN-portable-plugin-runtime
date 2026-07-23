#ifndef PORTABLE_PLUGIN_MANAGER_JOB_SCHEDULER_H
#define PORTABLE_PLUGIN_MANAGER_JOB_SCHEDULER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ppm {

enum class JobEventKind : std::uint32_t {
  kProgress = 0,
  kCompleted = 1,
  kCancelled = 2,
  kFailed = 3
};

struct JobEvent {
  std::string owner;
  std::string id;
  std::uint64_t generation = 0;
  JobEventKind kind = JobEventKind::kProgress;
  std::uint8_t progress = 0;
  std::string message;
};

struct JobSnapshot {
  std::string owner;
  std::string id;
  std::uint64_t generation = 0;
  std::uint8_t progress = 0;
  bool running = false;
  bool cancelling = false;
};

class JobScheduler {
 public:
  using Callback = std::function<void(const JobEvent&)>;

  explicit JobScheduler(std::size_t worker_count = 2,
                        std::size_t maximum_jobs = 32,
                        std::size_t maximum_jobs_per_owner = 2);
  ~JobScheduler();

  JobScheduler(const JobScheduler&) = delete;
  JobScheduler& operator=(const JobScheduler&) = delete;

  bool Start(const std::string& owner, const std::string& id,
             std::uint64_t generation, std::uint32_t work_units,
             Callback callback, std::string* diagnostic);
  bool Cancel(const std::string& owner, const std::string& id,
              std::uint64_t generation);
  void CancelOwner(const std::string& owner);
  bool WaitOwnerIdle(const std::string& owner,
                     std::chrono::milliseconds timeout);
  std::vector<JobSnapshot> Snapshots() const;
  void Shutdown();

 private:
  struct Job;
  using Key = std::pair<std::string, std::string>;

  void Worker();

  const std::size_t maximum_jobs_;
  const std::size_t maximum_jobs_per_owner_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::condition_variable idle_;
  std::deque<std::shared_ptr<Job>> queue_;
  std::map<Key, std::shared_ptr<Job>> jobs_;
  std::vector<std::thread> workers_;
  bool stopped_ = false;
};

}  // namespace ppm

#endif
