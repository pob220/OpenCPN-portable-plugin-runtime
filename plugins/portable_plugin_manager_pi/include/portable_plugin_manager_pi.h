#ifndef PORTABLE_PLUGIN_MANAGER_PI_H
#define PORTABLE_PLUGIN_MANAGER_PI_H

#include <memory>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <wx/bitmap.h>

#include "action_registry.h"
#include "ocpn_plugin.h"
#include "package_store.h"
#include "permission_store.h"
#include "runtime_engine.h"

class PortableWeatherRoutingHost;
class PortableEnvironmentHost;
struct PortableNavigationPosition;
struct PortableNavigationRoute;

namespace ppm {

class ManagerDialog;
class SurfaceDialog;

class PortablePluginManagerPi final : public opencpn_plugin_121 {
 public:
  explicit PortablePluginManagerPi(void* manager);
  ~PortablePluginManagerPi() override;

  int Init() override;
  bool DeInit() override;
  int GetAPIVersionMajor() override { return 1; }
  int GetAPIVersionMinor() override { return 21; }
  int GetPlugInVersionMajor() override { return 0; }
  int GetPlugInVersionMinor() override { return 2; }
  int GetPlugInVersionPatch() override { return 1; }
  int GetToolbarToolCount() override { return 1; }
  wxBitmap* GetPlugInBitmap() override { return &plugin_bitmap_; }
  wxString GetCommonName() override { return "Portable Plugin Manager"; }
  wxString GetShortDescription() override;
  wxString GetLongDescription() override;
  void OnToolbarToolCallback(int id) override;
  void OnContextMenuItemCallback(int id) override;
  void ShowPreferencesDialog(wxWindow* parent) override;
  void SetPositionFixEx(PlugIn_Position_Fix_Ex& fix) override;
  void SetNMEASentence(wxString& sentence) override;
  void SetAISSentence(wxString& sentence) override;
  void SetActiveLegInfo(Plugin_Active_Leg_Info& leg_info) override;
  void SetPluginMessage(wxString& message_id,
                        wxString& message_body) override;
  void SetCursorLatLon(double latitude, double longitude) override;
  bool MouseEventHook(wxMouseEvent& event) override;
  bool KeyboardEventHook(wxKeyEvent& event) override;
  bool RenderOverlayMultiCanvas(wxDC& dc, PlugIn_ViewPort* viewport,
                                int canvas_index, int priority) override;
  bool RenderGLOverlayMultiCanvas(wxGLContext* context,
                                  PlugIn_ViewPort* viewport,
                                  int canvas_index, int priority) override;

 private:
  bool RegisterManagerAction();
  int RegisterPortableAction(const RuntimeAction& action,
                             std::uint32_t* host_action_id);
  void RemovePackageActions(const std::string& package_id);
  void RemoveAllActions();
  void InstallPackage(const std::string& archive_path);
  void EnablePackage(const std::string& package_id);
  void DisablePackage(const std::string& package_id);
  void UnloadPackage(const std::string& package_id);
  void RemovePackage(const std::string& package_id);
  void RollbackPackage(const std::string& package_id);
  void RevokePackagePermissions(const std::string& package_id);
  void OpenPackageSurface(const std::string& package_id,
                          const DeclarativeSurface& surface);
  void ApplySurfaceResponse(const std::string& package_id,
                            const std::string& surface_id,
                            const std::string& control_id,
                            const std::string& state_json,
                            const std::string& diagnostic);
  void ClosePackageSurfaces(const std::string& package_id);
  bool PreparePermissions(const std::string& package_id, bool interactive,
                          wxString* diagnostic);
  void SetManagerStatus(const wxString& status);
  bool RestorePreviousPackage(const std::string& package_id,
                              bool enable_after_restore,
                              wxString* diagnostic);
  void ShowManager(wxWindow* parent);
  void OnEngineStateChanged();
  void RefreshManager();
  std::vector<PortableNavigationPosition> ListWaypoints() const;
  std::vector<PortableNavigationRoute> ListRoutes() const;
  bool CreateOpenCpnRoute(
      const wxString& name,
      const std::vector<PortableNavigationPosition>& points,
      wxString* diagnostic);
  int HandleAuthorUiRequest(const std::string& package_id,
                            const std::string& operation,
                            const std::string& request_json,
                            std::string* response_json);
  void HandleNmea2000(std::uint32_t pgn, ObservedEvt event);
  void RefreshSceneHitRegions(PlugIn_ViewPort* viewport, int canvas_index);

  struct SceneHitRegion {
    std::string package_id;
    std::string scene_id;
    std::string primitive_id;
    int canvas_index = 0;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    int z_index = 0;
  };

  ActionRegistry actions_;
  wxBitmap plugin_bitmap_;
  wxString storage_root_;
  std::unique_ptr<PackageStore> package_store_;
  std::unique_ptr<PermissionStore> permission_store_;
  std::unique_ptr<RuntimeEngine> runtime_engine_;
  std::shared_ptr<std::atomic_bool> ui_callback_gate_;
  std::map<std::string, std::unique_ptr<SurfaceDialog>> surface_dialogs_;
  std::unique_ptr<::PortableEnvironmentHost> environment_workbench_;
  std::unique_ptr<::PortableWeatherRoutingHost> weather_routing_host_;
  std::unique_ptr<ManagerDialog> manager_dialog_;
  double vessel_latitude_ = 0.0;
  double vessel_longitude_ = 0.0;
  double cursor_latitude_ = 0.0;
  double cursor_longitude_ = 0.0;
  bool vessel_position_valid_ = false;
  bool cursor_position_valid_ = false;
  double view_west_ = 0.0;
  double view_south_ = 0.0;
  double view_east_ = 0.0;
  double view_north_ = 0.0;
  bool view_bounds_valid_ = false;
  bool developer_smoke_fixture_opened_ = false;
  bool developer_software_overlay_logged_ = false;
  bool developer_opengl_overlay_logged_ = false;
  bool developer_mode_ = false;
  bool initialized_ = false;
  std::map<std::string,
           std::deque<std::chrono::steady_clock::time_point>>
      nmea_output_history_;
  std::vector<SceneHitRegion> scene_hit_regions_;
  std::vector<std::shared_ptr<ObservableListener>> nmea2000_listeners_;
  std::vector<wxEventType> nmea2000_event_types_;
  std::unique_ptr<wxEvtHandler> nmea2000_handler_;
};

}  // namespace ppm

#endif
