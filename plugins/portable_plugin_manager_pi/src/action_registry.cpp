#include "action_registry.h"

#include <set>

namespace ppm {

bool ActionKey::operator<(const ActionKey& other) const {
  return package_id < other.package_id ||
         (package_id == other.package_id && action_id < other.action_id);
}

bool ActionKey::operator==(const ActionKey& other) const {
  return package_id == other.package_id && action_id == other.action_id;
}

bool ActionRegistry::Add(const ActionKey& key, int tool_id, int context_id) {
  return Add(
      key, tool_id,
      context_id < 0 ? std::vector<int>{} : std::vector<int>{context_id});
}

bool ActionRegistry::Add(const ActionKey& key, int tool_id,
                         const std::vector<int>& context_ids) {
  if ((tool_id < 0 && context_ids.empty()) || by_key_.count(key) != 0 ||
      (tool_id >= 0 && by_tool_id_.count(tool_id) != 0)) {
    return false;
  }
  std::set<int> unique_context_ids;
  for (const int context_id : context_ids) {
    if (context_id < 0 || by_context_id_.count(context_id) != 0 ||
        !unique_context_ids.insert(context_id).second)
      return false;
  }
  Action action;
  action.key = key;
  action.tool_id = tool_id;
  action.context_id = context_ids.empty() ? -1 : context_ids.front();
  action.context_ids = context_ids;
  by_key_.emplace(key, std::move(action));
  if (tool_id >= 0) by_tool_id_.emplace(tool_id, key);
  for (const int context_id : context_ids)
    by_context_id_.emplace(context_id, key);
  return true;
}

bool ActionRegistry::Remove(const ActionKey& key) {
  const auto item = by_key_.find(key);
  if (item == by_key_.end()) return false;
  if (item->second.tool_id >= 0) by_tool_id_.erase(item->second.tool_id);
  for (const int context_id : item->second.context_ids)
    by_context_id_.erase(context_id);
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
    if (item->second.tool_id >= 0) by_tool_id_.erase(item->second.tool_id);
    for (const int context_id : item->second.context_ids)
      by_context_id_.erase(context_id);
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

std::optional<Action> ActionRegistry::FindByContextId(int context_id) const {
  const auto id_item = by_context_id_.find(context_id);
  if (id_item == by_context_id_.end()) return std::nullopt;
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
  by_context_id_.clear();
  return actions;
}

}  // namespace ppm
