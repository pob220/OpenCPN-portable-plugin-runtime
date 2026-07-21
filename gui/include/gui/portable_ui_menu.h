#ifndef GUI_PORTABLE_UI_MENU_H_
#define GUI_PORTABLE_UI_MENU_H_

#include <vector>

#include <wx/jsonval.h>
#include <wx/string.h>

/** A host-rendered command declared by a portable UI surface. */
struct PortableUiMenuItemDefinition {
  wxString id;
  wxString label;
  wxString accelerator;
  bool separator = false;
  bool checkable = false;
};

/** A host-rendered top-level menu declared by a portable UI surface. */
struct PortableUiMenuDefinition {
  wxString label;
  std::vector<PortableUiMenuItemDefinition> items;
};

/**
 * Parse and validate the generic `menus` section of a portable UI surface.
 *
 * Optional JSON booleans are deliberately read only when present and of the
 * correct type.  wxJSONValue::AsBool() does not perform conversion or supply a
 * default for a missing member.
 */
bool ParsePortableUiMenus(const wxJSONValue& surface,
                          std::vector<PortableUiMenuDefinition>* menus,
                          wxString* error);

#endif
