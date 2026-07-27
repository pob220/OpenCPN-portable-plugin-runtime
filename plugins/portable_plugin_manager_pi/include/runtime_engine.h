#ifndef PORTABLE_PLUGIN_MANAGER_RUNTIME_ENGINE_H
#define PORTABLE_PLUGIN_MANAGER_RUNTIME_ENGINE_H

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "declarative_ui.h"
#include "routing_service.h"

struct PlugIn_Position_Fix_Ex;

namespace ppm {

struct RuntimeAction {
  std::string package_id;
  std::string action_id;
  std::string label;
  std::string tooltip;
  std::string icon_path;
  bool toolbar = true;
  bool context_menu = false;
  std::vector<std::string> locations;
};

struct RuntimeActionContext {
  std::string location = "toolbar";
  std::uint32_t canvas_index = 0;
  bool has_canvas_index = false;
  double latitude = 0.0;
  double longitude = 0.0;
  bool has_position = false;
  std::string object_kind;
  std::string object_id;
};

struct PackageSnapshot {
  std::string id;
  std::string name;
  std::string version;
  std::string state;
  std::string access;
  std::string diagnostic;
  std::uint64_t generation = 0;
  std::size_t pending_calls = 0;
  std::uint64_t enable_count = 0;
  std::uint64_t disable_count = 0;
  std::size_t surface_count = 0;
  std::size_t job_count = 0;
  std::size_t provided_service_count = 0;
  std::size_t required_service_count = 0;
};

struct OverlayPoint {
  double latitude = 0.0;
  double longitude = 0.0;
};

struct OverlayColor {
  unsigned char red = 255;
  unsigned char green = 255;
  unsigned char blue = 255;
  unsigned char alpha = 255;
};

struct OverlayStyle {
  bool has_stroke = true;
  OverlayColor stroke;
  bool has_fill = false;
  OverlayColor fill;
  float width_pixels = 1.0F;
  std::vector<float> dash_pattern;
};

enum class OverlayPrimitiveKind {
  kPolyline,
  kPolygon,
  kCircle,
  kIcon,
  kText,
};

struct OverlayPrimitive {
  std::string primitive_id;
  OverlayPrimitiveKind kind = OverlayPrimitiveKind::kPolyline;
  std::vector<OverlayPoint> points;
  OverlayPoint centre;
  double radius_metres = 0.0;
  OverlayStyle style;
  std::string resource_path;
  std::string text;
  float width_pixels = 0.0F;
  float height_pixels = 0.0F;
  float size_pixels = 0.0F;
  float rotation_degrees = 0.0F;
  float anchor_x = 0.5F;
  float anchor_y = 0.5F;
  std::string horizontal_alignment = "left";
  OverlayColor text_color;
  bool interactive = false;
};

struct OverlayLayer {
  std::string layer_id;
  int z_index = 0;
  bool visible = true;
  std::vector<OverlayPrimitive> primitives;
};

struct OverlayScene {
  std::string package_id;
  std::string scene_id;
  std::uint64_t revision = 0;
  std::string canvas_target = "all";
  std::vector<std::uint32_t> selected_canvases;
  std::string render_phase = "below-vessels";
  std::vector<OverlayLayer> layers;

  // API 0.1/0.2 compatibility representation.
  std::vector<OverlayPoint> points;
  unsigned char red = 255;
  unsigned char green = 255;
  unsigned char blue = 255;
  unsigned char alpha = 255;
  float width_pixels = 1.0F;
};

class RuntimeEngine {
public:
  using RegisterAction =
      std::function<int(const RuntimeAction&, std::uint32_t*)>;
  using RemoveActions = std::function<void(const std::string&)>;
  using StateChanged = std::function<void()>;
  using UiDispatch = std::function<void(std::function<void()>)>;
  using SurfaceOpened =
      std::function<void(const std::string&, const DeclarativeSurface&)>;
  using SurfaceResponse = std::function<void(
      const std::string&, const std::string&, const std::string&,
      const std::string&, const std::string&)>;
  using RoutingProgress =
      std::function<void(const std::string&, std::uint8_t, const std::string&)>;
  using RoutingCompleted = std::function<void(
      const std::string&, bool, RoutingOutcome, const std::string&)>;
  using PluginMessageSender =
      std::function<void(const std::string&, const std::string&)>;
  using AuthorUiRequest =
      std::function<int(const std::string&, const std::string&,
                        const std::string&, std::string*)>;

  RuntimeEngine(std::string storage_root, RegisterAction register_action,
                RemoveActions remove_actions, StateChanged state_changed,
                UiDispatch ui_dispatch = {});
  ~RuntimeEngine();

