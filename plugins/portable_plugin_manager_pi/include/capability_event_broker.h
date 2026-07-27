#ifndef PORTABLE_PLUGIN_MANAGER_CAPABILITY_EVENT_BROKER_H
#define PORTABLE_PLUGIN_MANAGER_CAPABILITY_EVENT_BROKER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ppm {

enum class CapabilityEventKind {
  kNmea0183,
  kNmea2000,
  kSignalK,
  kNavigationPosition,
  kAisTarget,
  kActiveLeg,
  kCursor,
  kViewport,
  kPluginMessage,
  kHostEnvironment,
};

const char* CapabilityEventName(CapabilityEventKind kind);
bool ParseCapabilityEventKind(const std::string& value,
                              CapabilityEventKind* kind);
const char* CapabilityEventPermission(CapabilityEventKind kind);

struct CapabilityEvent {
  CapabilityEventKind kind = CapabilityEventKind::kNmea0183;
  std::string topic;
  std::string payload;
  std::uint64_t sequence = 0;
};

struct CapabilityEventSubscription {
  std::string package_id;
  CapabilityEventKind kind = CapabilityEventKind::kNmea0183;
  std::string topic_prefix;
  std::size_t queue_limit = 32;
  std::uint64_t id = 0;
};

struct CapabilityEventStats {
  std::uint64_t accepted = 0;
  std::uint64_t coalesced = 0;
  std::uint64_t dropped = 0;
  std::size_t pending = 0;
};

/**
 * Permission-neutral, bounded event transport used by the runtime engine.
 *
 * The caller validates package permissions before subscribing. The broker
 * then applies topic filtering, keeps at most one high-frequency navigation
 * update per topic, and enforces an independent queue bound for every
 * package. It never invokes guest code while holding its mutex.
 */
class CapabilityEventBroker {
public:
  static constexpr std::size_t kMaximumPayloadBytes = 64 * 1024;
  static constexpr std::size_t kMaximumTopicBytes = 256;
  static constexpr std::size_t kMaximumQueueLimit = 256;

  bool Subscribe(const CapabilityEventSubscription& subscription,
                 std::string* diagnostic,
                 std::uint64_t* subscription_id = nullptr);
  bool Unsubscribe(const std::string& package_id,
                   std::uint64_t subscription_id);
  void RemovePackage(const std::string& package_id);
  void ClearPending(const std::string& package_id);

  bool Publish(CapabilityEvent event, std::string* diagnostic = nullptr);
  std::vector<CapabilityEvent> Drain(const std::string& package_id,
                                     std::size_t maximum);
  std::size_t Pending(const std::string& package_id) const;
  CapabilityEventStats Stats(const std::string& package_id) const;

private:
  struct PackageQueue {
    std::vector<CapabilityEventSubscription> subscriptions;
    std::deque<CapabilityEvent> pending;
    CapabilityEventStats stats;
  };

  static bool Matches(const CapabilityEventSubscription& subscription,
                      const CapabilityEvent& event);
  static bool Coalesces(CapabilityEventKind kind);

  mutable std::mutex mutex_;
  std::map<std::string, PackageQueue> packages_;
  std::uint64_t next_sequence_ = 1;
  std::uint64_t next_subscription_id_ = 1;
};

}  // namespace ppm

#endif
