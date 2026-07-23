#ifndef PORTABLE_PLUGIN_MANAGER_SURFACE_DIALOG_H
#define PORTABLE_PLUGIN_MANAGER_SURFACE_DIALOG_H

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <wx/frame.h>

#include "declarative_ui.h"

class wxWindow;

namespace ppm {

class SurfaceDialog final : public wxFrame {
 public:
  struct UserFileSelection {
    std::string path;
    bool writable = false;
  };
  using EventCallback =
      std::function<void(const std::string&, const std::string&,
                         const std::vector<UserFileSelection>&)>;

  SurfaceDialog(wxWindow* parent, DeclarativeSurface definition,
                EventCallback callback);

  const std::string& SurfaceId() const { return definition_.id; }
  void ApplyResponse(const std::string& control_id,
                     const std::string& state_json,
                     const std::string& diagnostic);

 private:
  wxWindow* BuildControl(wxWindow* parent, const UiControl& control);
  void SendEvent(const std::string& control_id,
                 const std::string& value_json,
                 std::vector<UserFileSelection> selections = {});

  DeclarativeSurface definition_;
  EventCallback callback_;
  std::map<std::string, wxWindow*> controls_;
  bool applying_response_ = false;
};

}  // namespace ppm

#endif
