#include "portable_ui_menu.h"

#include <iostream>
#include <set>

#include <wx/jsonreader.h>
#include <wx/wfstream.h>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: portable_ui_menu_test <surface.json>\n";
    return 2;
  }
  wxFileInputStream input(wxString::FromUTF8(argv[1]));
  wxJSONValue surface;
  wxJSONReader reader;
  if (!input.IsOk() || reader.Parse(input, &surface) != 0) {
    std::cerr << "could not read portable UI fixture\n";
    return 1;
  }

  std::vector<PortableUiMenuDefinition> menus;
  wxString error;
  if (!ParsePortableUiMenus(surface, &menus, &error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::set<wxString> actions;
  size_t separators = 0;
  size_t checkable = 0;
  for (const auto& menu : menus) {
    for (const auto& item : menu.items) {
      if (item.separator)
        ++separators;
      else
        actions.insert(item.id);
      if (item.checkable) ++checkable;
    }
  }
  if (menus.size() != 5 || actions.size() != 16 || separators != 3 ||
      checkable != 5 || !actions.count("close") ||
      !actions.count("refresh-positions") || !actions.count("new-routing") ||
      !actions.count("show-configuration") || !actions.count("about") ||
      menus.front().items.front().separator ||
      menus.front().items.front().checkable) {
    std::cerr << "portable workbench menu model is incomplete\n";
    return 1;
  }

  wxJSONValue invalid = surface;
  invalid["menus"][0]["items"][0]["separator"] = "false";
  if (ParsePortableUiMenus(invalid, &menus, &error)) {
    std::cerr << "non-boolean menu flags were accepted\n";
    return 1;
  }

  invalid = surface;
  invalid["menus"][2]["items"][2]["id"] = "separator-command";
  if (ParsePortableUiMenus(invalid, &menus, &error)) {
    std::cerr << "menu separator command properties were accepted\n";
    return 1;
  }
  std::cout << "portable UI menu parser test passed\n";
  return 0;
}
