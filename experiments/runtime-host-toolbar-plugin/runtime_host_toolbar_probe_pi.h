#ifndef RUNTIME_HOST_TOOLBAR_PROBE_PI_H
#define RUNTIME_HOST_TOOLBAR_PROBE_PI_H

#include <wx/timer.h>
#include <wx/wx.h>

#include "action_registry.h"
#include "ocpn_plugin.h"

class RuntimeHostToolbarProbePi : public wxEvtHandler,
                                  public opencpn_plugin_121 {
 public:
  explicit RuntimeHostToolbarProbePi(void* manager);
  ~RuntimeHostToolbarProbePi() override;

  int Init() override;
  bool DeInit() override;
  int GetAPIVersionMajor() override { return 1; }
  int GetAPIVersionMinor() override { return 21; }
  int GetPlugInVersionMajor() override { return 0; }
  int GetPlugInVersionMinor() override { return 1; }
  int GetToolbarToolCount() override { return 3; }
  wxBitmap* GetPlugInBitmap() override { return &plugin_bitmap_; }
  wxString GetCommonName() override { return "Runtime Host Toolbar Probe"; }
  wxString GetShortDescription() override {
    return "Experimental child-action toolbar lifecycle probe";
  }
  wxString GetLongDescription() override {
    return "Registers and routes manager, iGRIB, and iWeatherRouting actions "
           "using only the public OpenCPN plugin API.";
  }
  void OnToolbarToolCallback(int id) override;

 private:
  bool RegisterAction(const LogicalActionKey& key, const wxString& label,
                      const wxString& tooltip, const wxString& icon_file,
                      bool force_rebuild);
  bool RemoveAction(const LogicalActionKey& key, const char* reason);
  void OnProbeTimer(wxTimerEvent& event);
  void LogRegistry(const char* event, const RegisteredAction& action) const;

  ActionRegistry registry_;
  wxBitmap plugin_bitmap_;
  wxTimer timer_;
  int probe_stage_ = 0;
  bool initialized_ = false;
};

#endif
