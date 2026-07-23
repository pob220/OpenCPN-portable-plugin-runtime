#include "job_scheduler.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>

namespace ppm {
namespace {

constexpr std::uint32_t kMaximumWorkUnits = 10'000;

bool SafeName(const std::string& value) {
  return !value.empty() && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isalnum(character) || character == '-' ||
                  character == '_' || character == '.';
         });
}

}  // namespace

struct JobScheduler::Job {
  std::string owner;
  std::string id;
  std::uint64_t generation = 0;
  std::uint32_t work_units = 0;
  Callback callback;
  std::atomic_bool cancel{false};
  std::atomic_bool running{false};
  std::atomic_uint8_t progress{0};
};

JobScheduler::JobScheduler(std::size_t worker_count,
                           std::size_t maximum_jobs,
                           std::size_t maximum_jobs_per_owner)
    : maximum_jobs_(std::max<std::size_t>(1, maximum_jobs)),
      maximum_jobs_per_owner_(
          std::max<std::size_t>(1, maximum_jobs_per_owner)) {
  const std::size_t count =
      std::clamp<std::size_t>(worker_count, 1, 8);
  workers_.reserve(count);
  for (std::size_t index = 0; index < count; ++index)
    workers_.emplace_back([this]() { Worker(); });
}

JobScheduler::~JobScheduler() { Shutdown(); }

bool JobScheduler::Start(const std::string& owner, const std::string& id,
                         std::uint64_t generation,
                         std::uint32_t work_units, Callback callback,
                         std::string* diagnostic) {
  if (!SafeName(owner) || !SafeName(id) || generation == 0 ||
      work_units == 0 || work_units > kMaximumWorkUnits || !callback) {
    if (diagnostic) *diagnostic = "invalid bounded job request";
    return false;
  }
  auto job = std::make_shared<Job>();
  job->owner = owner;
  job->id = id;
  job->generation = generation;
  job->work_units = work_units;
  job->callback = std::move(callback);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      if (diagnostic) *diagnostic = "job scheduler is shutting down";
      return false;
    }
    const Key key{owner, id};
    if (jobs_.count(key) != 0) {
      if (diagnostic) *diagnostic = "job identifier is already active";
      return false;
    }
    const auto owner_jobs =
        std::count_if(jobs_.begin(), jobs_.end(), [&](const auto& item) {
          return item.first.first == owner;
        });
    if (jobs_.size() >= maximum_jobs_ ||
        static_cast<std::size_t>(owner_jobs) >= maximum_jobs_per_owner_) {
      if (diagnostic) *diagnostic = "job concurrency quota is full";
      return false;
    }
    jobs_.emplace(key, job);
    queue_.push_back(job);
  }
  ready_.notify_one();
  return true;
}

bool JobScheduler::Cancel(const std::string& owner, const std::string& id,
                          std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto item = jobs_.find({owner, id});
  if (item == jobs_.end() || item->second->generation != generation)
    return false;
  item->second->cancel = true;
  return true;
}

void JobScheduler::CancelOwner(const std::string& owner) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& item : jobs_) {
    if (item.first.first == owner) item.second->cancel = true;
  }
}

bool JobScheduler::WaitOwnerIdle(const std::string& owner,
                                 std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return idle_.wait_for(lock, timeout, [&]() {
    return std::none_of(jobs_.begin(), jobs_.end(), [&](const auto& item) {
      return item.first.first == owner;
    });
  });
}

std::vector<JobSnapshot> JobScheduler::Snapshots() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<JobSnapshot> result;
  result.reserve(jobs_.size());
  for (const auto& item : jobs_) {
    const auto& job = *item.second;
    result.push_back({job.owner, job.id, job.generation,
                      job.progress.load(), job.running.load(),
                      job.cancel.load()});
  }
  return result;
}

void JobScheduler::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopped_) {
      stopped_ = true;
      for (const auto& item : jobs_) item.second->cancel = true;
    }
  }
  ready_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable() && worker.get_id() != std::this_thread::get_id())
      worker.join();
  }
  workers_.clear();
}

void JobScheduler::Worker() {
  while (true) {
    std::shared_ptr<Job> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [&]() { return stopped_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopped_) return;
        continue;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
      job->running = true;
    }

    JobEvent terminal{job->owner, job->id, job->generation,
                      JobEventKind::kCompleted, 100, {}};
    std::uint8_t reported = 0;
    try {
      for (std::uint32_t unit = 1; unit <= job->work_units; ++unit) {
        if (job->cancel.load()) {
          terminal.kind = JobEventKind::kCancelled;
          terminal.progress = job->progress.load();
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const auto progress = static_cast<std::uint8_t>(
            (static_cast<std::uint64_t>(unit) * 100) / job->work_units);
        job->progress = progress;
        if (progress != reported && progress < 100) {
          reported = progress;
          job->callback({job->owner, job->id, job->generation,
                         JobEventKind::kProgress, progress, {}});
        }
      }
    } catch (const std::exception& error) {
      terminal.kind = JobEventKind::kFailed;
      terminal.progress = job->progress.load();
      terminal.message = error.what();
    } catch (...) {
      terminal.kind = JobEventKind::kFailed;
      terminal.progress = job->progress.load();
      terminal.message = "unhandled native job exception";
    }
    try {
      job->callback(terminal);
    } catch (...) {
      // A native consumer bug must not strand the scheduler record or
      // terminate a worker. Runtime consumers are expected to be noexcept.
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      jobs_.erase({job->owner, job->id});
      idle_.notify_all();
    }
  }
}

}  // namespace ppm
