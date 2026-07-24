#include "window_activation.h"

#include <wx/app.h>
#include <wx/weakref.h>
#include <wx/window.h>

#include "ocpn_plugin.h"

namespace ppm {

wxWindow* ResolveOpenCpnTopLevelParent(wxWindow* preferred) {
  if (wxTheApp) {
    if (wxWindow* top = wxTheApp->GetTopWindow()) return top;
  }
  if (wxWindow* canvas = GetOCPNCanvasWindow()) {
    if (wxWindow* top = wxGetTopLevelParent(canvas)) return top;
    return canvas;
  }
  if (preferred) {
    if (wxWindow* top = wxGetTopLevelParent(preferred)) return top;
  }
  return preferred;
}

void ShowAndActivateWindow(wxWindow* window) {
  if (!window) return;
  window->Show();
  window->Raise();
  window->SetFocus();
  if (!wxTheApp) return;

  const wxWeakRef<wxWindow> weak_window(window);
  wxTheApp->CallAfter([weak_window]() {
    wxWindow* shown_window = weak_window.get();
    if (!shown_window || !shown_window->IsShown()) return;
    shown_window->Raise();
    shown_window->SetFocus();
  });
}

}  // namespace ppm
