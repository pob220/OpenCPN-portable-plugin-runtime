#include "runtime_engine.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

#include <wx/init.h>

#define CHECK(expression)                                                   \
  do {                                                                      \
    if (!(expression)) {                                                    \
      std::cerr << "check failed at " << __FILE__ << ':' << __LINE__       \
                << ": " #expression "\n";                                  \
      return 1;                                                             \
    }                                                                       \
  } while (false)

namespace {

namespace fs = std::filesystem;

bool Write(const fs::path& path, const std::string& contents) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  if (error) return false;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return static_cast<bool>(output);
}

const ppm::PackageSnapshot* Snapshot(
    const std::vector<ppm::PackageSnapshot>& packages,
    const std::string& package_id) {
  for (const auto& package : packages) {
    if (package.id == package_id) return &package;
  }
  return nullptr;
}

}  // namespace

extern "C" bool PlugIn_GSHHS_CrossesLand(double, double, double, double) {
  return false;
}

int main() {
  wxInitializer wx;
  CHECK(wx.IsOk());
  const auto stamp =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const fs::path test_root =
      fs::temp_directory_path() /
      fs::path("ppm runtime lifecycle " + std::to_string(stamp));
  const std::string package_id = "org.opencpn.igrib";
  const fs::path package_root = test_root / "packages" / package_id;
  std::error_code error;
  fs::create_directories(package_root / "component", error);
  CHECK(!error);
  fs::copy_file(PPM_TEST_IGRIB_WASM,
                package_root / "component" / "igrib.wasm",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  fs::create_directories(package_root / "ui", error);
  CHECK(!error);
  fs::copy_file(PPM_TEST_IGRIB_UI,
                package_root / "ui" / "igrib-viewer.ui.json",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  CHECK(Write(package_root / "resources" / "igrib.svg", "<svg/>"));
  CHECK(Write(package_root / "resources" / "fault-test.svg", "<svg/>"));
  CHECK(Write(package_root / "resources" / "http-download.svg", "<svg/>"));
  CHECK(Write(
      package_root / "manifest.json",
      "{"
      "\"format_version\":1,"
      "\"id\":\"org.opencpn.igrib\","
      "\"name\":\"iGRIB\","
      "\"version\":\"0.1.0\","
      "\"component\":\"component/igrib.wasm\","
      "\"runtime\":\">=0.1.0 <0.2.0\","
      "\"portable_api\":\">=0.1.0 <0.2.0\","
      "\"surfaces\":{\"environment.viewer\":"
      "\"ui/igrib-viewer.ui.json\"},"
      "\"permissions\":["
      "\"ui.commands\",\"navigation.position.read\","
      "\"navigation.objects.read\",\"settings.read-write\","
      "\"overlay.submit\",\"jobs.compute\",\"environment.datasets\","
      "\"storage.user-selected\",\"network.providers\","
      "\"helpers.environment.decode\","
      "\"helpers.environment.generate\",\"charts.coverage\","
      "\"network.http\",\"storage.private\",\"credentials.provider\"],"
      "\"development\":true"
      "}"));
  CHECK(Write(test_root / "state" / (package_id + ".enabled"), "1\n"));

  std::set<std::string> actions;
  std::uint32_t next_action = 100;
  int state_changes = 0;
  int opened_surfaces = 0;
  bool opened_surface_valid = true;
  auto register_action = [&](const ppm::RuntimeAction& action,
                             std::uint32_t* host_action_id) {
    const std::string key = action.package_id + "/" + action.action_id;
    if (!actions.insert(key).second) return -5;
    *host_action_id = next_action++;
    return 0;
  };
  auto remove_actions = [&](const std::string& id) {
    for (auto item = actions.begin(); item != actions.end();) {
      if (item->rfind(id + "/", 0) == 0)
        item = actions.erase(item);
      else
        ++item;
    }
  };

  {
    ppm::RuntimeEngine engine(test_root.string(), register_action,
                              remove_actions,
                              [&]() { ++state_changes; });
    engine.SetSurfaceOpenedCallback(
        [&](const std::string& id,
            const ppm::DeclarativeSurface& surface) {
          if (id != package_id || surface.id != "environment.viewer")
            opened_surface_valid = false;
          ++opened_surfaces;
        });
    const bool loaded = engine.LoadInstalled(true);
    if (!loaded) {
      for (const auto& package : engine.Packages())
        std::cerr << package.id << " state=" << package.state
                  << " diagnostic=" << package.diagnostic << '\n';
    }
    CHECK(loaded);
    auto packages = engine.Packages();
    CHECK(packages.size() == 1);
    CHECK(Snapshot(packages, package_id));
    CHECK(Snapshot(packages, package_id)->state == "Unloaded");
    CHECK(Snapshot(packages, package_id)->surface_count == 1);
    CHECK(!engine.IsEnabled(package_id));
    CHECK(actions.empty());
    std::string diagnostic;
    const auto requested = engine.RequestedPermissions(package_id);
    CHECK(requested.size() == 15);
    CHECK(!engine.Enable(package_id, &diagnostic));
    CHECK(diagnostic == "permission approval is required");
    CHECK(actions.empty());
    diagnostic.clear();
    CHECK(!engine.SetGrantedPermissions(
        package_id, {requested.front()}, &diagnostic));
    CHECK(!diagnostic.empty());
    diagnostic.clear();
    CHECK(engine.SetGrantedPermissions(package_id, requested, &diagnostic));
    CHECK(engine.Enable(package_id, &diagnostic));
    CHECK(engine.IsEnabled(package_id));
    CHECK(actions.size() == 3);

    CHECK(engine.HandleAction(package_id, "igrib.toggle"));
    CHECK(engine.WaitForIdle(package_id, std::chrono::seconds(2)));
    CHECK(opened_surfaces == 1);
    CHECK(opened_surface_valid);
    CHECK(Snapshot(engine.Packages(), package_id)->state == "Failed");
    CHECK(engine.Enable(package_id, &diagnostic));
    CHECK(engine.IsEnabled(package_id));

    CHECK(engine.Disable(package_id, &diagnostic));
    CHECK(!engine.IsEnabled(package_id));
    const auto disabled = engine.Packages();
    CHECK(Snapshot(disabled, package_id)->state == "Disabled");
    CHECK(Snapshot(disabled, package_id)->enable_count == 2);
    CHECK(Snapshot(disabled, package_id)->disable_count == 1);
    CHECK(actions.empty());

    CHECK(engine.Enable(package_id, &diagnostic));
    CHECK(engine.IsEnabled(package_id));
    CHECK(actions.size() == 3);

    CHECK(engine.Unload(package_id, &diagnostic));
    CHECK(Snapshot(engine.Packages(), package_id)->state == "Unloaded");
    CHECK(actions.empty());

    CHECK(engine.Enable(package_id, &diagnostic));
    CHECK(engine.IsEnabled(package_id));
    CHECK(actions.size() == 3);

    CHECK(engine.HandleAction(package_id, "igrib.failure-test"));
    CHECK(engine.WaitForIdle(package_id, std::chrono::seconds(2)));
    CHECK(Snapshot(engine.Packages(), package_id)->state == "Failed");
    CHECK(!engine.IsEnabled(package_id));
    CHECK(actions.empty());

    diagnostic.clear();
    CHECK(engine.Enable(package_id, &diagnostic));
    CHECK(engine.IsEnabled(package_id));
    CHECK(actions.size() == 3);

    CHECK(engine.Unload(package_id, &diagnostic));
    CHECK(engine.RefreshPackage(package_id, true, &diagnostic));
    CHECK(Snapshot(engine.Packages(), package_id)->state == "Unloaded");
    CHECK(actions.empty());
    CHECK(engine.SetGrantedPermissions(
        package_id, engine.RequestedPermissions(package_id), &diagnostic));
    CHECK(engine.Enable(package_id, &diagnostic));
    CHECK(Snapshot(engine.Packages(), package_id)->state == "Enabled");
    CHECK(actions.size() == 3);
    engine.Shutdown();
    CHECK(actions.empty());
    CHECK(state_changes >= 8);
  }

  actions.clear();
  {
    ppm::RuntimeEngine blocked(test_root.string(), register_action,
                               remove_actions, []() {});
    CHECK(!blocked.LoadInstalled(false));
    const auto packages = blocked.Packages();
    CHECK(packages.size() == 1);
    CHECK(packages[0].state == "Failed");
    CHECK(!blocked.IsEnabled(package_id));
    CHECK(actions.empty());
  }

  fs::remove_all(test_root, error);
  return 0;
}
