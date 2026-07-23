#ifndef PORTABLE_PLUGIN_MANAGER_RUNTIME_ENGINE_H
#define PORTABLE_PLUGIN_MANAGER_RUNTIME_ENGINE_H

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct PlugIn_Position_Fix_Ex;

namespace ppm {

struct RuntimeAction {
  std::string package_id;
  std::string action_id;
  std::string label;
  std::string tooltip;
  std::string icon_path;
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
};

struct OverlayPoint {
  double latitude = 0.0;
  double longitude = 0.0;
};

struct OverlayScene {
  std::string package_id;
  std::string scene_id;
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

  RuntimeEngine(std::string storage_root, RegisterAction register_action,
                RemoveActions remove_actions, StateChanged state_changed,
                UiDispatch ui_dispatch = {});
  ~RuntimeEngine();

  RuntimeEngine(const RuntimeEngine&) = delete;
  RuntimeEngine& operator=(const RuntimeEngine&) = delete;

  bool LoadInstalled(bool developer_mode);
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
  bool HandleAction(const std::string& package_id,
                    const std::string& action_id);
  bool WaitForIdle(const std::string& package_id,
                   std::chrono::milliseconds timeout);
  void SetPositionFix(const PlugIn_Position_Fix_Ex& fix);
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
