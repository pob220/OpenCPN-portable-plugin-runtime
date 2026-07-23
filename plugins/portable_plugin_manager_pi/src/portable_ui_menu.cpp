#include "portable_ui_menu.h"

#include <set>
#include <utility>

namespace {
bool OptionalBoolean(const wxJSONValue& object, const wxString& key,
                     bool* value, wxString* error) {
  *value = false;
  if (!object.HasMember(key)) return true;
  const wxJSONValue member = object.ItemAt(key);
  if (!member.IsBool()) {
    if (error)
      *error = "portable UI menu property '" + key + "' must be boolean";
    return false;
  }
  *value = member.AsBool();
  return true;
}

bool OptionalString(const wxJSONValue& object, const wxString& key,
                    wxString* value, wxString* error) {
  value->clear();
  if (!object.HasMember(key)) return true;
  const wxJSONValue member = object.ItemAt(key);
  if (!member.IsString()) {
    if (error)
      *error = "portable UI menu property '" + key + "' must be a string";
    return false;
  }
  *value = member.AsString();
  return true;
}
}  // namespace

bool ParsePortableUiMenus(const wxJSONValue& surface,
                          std::vector<PortableUiMenuDefinition>* menus,
                          wxString* error) {
  if (!menus) {
    if (error) *error = "portable UI menu output is unavailable";
    return false;
  }
  menus->clear();
  if (error) error->clear();
  if (!surface.IsObject() || !surface.HasMember("menus")) {
    if (error) *error = "portable UI must declare at least one menu";
    return false;
  }
  const wxJSONValue definitions = surface.ItemAt("menus");
  if (!definitions.IsArray() || definitions.Size() == 0) {
    if (error) *error = "portable UI must declare at least one menu";
    return false;
  }

  std::set<wxString> actions;
  for (int menu_index = 0; menu_index < definitions.Size(); ++menu_index) {
    const wxJSONValue definition = definitions.ItemAt(menu_index);
    if (!definition.IsObject() || !definition.HasMember("label") ||
        !definition.ItemAt("label").IsString() ||
        definition.ItemAt("label").AsString().empty() ||
        !definition.HasMember("items")) {
      if (error) *error = "portable UI has an invalid or empty menu";
      return false;
    }
    const wxJSONValue items = definition.ItemAt("items");
    if (!items.IsArray() || items.Size() == 0) {
      if (error) *error = "portable UI has an invalid or empty menu";
      return false;
    }
    PortableUiMenuDefinition menu;
    menu.label = definition.ItemAt("label").AsString();
    bool has_command = false;
    for (int item_index = 0; item_index < items.Size(); ++item_index) {
      const wxJSONValue definition_item = items.ItemAt(item_index);
      PortableUiMenuItemDefinition item;
      if (!definition_item.IsObject() ||
          !OptionalBoolean(definition_item, "separator", &item.separator,
                           error) ||
          !OptionalBoolean(definition_item, "checkable", &item.checkable,
                           error) ||
          !OptionalString(definition_item, "accelerator", &item.accelerator,
                          error))
        return false;
      if (item.separator) {
        if (item.checkable || !item.accelerator.empty() ||
            definition_item.HasMember("id") ||
            definition_item.HasMember("label")) {
          if (error)
            *error = "portable UI menu separator has command properties";
          return false;
        }
      } else {
        if (!definition_item.HasMember("id") ||
            !definition_item.ItemAt("id").IsString() ||
            !definition_item.HasMember("label") ||
            !definition_item.ItemAt("label").IsString()) {
          if (error) *error = "portable UI menu command omits its id or label";
          return false;
        }
        item.id = definition_item.ItemAt("id").AsString();
        item.label = definition_item.ItemAt("label").AsString();
        if (item.id.empty() || item.label.empty() ||
            !actions.insert(item.id).second) {
          if (error) *error = "portable UI menu command is empty or duplicated";
          return false;
        }
        has_command = true;
      }
      menu.items.push_back(std::move(item));
    }
    if (!has_command) {
      if (error) *error = "portable UI menu contains only separators";
      return false;
    }
    menus->push_back(std::move(menu));
  }
  return true;
}
