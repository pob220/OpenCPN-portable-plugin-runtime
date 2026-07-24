#include "capability_event_broker.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace ppm {
namespace {

struct EventDescriptor {
  CapabilityEventKind kind;
  const char* name;
  const char* permission;
};

constexpr std::array<EventDescriptor, 9> kEventDescriptors{{
    {CapabilityEventKind::kNmea0183, "navigation.nmea0183",
     "navigation.nmea.read"},
    {CapabilityEventKind::kNmea2000, "navigation.nmea2000",
     "navigation.nmea2000.read"},
    {CapabilityEventKind::kSignalK, "navigation.signalk",
     "navigation.signalk.read"},
    {CapabilityEventKind::kNavigationPosition, "navigation.position",
     "navigation.position.read"},
    {CapabilityEventKind::kAisTarget, "navigation.ais", "navigation.ais.read"},
    {CapabilityEventKind::kActiveLeg, "navigation.active-leg",
     "navigation.active-leg.read"},
    {CapabilityEventKind::kCursor, "chart.cursor", "chart.cursor.read"},
    {CapabilityEventKind::kViewport, "chart.viewport", "chart.viewport.read"},
    {CapabilityEventKind::kPluginMessage, "opencpn.plugin-message",
     "plugin.messages.receive"},
}};

bool SafePackageId(const std::string& value) {
  if (value.empty() || value.size() > 128 || value.find('.') == value.npos)
    return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::islower(character) || std::isdigit(character) ||
           character == '.' || character == '-';
  });
}

}  // namespace

const char* CapabilityEventName(CapabilityEventKind kind) {
  for (const auto& descriptor : kEventDescriptors) {
    if (descriptor.kind == kind) return descriptor.name;
  }
  return "";
}

bool ParseCapabilityEventKind(const std::string& value,
                              CapabilityEventKind* kind) {
  if (!kind) return false;
  for (const auto& descriptor : kEventDescriptors) {
    if (value == descriptor.name) {
      *kind = descriptor.kind;
      return true;
    }
  }
  return false;
}

const char* CapabilityEventPermission(CapabilityEventKind kind) {
  for (const auto& descriptor : kEventDescriptors) {
    if (descriptor.kind == kind) return descriptor.permission;
  }
  return "";
}

bool CapabilityEventBroker::Subscribe(
    const CapabilityEventSubscription& subscription, std::string* diagnostic,
    std::uint64_t* subscription_id) {
  if (!SafePackageId(subscription.package_id)) {
    if (diagnostic) *diagnostic = "event subscription package id is invalid";
    return false;
  }
  if (subscription.topic_prefix.size() > kMaximumTopicBytes) {
    if (diagnostic) *diagnostic = "event topic prefix exceeds policy";
    return false;
  }
  if (subscription.queue_limit == 0 ||
      subscription.queue_limit > kMaximumQueueLimit) {
    if (diagnostic) *diagnostic = "event queue limit exceeds policy";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto& subscriptions = packages_[subscription.package_id].subscriptions;
  const bool duplicate =
      std::any_of(subscriptions.begin(), subscriptions.end(),
                  [&subscription](const auto& existing) {
                    return existing.kind == subscription.kind &&
                           existing.topic_prefix == subscription.topic_prefix;
                  });
  if (duplicate) {
    if (diagnostic) *diagnostic = "event subscription is duplicated";
    return false;
  }
  CapabilityEventSubscription stored = subscription;
  stored.id = next_subscription_id_++;
  subscriptions.push_back(stored);
  if (subscription_id) *subscription_id = stored.id;
  return true;
}

bool CapabilityEventBroker::Unsubscribe(const std::string& package_id,
                                        std::uint64_t subscription_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto package = packages_.find(package_id);
  if (package == packages_.end()) return false;
  auto& subscriptions = package->second.subscriptions;
  const auto item =
      std::find_if(subscriptions.begin(), subscriptions.end(),
                   [subscription_id](const auto& subscription) {
                     return subscription.id == subscription_id;
                   });
  if (item == subscriptions.end()) return false;
  subscriptions.erase(item);
  return true;
}

void CapabilityEventBroker::RemovePackage(const std::string& package_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  packages_.erase(package_id);
}

void CapabilityEventBroker::ClearPending(const std::string& package_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto item = packages_.find(package_id);
  if (item == packages_.end()) return;
  item->second.pending.clear();
  item->second.stats.pending = 0;
}

bool CapabilityEventBroker::Matches(
    const CapabilityEventSubscription& subscription,
    const CapabilityEvent& event) {
  return subscription.kind == event.kind &&
         (subscription.topic_prefix.empty() ||
          event.topic.rfind(subscription.topic_prefix, 0) == 0);
}

bool CapabilityEventBroker::Coalesces(CapabilityEventKind kind) {
  switch (kind) {
    case CapabilityEventKind::kNavigationPosition:
    case CapabilityEventKind::kAisTarget:
    case CapabilityEventKind::kActiveLeg:
    case CapabilityEventKind::kCursor:
    case CapabilityEventKind::kViewport:
      return true;
    default:
      return false;
  }
}

bool CapabilityEventBroker::Publish(CapabilityEvent event,
                                    std::string* diagnostic) {
  if (event.topic.size() > kMaximumTopicBytes) {
    if (diagnostic) *diagnostic = "event topic exceeds policy";
    return false;
  }
  if (event.payload.size() > kMaximumPayloadBytes) {
    if (diagnostic) *diagnostic = "event payload exceeds policy";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  event.sequence = next_sequence_++;
  for (auto& [package_id, queue] : packages_) {
    (void)package_id;
    std::size_t queue_limit = 0;
    for (const auto& subscription : queue.subscriptions) {
      if (Matches(subscription, event))
        queue_limit = std::max(queue_limit, subscription.queue_limit);
    }
    if (queue_limit == 0) continue;

    if (Coalesces(event.kind)) {
      const auto existing = std::find_if(
          queue.pending.begin(), queue.pending.end(),
          [&event](const auto& queued) {
            return queued.kind == event.kind && queued.topic == event.topic;
          });
      if (existing != queue.pending.end()) {
        *existing = event;
        ++queue.stats.coalesced;
        ++queue.stats.accepted;
        continue;
      }
    }
    if (queue.pending.size() >= queue_limit) {
      queue.pending.pop_front();
      ++queue.stats.dropped;
    }
    queue.pending.push_back(event);
    ++queue.stats.accepted;
    queue.stats.pending = queue.pending.size();
  }
  return true;
}

std::vector<CapabilityEvent> CapabilityEventBroker::Drain(
    const std::string& package_id, std::size_t maximum) {
  std::vector<CapabilityEvent> events;
  if (maximum == 0) return events;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto item = packages_.find(package_id);
  if (item == packages_.end()) return events;
  auto& queue = item->second;
  const std::size_t count = std::min(maximum, queue.pending.size());
  events.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    events.push_back(std::move(queue.pending.front()));
    queue.pending.pop_front();
  }
  queue.stats.pending = queue.pending.size();
  return events;
}

std::size_t CapabilityEventBroker::Pending(
    const std::string& package_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto item = packages_.find(package_id);
  return item == packages_.end() ? 0 : item->second.pending.size();
}

CapabilityEventStats CapabilityEventBroker::Stats(
    const std::string& package_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto item = packages_.find(package_id);
  return item == packages_.end() ? CapabilityEventStats{} : item->second.stats;
}

}  // namespace ppm
