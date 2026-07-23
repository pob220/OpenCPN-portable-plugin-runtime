#ifndef PORTABLE_PLUGIN_MANAGER_ACTION_REGISTRY_H
#define PORTABLE_PLUGIN_MANAGER_ACTION_REGISTRY_H

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ppm {

struct ActionKey {
  std::string package_id;
  std::string action_id;

  bool operator<(const ActionKey& other) const;
  bool operator==(const ActionKey& other) const;
};

struct Action {
  ActionKey key;
  int tool_id = -1;
  bool checked = false;
  bool dispatchable = true;
};

class ActionRegistry {
 public:
  bool Add(const ActionKey& key, int tool_id);
  bool Remove(const ActionKey& key);
  std::vector<Action> RemovePackage(const std::string& package_id);
  std::optional<Action> FindByToolId(int tool_id) const;
  Action* Find(const ActionKey& key);
  const Action* Find(const ActionKey& key) const;
  std::vector<Action> Clear();
  std::size_t Size() const { return by_key_.size(); }

 private:
  std::map<ActionKey, Action> by_key_;
  std::map<int, ActionKey> by_tool_id_;
};

}  // namespace ppm

#endif
