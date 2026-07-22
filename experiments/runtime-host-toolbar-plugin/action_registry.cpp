#include "action_registry.h"

bool ActionRegistry::Add(const LogicalActionKey& key, int tool_id) {
  if (tool_id < 0 || by_key_.count(key) || by_tool_id_.count(tool_id)) {
    return false;
  }
  by_key_.emplace(key, RegisteredAction{key, tool_id, false, true});
  by_tool_id_.emplace(tool_id, key);
  return true;
}

bool ActionRegistry::Remove(const LogicalActionKey& key) {
  auto it = by_key_.find(key);
  if (it == by_key_.end()) return false;
  by_tool_id_.erase(it->second.tool_id);
  by_key_.erase(it);
  return true;
}

std::optional<RegisteredAction> ActionRegistry::FindByToolId(int tool_id) const {
  auto id_it = by_tool_id_.find(tool_id);
  if (id_it == by_tool_id_.end()) return std::nullopt;
  auto key_it = by_key_.find(id_it->second);
  if (key_it == by_key_.end()) return std::nullopt;
  return key_it->second;
}

RegisteredAction* ActionRegistry::Find(const LogicalActionKey& key) {
  auto it = by_key_.find(key);
  return it == by_key_.end() ? nullptr : &it->second;
}

const RegisteredAction* ActionRegistry::Find(
    const LogicalActionKey& key) const {
  auto it = by_key_.find(key);
  return it == by_key_.end() ? nullptr : &it->second;
}
