#include "runtime_engine.h"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>

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

class UiLoop {
 public:
  UiLoop() : thread_([this]() { Run(); }) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this]() { return started_; });
  }
  ~UiLoop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    changed_.notify_all();
    thread_.join();
  }
  void Post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push(std::move(task));
      ++pending_;
    }
    changed_.notify_all();
  }
  bool WaitIdle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout,
                             [this]() { return pending_ == 0; });
  }
  std::thread::id ThreadId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return thread_id_;
  }

 private:
  void Run() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      thread_id_ = std::this_thread::get_id();
      started_ = true;
    }
    changed_.notify_all();
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
        if (stop_ && tasks_.empty()) return;
        task = std::move(tasks_.front());
        tasks_.pop();
      }
      task();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        --pending_;
      }
      changed_.notify_all();
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::queue<std::function<void()>> tasks_;
  std::thread thread_;
  std::thread::id thread_id_;
  std::size_t pending_ = 0;
  bool started_ = false;
  bool stop_ = false;
};

std::mutex land_thread_mutex;
std::thread::id land_thread;

}  // namespace

extern "C" bool PlugIn_GSHHS_CrossesLand(double, double, double, double) {
  std::lock_guard<std::mutex> lock(land_thread_mutex);
  land_thread = std::this_thread::get_id();
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
  std::atomic_int state_changes{0};
  std::atomic_int opened_surfaces{0};
  std::atomic_bool opened_surface_valid{true};
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
  UiLoop ui_loop;

  {
    ppm::RuntimeEngine engine(test_root.string(), register_action,
                              remove_actions,
                              [&]() { ++state_changes; },
                              [&](std::function<void()> task) {
                                ui_loop.Post(std::move(task));
                              });
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
    CHECK(ui_loop.WaitIdle(std::chrono::seconds(2)));
    CHECK(opened_surfaces == 1);
    CHECK(opened_surface_valid);
    CHECK(engine.IsEnabled(package_id));
    CHECK(Snapshot(engine.Packages(), package_id)->state == "Enabled");
    CHECK(engine.Scenes().size() == 1);
    {
      std::lock_guard<std::mutex> lock(land_thread_mutex);
      CHECK(land_thread == ui_loop.ThreadId());
    }

    CHECK(engine.Disable(package_id, &diagnostic));
    CHECK(!engine.IsEnabled(package_id));
    const auto disabled = engine.Packages();
    CHECK(Snapshot(disabled, package_id)->state == "Disabled");
    CHECK(Snapshot(disabled, package_id)->enable_count == 1);
    CHECK(Snapshot(disabled, package_id)->disable_count == 1);
    CHECK(actions.empty());

    CHECK(engine.Enable(package_id, &diagnostic));
    CHECK(engine.IsEnabled(package_id));
    CHECK(actions.size() == 3);

    CHECK(engine.HandleAction(package_id, "igrib.toggle"));
    bool running_job_seen = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
      if (Snapshot(engine.Packages(), package_id)->job_count != 0) {
        running_job_seen = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(running_job_seen);
    CHECK(engine.Disable(package_id, &diagnostic));
    CHECK(Snapshot(engine.Packages(), package_id)->job_count == 0);
    CHECK(engine.Enable(package_id, &diagnostic));
    CHECK(engine.IsEnabled(package_id));

    CHECK(engine.Unload(package_id, &diagnostic));
    CHECK(Snapshot(engine.Packages(), package_id)->state == "Unloaded");
    CHECK(actions.empty());

    CHECK(engine.Enable(package_id, &diagnostic));
    CHECK(engine.IsEnabled(package_id));
    CHECK(actions.size() == 3);

    CHECK(engine.HandleAction(package_id, "igrib.failure-test"));
    CHECK(engine.WaitForIdle(package_id, std::chrono::seconds(2)));
    CHECK(ui_loop.WaitIdle(std::chrono::seconds(2)));
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

  const fs::path dependency_root =
      fs::temp_directory_path() /
      fs::path("ppm runtime services " + std::to_string(stamp));
  const fs::path provider_root =
      dependency_root / "packages" / "org.opencpn.igrib";
  const fs::path consumer_root =
      dependency_root / "packages" / "org.opencpn.iweather-routing";
  fs::create_directories(provider_root / "component", error);
  CHECK(!error);
  fs::create_directories(provider_root / "ui", error);
  CHECK(!error);
  fs::copy_file(PPM_TEST_IGRIB_WASM,
                provider_root / "component" / "igrib.wasm",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  fs::copy_file(PPM_TEST_IGRIB_UI,
                provider_root / "ui" / "igrib-viewer.ui.json",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  CHECK(Write(provider_root / "resources" / "igrib.svg", "<svg/>"));
  CHECK(Write(provider_root / "resources" / "fault-test.svg", "<svg/>"));
  CHECK(Write(provider_root / "resources" / "http-download.svg", "<svg/>"));
  CHECK(Write(
      provider_root / "manifest.json",
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
      "\"provides\":[{\"interface\":"
      "\"org.opencpn.environment.provider\",\"version\":\"0.1.0\"}],"
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
  fs::create_directories(consumer_root / "component", error);
  CHECK(!error);
  fs::create_directories(consumer_root / "ui", error);
  CHECK(!error);
  fs::copy_file(PPM_TEST_IWEATHER_WASM,
                consumer_root / "component" / "iweather-routing.wasm",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  fs::copy_file(PPM_TEST_IWEATHER_UI,
                consumer_root / "ui" / "iweather-routing.ui.json",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  CHECK(Write(consumer_root / "resources" / "iweather-routing.svg",
              "<svg/>"));
  CHECK(Write(
      consumer_root / "manifest.json",
      "{"
      "\"format_version\":1,"
      "\"id\":\"org.opencpn.iweather-routing\","
      "\"name\":\"iWeatherRouting\","
      "\"version\":\"0.1.0\","
      "\"component\":\"component/iweather-routing.wasm\","
      "\"runtime\":\">=0.1.0 <0.2.0\","
      "\"portable_api\":\">=0.1.0 <0.2.0\","
      "\"surfaces\":{\"routing.workbench\":"
      "\"ui/iweather-routing.ui.json\"},"
      "\"requires\":[{\"interface\":"
      "\"org.opencpn.environment.provider\","
      "\"range\":\">=0.1.0 <0.2.0\"}],"
      "\"permissions\":["
      "\"ui.commands\",\"navigation.position.read\","
      "\"navigation.objects.read\",\"weather-routing.compute\","
      "\"environment.consume\",\"charts.coverage\","
      "\"storage.user-selected\",\"navigation.routes.write\"],"
      "\"development\":true"
      "}"));
  actions.clear();
  {
    ppm::RuntimeEngine services(dependency_root.string(), register_action,
                                remove_actions, []() {});
    CHECK(services.LoadInstalled(true));
    const auto service_packages = services.Packages();
    CHECK(service_packages.size() == 2);
    const auto* provider =
        Snapshot(service_packages, "org.opencpn.igrib");
    const auto* consumer =
        Snapshot(service_packages, "org.opencpn.iweather-routing");
    CHECK(provider && provider->provided_service_count == 1);
    CHECK(consumer && consumer->required_service_count == 1);
    std::string diagnostic;
    CHECK(services.SetGrantedPermissions(
        "org.opencpn.iweather-routing",
        services.RequestedPermissions("org.opencpn.iweather-routing"),
        &diagnostic));
    CHECK(!services.Enable("org.opencpn.iweather-routing", &diagnostic));
    CHECK(diagnostic.find("no enabled compatible provider") !=
          std::string::npos);
    CHECK(services.SetGrantedPermissions(
        "org.opencpn.igrib",
        services.RequestedPermissions("org.opencpn.igrib"), &diagnostic));
    CHECK(services.Enable("org.opencpn.igrib", &diagnostic));
    CHECK(services.Enable("org.opencpn.iweather-routing", &diagnostic));
    CHECK(!services.Disable("org.opencpn.igrib", &diagnostic));
    CHECK(diagnostic.find("disable the dependent package first") !=
          std::string::npos);
    CHECK(services.Disable("org.opencpn.iweather-routing", &diagnostic));
    CHECK(services.Disable("org.opencpn.igrib", &diagnostic));
    services.Shutdown();
  }

  fs::remove_all(test_root, error);
  fs::remove_all(dependency_root, error);
  return 0;
}
