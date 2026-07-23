#include "declarative_ui.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <utility>

namespace ppm {
namespace {

constexpr int kMaximumMenus = 16;
constexpr int kMaximumMenuItems = 128;
constexpr int kMaximumTabs = 16;
constexpr int kMaximumControls = 256;
constexpr int kMaximumColumns = 32;
constexpr std::size_t kMaximumId = 96;
constexpr std::size_t kMaximumLabel = 512;

std::string Utf8(const wxString& value) {
  const wxScopedCharBuffer bytes = value.utf8_str();
  return bytes ? std::string(bytes.data(), bytes.length()) : std::string();
}

bool SafeId(const std::string& value) {
  return !value.empty() && value.size() <= kMaximumId &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isalnum(character) || character == '-' ||
                  character == '_' || character == '.';
         });
}

bool RequiredString(const wxJSONValue& object, const wxString& key,
                    std::string* value, std::string* diagnostic,
                    std::size_t limit = kMaximumLabel) {
  const wxJSONValue member = object.ItemAt(key);
  if (!object.HasMember(key) || !member.IsString()) {
    *diagnostic = "missing or mistyped string property: " +
                  key.ToStdString();
    return false;
  }
  *value = Utf8(member.AsString());
  if (value->empty() || value->size() > limit) {
    *diagnostic = "empty or oversized string property: " +
                  key.ToStdString();
    return false;
  }
  return true;
}

bool OptionalString(const wxJSONValue& object, const wxString& key,
                    std::string* value, std::string* diagnostic,
                    std::size_t limit = kMaximumLabel) {
  value->clear();
  if (!object.HasMember(key)) return true;
  const wxJSONValue member = object.ItemAt(key);
  if (!member.IsString()) {
    *diagnostic = "mistyped string property: " + key.ToStdString();
    return false;
  }
  *value = Utf8(member.AsString());
  if (value->size() > limit) {
    *diagnostic = "oversized string property: " + key.ToStdString();
    return false;
  }
  return true;
}

bool OptionalBoolean(const wxJSONValue& object, const wxString& key,
                     bool* value, std::string* diagnostic) {
  *value = false;
  if (!object.HasMember(key)) return true;
  const wxJSONValue member = object.ItemAt(key);
  if (!member.IsBool()) {
    *diagnostic = "mistyped boolean property: " + key.ToStdString();
    return false;
  }
  *value = member.AsBool();
  return true;
}

bool ParseMenus(const wxJSONValue& document, std::vector<UiMenu>* menus,
                std::set<std::string>* command_ids,
                std::string* diagnostic) {
  if (!document.HasMember("menus")) return true;
  const wxJSONValue definitions = document.ItemAt("menus");
  if (!definitions.IsArray() || definitions.Size() == 0 ||
      definitions.Size() > kMaximumMenus) {
    *diagnostic = "menus must be a non-empty bounded array";
    return false;
  }
  int total_items = 0;
  for (int menu_index = 0; menu_index < definitions.Size(); ++menu_index) {
    const wxJSONValue definition = definitions.ItemAt(menu_index);
    const wxJSONValue items = definition.ItemAt("items");
    UiMenu menu;
    if (!definition.IsObject() ||
        !RequiredString(definition, "label", &menu.label, diagnostic) ||
        !definition.HasMember("items") || !items.IsArray() ||
        items.Size() == 0) {
      *diagnostic = "portable UI has an invalid or empty menu";
      return false;
    }
    bool has_command = false;
    for (int item_index = 0; item_index < items.Size(); ++item_index) {
      if (++total_items > kMaximumMenuItems) {
        *diagnostic = "portable UI declares too many menu items";
        return false;
      }
      const wxJSONValue item_value = items.ItemAt(item_index);
      UiMenuItem item;
      if (!item_value.IsObject() ||
          !OptionalBoolean(item_value, "separator", &item.separator,
                           diagnostic) ||
          !OptionalBoolean(item_value, "checkable", &item.checkable,
                           diagnostic) ||
          !OptionalString(item_value, "accelerator", &item.accelerator,
                          diagnostic)) {
        return false;
      }
      if (item.separator) {
        if (item.checkable || !item.accelerator.empty() ||
            item_value.HasMember("id") || item_value.HasMember("label")) {
          *diagnostic = "menu separator has command properties";
          return false;
        }
      } else {
        if (!RequiredString(item_value, "id", &item.id, diagnostic,
                            kMaximumId) ||
            !RequiredString(item_value, "label", &item.label, diagnostic) ||
            !SafeId(item.id) || !command_ids->insert(item.id).second) {
          *diagnostic = "menu command identifier is invalid or duplicated";
          return false;
        }
        has_command = true;
      }
      menu.items.push_back(std::move(item));
    }
    if (!has_command) {
      *diagnostic = "menu contains only separators";
      return false;
    }
    menus->push_back(std::move(menu));
  }
  return true;
}

