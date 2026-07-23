#ifndef PORTABLE_PLUGIN_MANAGER_PI_H
#define PORTABLE_PLUGIN_MANAGER_PI_H

#include <memory>

#include <wx/bitmap.h>

#include "action_registry.h"
#include "ocpn_plugin.h"
#include "runtime_engine.h"

namespace ppm {

class ManagerDialog;

class PortablePluginManagerPi final : public opencpn_plugin_121 {
 public:
  explicit PortablePluginManagerPi(void* manager);
  ~PortablePluginManagerPi() override;

  int Init() override;
  bool DeInit() override;
  int GetAPIVersionMajor() override { return 1; }
  int GetAPIVersionMinor() override { return 21; }
  int GetPlugInVersionMajor() override { return 0; }
  int GetPlugInVersionMinor() override { return 1; }
  int GetPlugInVersionPatch() override { return 0; }
  int GetToolbarToolCount() override { return 1; }
  wxBitmap* GetPlugInBitmap() override { return &plugin_bitmap_; }
  wxString GetCommonName() override { return "Portable Plugin Manager"; }
  wxString GetShortDescription() override;
  wxString GetLongDescription() override;
  void OnToolbarToolCallback(int id) override;
  void ShowPreferencesDialog(wxWindow* parent) override;
  void SetPositionFixEx(PlugIn_Position_Fix_Ex& fix) override;
  bool RenderOverlayMultiCanvas(wxDC& dc, PlugIn_ViewPort* viewport,
                                int canvas_index, int priority) override;
  bool RenderGLOverlayMultiCanvas(wxGLContext* context,
                                  PlugIn_ViewPort* viewport,
                                  int canvas_index, int priority) override;

 private:
  bool RegisterManagerAction();
  int RegisterPortableAction(const RuntimeAction& action,
                             std::uint32_t* host_action_id);
  void RemoveAllActions();
  void ShowManager(wxWindow* parent);
  void OnEngineStateChanged();
  void RefreshManager();

  ActionRegistry actions_;
  wxBitmap plugin_bitmap_;
  wxString storage_root_;
  std::unique_ptr<RuntimeEngine> runtime_engine_;
  std::unique_ptr<ManagerDialog> manager_dialog_;
  bool initialized_ = false;
};

}  // namespace ppm

#endif
