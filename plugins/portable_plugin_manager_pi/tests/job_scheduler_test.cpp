#include "job_scheduler.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
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
  ppm::JobScheduler scheduler(2, 4, 2);
  std::mutex mutex;
  std::vector<ppm::JobEvent> events;
  auto receive = [&](const ppm::JobEvent& event) {
    std::lock_guard<std::mutex> lock(mutex);
    events.push_back(event);
  };
  std::string diagnostic;
  CHECK(scheduler.Start("org.test.one", "first", 7, 12, receive,
                        &diagnostic));
  CHECK(!scheduler.Start("org.test.one", "first", 7, 12, receive,
                         &diagnostic));
  CHECK(scheduler.Start("org.test.one", "cancel", 7, 500, receive,
                        &diagnostic));
  CHECK(!scheduler.Start("org.test.one", "quota", 7, 5, receive,
                         &diagnostic));
  CHECK(scheduler.Cancel("org.test.one", "cancel", 7));
  CHECK(!scheduler.Cancel("org.test.one", "cancel", 6));
  CHECK(scheduler.WaitOwnerIdle("org.test.one", 3s));

  bool completed = false;
  bool cancelled = false;
  std::uint8_t last_progress = 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& event : events) {
      CHECK(event.generation == 7);
      if (event.kind == ppm::JobEventKind::kProgress) {
        CHECK(event.progress >= last_progress || event.id != "first");
        if (event.id == "first") last_progress = event.progress;
      }
      if (event.id == "first" &&
          event.kind == ppm::JobEventKind::kCompleted)
        completed = true;
      if (event.id == "cancel" &&
          event.kind == ppm::JobEventKind::kCancelled)
        cancelled = true;
    }
  }
  CHECK(completed);
  CHECK(cancelled);
  CHECK(scheduler.Snapshots().empty());
  scheduler.Shutdown();
  CHECK(!scheduler.Start("org.test.one", "late", 8, 1, receive,
                         &diagnostic));
  return 0;
}
