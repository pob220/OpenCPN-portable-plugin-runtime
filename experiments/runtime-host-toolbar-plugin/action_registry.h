#ifndef RUNTIME_HOST_TOOLBAR_PROBE_ACTION_REGISTRY_H
#define RUNTIME_HOST_TOOLBAR_PROBE_ACTION_REGISTRY_H

#include <map>
#include <optional>
#include <string>

struct LogicalActionKey {
  std::string package_id;
  std::string action_id;

  bool operator<(const LogicalActionKey& other) const {
    return package_id < other.package_id ||
           (package_id == other.package_id && action_id < other.action_id);
  }

  bool operator==(const LogicalActionKey& other) const {
    return package_id == other.package_id && action_id == other.action_id;
  }
};

struct RegisteredAction {
  LogicalActionKey key;
  int tool_id = -1;
  bool checked = false;
  bool dispatchable = true;
};

class ActionRegistry {
 public:
  bool Add(const LogicalActionKey& key, int tool_id);
  bool Remove(const LogicalActionKey& key);
  std::optional<RegisteredAction> FindByToolId(int tool_id) const;
  RegisteredAction* Find(const LogicalActionKey& key);
  const RegisteredAction* Find(const LogicalActionKey& key) const;
  std::size_t Size() const { return by_key_.size(); }

 private:
  std::map<LogicalActionKey, RegisteredAction> by_key_;
  std::map<int, LogicalActionKey> by_tool_id_;
};

#endif
