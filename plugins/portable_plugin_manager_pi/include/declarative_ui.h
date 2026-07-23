#ifndef PORTABLE_PLUGIN_MANAGER_DECLARATIVE_UI_H
#define PORTABLE_PLUGIN_MANAGER_DECLARATIVE_UI_H

#include <string>
#include <vector>

#include <wx/jsonval.h>

namespace ppm {

struct UiMenuItem {
  std::string id;
  std::string label;
  std::string accelerator;
  bool separator = false;
  bool checkable = false;
};

struct UiMenu {
  std::string label;
  std::vector<UiMenuItem> items;
};

struct UiControl {
  std::string id;
  std::string tab;
  std::string type;
  std::string label;
  std::string file_filter;
  std::vector<std::string> columns;
  bool icon_only = false;
};

struct DeclarativeSurface {
  std::string id;
  std::string title;
  std::string role;
  std::string kind;
  std::vector<std::string> tabs;
  std::vector<UiMenu> menus;
  std::vector<UiControl> controls;
};

bool ParseDeclarativeSurface(const wxJSONValue& document,
                             const std::string& expected_surface_id,
                             DeclarativeSurface* surface,
                             std::string* diagnostic);

}  // namespace ppm

#endif
