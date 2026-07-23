#include "action_registry.h"

namespace ppm {

bool ActionKey::operator<(const ActionKey& other) const {
  return package_id < other.package_id ||
         (package_id == other.package_id && action_id < other.action_id);
}

bool ActionKey::operator==(const ActionKey& other) const {
  return package_id == other.package_id && action_id == other.action_id;
}

bool ActionRegistry::Add(const ActionKey& key, int tool_id) {
  if (tool_id < 0 || by_key_.count(key) != 0 ||
      by_tool_id_.count(tool_id) != 0) {
    return false;
  }
  by_key_.emplace(key, Action{key, tool_id, false, true});
  by_tool_id_.emplace(tool_id, key);
  return true;
}

bool ActionRegistry::Remove(const ActionKey& key) {
  const auto item = by_key_.find(key);
  if (item == by_key_.end()) return false;
  by_tool_id_.erase(item->second.tool_id);
  by_key_.erase(item);
  return true;
}

std::vector<Action> ActionRegistry::RemovePackage(
    const std::string& package_id) {
  std::vector<Action> removed;
  for (auto item = by_key_.begin(); item != by_key_.end();) {
    if (item->first.package_id != package_id) {
      ++item;
      continue;
    }
    removed.push_back(item->second);
    by_tool_id_.erase(item->second.tool_id);
    item = by_key_.erase(item);
  }
  return removed;
}

std::optional<Action> ActionRegistry::FindByToolId(int tool_id) const {
  const auto id_item = by_tool_id_.find(tool_id);
  if (id_item == by_tool_id_.end()) return std::nullopt;
  const auto key_item = by_key_.find(id_item->second);
  if (key_item == by_key_.end()) return std::nullopt;
  return key_item->second;
}

Action* ActionRegistry::Find(const ActionKey& key) {
  const auto item = by_key_.find(key);
  return item == by_key_.end() ? nullptr : &item->second;
}

const Action* ActionRegistry::Find(const ActionKey& key) const {
  const auto item = by_key_.find(key);
  return item == by_key_.end() ? nullptr : &item->second;
}

std::vector<Action> ActionRegistry::Clear() {
  std::vector<Action> actions;
  actions.reserve(by_key_.size());
  for (const auto& item : by_key_) actions.push_back(item.second);
  by_key_.clear();
  by_tool_id_.clear();
  return actions;
}

}  // namespace ppm