bool ParseTabs(const wxJSONValue& document, std::vector<std::string>* tabs,
               std::set<std::string>* tab_names, std::string* diagnostic) {
  if (!document.HasMember("tabs")) return true;
  const wxJSONValue values = document.ItemAt("tabs");
  if (!values.IsArray() || values.Size() == 0 ||
      values.Size() > kMaximumTabs) {
    *diagnostic = "tabs must be a non-empty bounded array";
    return false;
  }
  for (int index = 0; index < values.Size(); ++index) {
    const wxJSONValue value = values.ItemAt(index);
    if (!value.IsString()) {
      *diagnostic = "tab label must be a string";
      return false;
    }
    const std::string tab = Utf8(value.AsString());
    if (tab.empty() || tab.size() > kMaximumLabel ||
        !tab_names->insert(tab).second) {
      *diagnostic = "tab label is empty, oversized or duplicated";
      return false;
    }
    tabs->push_back(tab);
  }
  return true;
}

bool SupportedControlType(const std::string& type) {
  static const std::set<std::string> supported = {
      "button",          "cancel",          "choice",
      "departure-time",  "diagnostics",     "file-open",
      "file-open-multiple", "file-save",    "integer",
      "navigation-create", "navigation-object", "navigation-route",
      "number",          "optional-number", "position-source",
      "progress",        "slider",          "status",
      "table",           "text",            "toggle"};
  return supported.count(type) != 0;
}

bool ParseControls(const wxJSONValue& document,
                   const std::set<std::string>& tabs,
                   std::vector<UiControl>* controls,
                   std::set<std::string>* control_ids,
                   std::string* diagnostic) {
  const wxJSONValue values = document.ItemAt("controls");
  if (!document.HasMember("controls") || !values.IsArray() ||
      values.Size() == 0 || values.Size() > kMaximumControls) {
    *diagnostic = "controls must be a non-empty bounded array";
    return false;
  }
  for (int index = 0; index < values.Size(); ++index) {
    const wxJSONValue value = values.ItemAt(index);
    UiControl control;
    if (!value.IsObject() ||
        !RequiredString(value, "id", &control.id, diagnostic, kMaximumId) ||
        !RequiredString(value, "type", &control.type, diagnostic,
                        kMaximumId) ||
        !RequiredString(value, "label", &control.label, diagnostic) ||
        !OptionalString(value, "tab", &control.tab, diagnostic) ||
        !OptionalBoolean(value, "icon_only", &control.icon_only,
                         diagnostic) ||
        !SafeId(control.id) || !control_ids->insert(control.id).second ||
        !SupportedControlType(control.type)) {
      if (diagnostic->empty())
        *diagnostic =
            "control identifier is invalid/duplicated or type unsupported";
      return false;
    }
    if ((!tabs.empty() && control.tab.empty()) ||
        (!control.tab.empty() && tabs.count(control.tab) == 0)) {
      *diagnostic = "control refers to a missing tab";
      return false;
    }
    if (value.HasMember("columns")) {
      const wxJSONValue columns = value.ItemAt("columns");
      if (control.type != "table" || !columns.IsArray() ||
          columns.Size() == 0 || columns.Size() > kMaximumColumns) {
        *diagnostic = "table columns are invalid or exceed policy";
        return false;
      }
      for (int column = 0; column < columns.Size(); ++column) {
        const wxJSONValue column_value = columns.ItemAt(column);
        if (!column_value.IsString()) {
          *diagnostic = "table column label must be a string";
          return false;
        }
        const std::string label = Utf8(column_value.AsString());
        if (label.empty() || label.size() > kMaximumLabel) {
          *diagnostic = "table column label is empty or oversized";
          return false;
        }
        control.columns.push_back(label);
      }
    } else if (control.type == "table") {
      *diagnostic = "table control omits its columns";
      return false;
    }
    controls->push_back(std::move(control));
  }
  return true;
}

}  // namespace

bool ParseDeclarativeSurface(const wxJSONValue& document,
                             const std::string& expected_surface_id,
                             DeclarativeSurface* surface,
                             std::string* diagnostic) {
  if (!surface || !diagnostic) return false;
  *surface = {};
  diagnostic->clear();
  if (!document.IsObject()) {
    *diagnostic = "portable UI document must be an object";
    return false;
  }
  const wxJSONValue schema = document.ItemAt("schema");
  const wxJSONValue schema_version = document.ItemAt("schema_version");
  const bool modern =
      document.HasMember("schema") && schema.IsString() &&
      schema.AsString() == "org.opencpn.portable-ui/0.2";
  const bool legacy =
      document.HasMember("schema_version") && schema_version.IsInt() &&
      schema_version.AsInt() == 2;
  const wxString surface_key = modern ? "surface" : "surface_id";
  if ((!modern && !legacy) ||
      !RequiredString(document, surface_key, &surface->id, diagnostic,
                      kMaximumId) ||
      !SafeId(surface->id) ||
      (!expected_surface_id.empty() && surface->id != expected_surface_id) ||
      !RequiredString(document, "title", &surface->title, diagnostic) ||
      !OptionalString(document, "role", &surface->role, diagnostic,
                      kMaximumId) ||
      !OptionalString(document, "kind", &surface->kind, diagnostic,
                      kMaximumId)) {
    if (diagnostic->empty())
      *diagnostic = "surface schema or identity is invalid";
    return false;
  }
  std::set<std::string> menu_commands;
  std::set<std::string> tabs;
  std::set<std::string> control_ids;
  return ParseMenus(document, &surface->menus, &menu_commands, diagnostic) &&
         ParseTabs(document, &surface->tabs, &tabs, diagnostic) &&
         ParseControls(document, tabs, &surface->controls, &control_ids,
                       diagnostic);
}

}  // namespace ppm