  RuntimeEngine(const RuntimeEngine&) = delete;
  RuntimeEngine& operator=(const RuntimeEngine&) = delete;

  bool LoadInstalled(bool developer_mode);
  void SetSurfaceOpenedCallback(SurfaceOpened callback);
  void SetSurfaceResponseCallback(SurfaceResponse callback);
  void SetRoutingProgressCallback(RoutingProgress callback);
  void SetRoutingCompletedCallback(RoutingCompleted callback);
  void SetPluginMessageSender(PluginMessageSender callback);
  void SetAuthorUiRequestCallback(AuthorUiRequest callback);
  bool RefreshPackage(const std::string& package_id, bool developer_mode,
                      std::string* diagnostic);
  bool Enable(const std::string& package_id, std::string* diagnostic);
  bool SetGrantedPermissions(
      const std::string& package_id,
      const std::vector<std::string>& granted_permissions,
      std::string* diagnostic);
  std::vector<std::string> RequestedPermissions(
      const std::string& package_id) const;
  bool Disable(const std::string& package_id, std::string* diagnostic);
  bool Unload(const std::string& package_id, std::string* diagnostic);
  bool IsEnabled(const std::string& package_id) const;
  void Shutdown();
  bool HandleAction(const std::string& package_id, const std::string& action_id,
                    RuntimeActionContext context = {});
  bool HandleSurfaceEvent(const std::string& package_id,
                          const std::string& surface_id,
                          const std::string& control_id,
                          const std::string& value_json);
  bool RegisterUserFileGrant(const std::string& package_id,
                             const std::string& path, bool writable,
                             std::string* token, std::string* diagnostic);
  bool SelectEnvironmentDataset(const std::string& package_id,
                                const std::vector<std::string>& selected_paths);
  std::string EnvironmentSummary(const std::string& package_id) const;
  bool StartRoute(const std::string& package_id, RoutingRequest request,
                  std::string* diagnostic);
  bool CalculateRouteBlocking(const std::string& package_id,
                              RoutingRequest request, RoutingOutcome* outcome,
                              std::string* diagnostic);
  bool CalculatePassageBlocking(const std::string& package_id,
                                RoutingPassageRequest request,
                                RoutingOutcome* outcome,
                                std::string* diagnostic);
  bool BeginRouteAttempt(const std::string& package_id,
                         std::string* diagnostic);
  bool PreflightEnvironment(const std::string& package_id, double latitude,
                            double longitude,
                            const std::vector<std::int64_t>& unix_times,
                            std::vector<std::uint8_t>* availability,
                            std::string* diagnostic);
  bool CancelRoute(const std::string& package_id);
  bool WaitForRoute(const std::string& package_id,
                    std::chrono::milliseconds timeout);
  bool WaitForIdle(const std::string& package_id,
                   std::chrono::milliseconds timeout);
  void SetPositionFix(const PlugIn_Position_Fix_Ex& fix);
  void SetCursorPosition(double latitude, double longitude);
  void SetViewport(double west, double south, double east, double north,
                   double scale_ppm, double rotation, int canvas_index);
  void SetActiveLeg(double cross_track_error_nm, double bearing_degrees,
                    double distance_nm, const std::string& waypoint_name,
                    bool arrival);
  void DeliverNavigationSentence(const std::string& sentence);
  void DeliverNmea2000(std::uint32_t pgn, const std::string& source,
                       const std::vector<std::uint8_t>& payload);
  void DeliverAisSentence(const std::string& sentence);
  void DeliverSignalK(const std::string& payload);
  void DeliverHostEnvironment(const std::string& payload);
  bool DeliverPointerEvent(std::uint32_t kind, std::uint32_t button,
                           std::uint32_t canvas_index, std::int32_t x_pixels,
                           std::int32_t y_pixels, double latitude,
                           double longitude, bool has_position,
                           std::int32_t wheel_rotation, std::uint32_t modifiers,
                           const std::string& hit_package_id,
                           const std::string& hit_scene_id,
                           const std::string& hit_primitive_id);
  bool DeliverKeyEvent(std::uint32_t key_code, std::uint32_t unicode,
                       bool has_unicode, bool pressed, bool repeat,
                       std::uint32_t modifiers);
  void DeliverPluginMessage(const std::string& message_id,
                            const std::string& message_body);
  std::vector<PackageSnapshot> Packages() const;
  std::vector<OverlayScene> Scenes() const;
  const std::string& StorageRoot() const { return storage_root_; }

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  std::string storage_root_;
};

}  // namespace ppm

#endif
