#include "runtime_engine.h"

#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>

#include <wx/init.h>

#define CHECK(expression)                                            \
  do {                                                               \
    if (!(expression)) {                                             \
      std::cerr << "check failed at " << __FILE__ << ':' << __LINE__ \
                << ": " #expression "\n";                            \
      return 1;                                                      \
    }                                                                \
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
    return changed_.wait_for(lock, timeout, [this]() { return pending_ == 0; });
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
  fs::copy_file(PPM_TEST_IGRIB_WASM, package_root / "component" / "igrib.wasm",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  fs::create_directories(package_root / "ui", error);
  CHECK(!error);
  fs::copy_file(PPM_TEST_IGRIB_UI, package_root / "ui" / "igrib-viewer.ui.json",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  CHECK(Write(package_root / "resources" / "igrib.svg", "<svg/>"));
  CHECK(Write(package_root / "resources" / "fault-test.svg", "<svg/>"));
  CHECK(Write(package_root / "resources" / "http-download.svg", "<svg/>"));
  CHECK(Write(package_root / "manifest.json",
              "{"
              "\"format_version\":1,"
              "\"id\":\"org.opencpn.igrib\","
              "\"name\":\"iGRIB\","
              "\"version\":\"0.1.0\","
              "\"component\":\"component/igrib.wasm\","
              "\"runtime\":\">=0.1.0 <0.2.0\","
              "\"portable_api\":\">=0.2.0 <0.3.0\","
              "\"portable_world\":\"plugin\","
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
    ppm::RuntimeEngine engine(
        test_root.string(), register_action, remove_actions,
        [&]() { ++state_changes; },
        [&](std::function<void()> task) { ui_loop.Post(std::move(task)); });
    engine.SetSurfaceOpenedCallback(
        [&](const std::string& id, const ppm::DeclarativeSurface& surface) {
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
    CHECK(!engine.SetGrantedPermissions(package_id, {requested.front()},
                                        &diagnostic));
    CHECK(!diagnostic.empty());
    diagnostic.clear();
    CHECK(engine.SetGrantedPermissions(package_id, requested, &diagnostic));
    CHECK(engine.Enable(package_id, &diagnostic));
    CHECK(engine.IsEnabled(package_id));
    CHECK(actions.size() == 1);

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
    CHECK(actions.size() == 1);

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
    CHECK(actions.size() == 1);

    CHECK(engine.HandleAction(package_id, "igrib.failure-test"));
    CHECK(engine.WaitForIdle(package_id, std::chrono::seconds(2)));
    CHECK(ui_loop.WaitIdle(std::chrono::seconds(2)));
    CHECK(Snapshot(engine.Packages(), package_id)->state == "Failed");
    CHECK(!engine.IsEnabled(package_id));
    CHECK(actions.empty());

    diagnostic.clear();
    CHECK(engine.Enable(package_id, &diagnostic));
    CHECK(engine.IsEnabled(package_id));
    CHECK(actions.size() == 1);

    CHECK(engine.Unload(package_id, &diagnostic));
    CHECK(engine.RefreshPackage(package_id, true, &diagnostic));
    CHECK(Snapshot(engine.Packages(), package_id)->state == "Unloaded");
    CHECK(actions.empty());
    CHECK(engine.SetGrantedPermissions(
        package_id, engine.RequestedPermissions(package_id), &diagnostic));
    CHECK(engine.Enable(package_id, &diagnostic));
    CHECK(Snapshot(engine.Packages(), package_id)->state == "Enabled");
    CHECK(actions.size() == 1);
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
  fs::copy_file(PPM_TEST_IGRIB_WASM, provider_root / "component" / "igrib.wasm",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  fs::copy_file(PPM_TEST_IGRIB_UI,
                provider_root / "ui" / "igrib-viewer.ui.json",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  CHECK(Write(provider_root / "resources" / "igrib.svg", "<svg/>"));
  CHECK(Write(provider_root / "resources" / "fault-test.svg", "<svg/>"));
  CHECK(Write(provider_root / "resources" / "http-download.svg", "<svg/>"));
  CHECK(Write(provider_root / "manifest.json",
              "{"
              "\"format_version\":1,"
              "\"id\":\"org.opencpn.igrib\","
              "\"name\":\"iGRIB\","
              "\"version\":\"0.1.0\","
              "\"component\":\"component/igrib.wasm\","
              "\"runtime\":\">=0.1.0 <0.2.0\","
              "\"portable_api\":\">=0.2.0 <0.3.0\","
              "\"portable_world\":\"plugin\","
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
  CHECK(Write(consumer_root / "resources" / "iweather-routing.svg", "<svg/>"));
  CHECK(Write(consumer_root / "manifest.json",
              "{"
              "\"format_version\":1,"
              "\"id\":\"org.opencpn.iweather-routing\","
              "\"name\":\"iWeatherRouting\","
              "\"version\":\"0.1.0\","
              "\"component\":\"component/iweather-routing.wasm\","
              "\"runtime\":\">=0.1.0 <0.2.0\","
              "\"portable_api\":\">=0.2.0 <0.3.0\","
              "\"portable_world\":\"passage-weather-routing-plugin\","
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
    const auto* provider = Snapshot(service_packages, "org.opencpn.igrib");
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
        "org.opencpn.igrib", services.RequestedPermissions("org.opencpn.igrib"),
        &diagnostic));
    CHECK(services.Enable("org.opencpn.igrib", &diagnostic));
    CHECK(services.Enable("org.opencpn.iweather-routing", &diagnostic));
    CHECK(!services.Disable("org.opencpn.igrib", &diagnostic));
    CHECK(diagnostic.find("disable the dependent package first") !=
          std::string::npos);

    std::atomic_bool route_completed{false};
    std::atomic_bool route_succeeded{true};
    std::atomic_bool route_id_valid{true};
    std::mutex route_diagnostic_mutex;
    std::string route_diagnostic;
    services.SetRoutingCompletedCallback(
        [&](const std::string& id, bool succeeded, ppm::RoutingOutcome,
            const std::string& failure) {
          route_id_valid = id == "org.opencpn.iweather-routing";
          route_succeeded = succeeded;
          {
            std::lock_guard<std::mutex> lock(route_diagnostic_mutex);
            route_diagnostic = failure;
          }
          route_completed = true;
        });
    ppm::RoutingRequest request;
    request.parameters.start_latitude = 50.0;
    request.parameters.start_longitude = -4.0;
    request.parameters.destination_latitude = 50.05;
    request.parameters.destination_longitude = -3.95;
    request.parameters.departure_unix_time = 1780000000;
    request.parameters.time_step_seconds = 3600;
    request.parameters.heading_step_degrees = 15;
    request.parameters.refined_heading_step_degrees = 5;
    request.parameters.adaptive_headings = 1;
    request.parameters.spatial_cell_nautical_miles = 3.0;
    request.parameters.labels_per_cell = 2;
    request.parameters.max_hours = 24;
    request.parameters.max_states = 10'000;
    request.parameters.min_true_wind_angle_degrees = 40.0;
    request.parameters.max_true_wind_angle_degrees = 160.0;
    request.parameters.maximum_latitude_degrees = 89.0;
    request.parameters.upwind_efficiency = 1.0;
    request.parameters.downwind_efficiency = 1.0;
    request.parameters.maximum_search_angle_degrees = 120.0;
    request.parameters.destination_tolerance_nm = 1.0;
    ppm::RoutingPolar polar;
    polar.identity = "Runtime service test";
    polar.true_wind_speeds_knots = {0.0, 10.0, 20.0, 40.0};
    polar.true_wind_angles_degrees = {0.0, 40.0, 90.0, 160.0, 180.0};
    polar.boat_speeds_knots = {0.0,  0.0,  0.0,  0.0,  0.0,  0.0,  3.86,
                               5.47, 3.92, 3.76, 0.0,  3.98, 6.18, 5.30,
                               4.90, 0.0,  2.33, 3.99, 3.60, 3.33};
    request.polars.push_back(std::move(polar));
    CHECK(!services.CancelRoute("org.opencpn.iweather-routing"));
    diagnostic.clear();
    CHECK(services.BeginRouteAttempt("org.opencpn.iweather-routing",
                                     &diagnostic));
    CHECK(diagnostic.empty());
    CHECK(services.StartRoute("org.opencpn.iweather-routing",
                              std::move(request), &diagnostic));
    CHECK(services.WaitForRoute("org.opencpn.iweather-routing",
                                std::chrono::seconds(5)));
    CHECK(route_completed);
    CHECK(route_id_valid);
    CHECK(!route_succeeded);
    {
      std::lock_guard<std::mutex> lock(route_diagnostic_mutex);
      CHECK(!route_diagnostic.empty());
    }

    std::vector<std::uint8_t> availability;
    diagnostic.clear();
    CHECK(!services.PreflightEnvironment("org.opencpn.iweather-routing", 50.0,
                                         -4.0, {1780000000}, &availability,
                                         &diagnostic));
    CHECK(!diagnostic.empty());

    auto blocking_request = request;
    blocking_request.polars.push_back(
        {"Runtime blocking test",
         {0.0, 10.0, 20.0, 40.0},
         {0.0, 40.0, 90.0, 160.0, 180.0},
         {0.0, 0.0,  0.0,  0.0,  0.0,  0.0, 3.86, 5.47, 3.92, 3.76,
          0.0, 3.98, 6.18, 5.30, 4.90, 0.0, 2.33, 3.99, 3.60, 3.33}});
    auto invalid_blocking_request = blocking_request;
    invalid_blocking_request.polars.front().boat_speeds_knots.front() =
        std::numeric_limits<double>::quiet_NaN();
    ppm::RoutingOutcome invalid_outcome;
    diagnostic.clear();
    CHECK(!services.CalculateRouteBlocking("org.opencpn.iweather-routing",
                                           std::move(invalid_blocking_request),
                                           &invalid_outcome, &diagnostic));
    CHECK(diagnostic == "invalid or unbounded polar grid");

    ppm::RoutingPassageRequest invalid_passage;
    invalid_passage.route = blocking_request;
    invalid_passage.gates = {{"only", "Only gate", 50.0, -4.0}};
    diagnostic.clear();
    CHECK(!services.CalculatePassageBlocking("org.opencpn.iweather-routing",
                                             std::move(invalid_passage),
                                             &invalid_outcome, &diagnostic));
    CHECK(diagnostic == "passage routing requires 2-64 gates");

    ppm::RoutingPassageRequest passage;
    passage.route = blocking_request;
    passage.gates = {{"start", "Start", 50.0, -4.0},
                     {"gate", "Intermediate gate", 50.025, -3.975},
                     {"finish", "Finish", 50.05, -3.95}};
    diagnostic.clear();
    CHECK(!services.CalculatePassageBlocking("org.opencpn.iweather-routing",
                                             std::move(passage),
                                             &invalid_outcome, &diagnostic));
    CHECK(!diagnostic.empty());

    std::array<bool, 2> blocking_results{true, true};
    std::array<std::string, 2> blocking_diagnostics;
    std::array<std::thread, 2> blocking_workers;
    for (std::size_t index = 0; index < blocking_workers.size(); ++index) {
      blocking_workers[index] = std::thread([&, index]() {
        ppm::RoutingOutcome outcome;
        blocking_results[index] = services.CalculateRouteBlocking(
            "org.opencpn.iweather-routing", blocking_request, &outcome,
            &blocking_diagnostics[index]);
      });
    }
    for (auto& worker : blocking_workers) worker.join();
    CHECK(!blocking_results[0]);
    CHECK(!blocking_results[1]);
    CHECK(!blocking_diagnostics[0].empty());
    CHECK(!blocking_diagnostics[1].empty());
    CHECK(services.WaitForRoute("org.opencpn.iweather-routing",
                                std::chrono::milliseconds(0)));

    CHECK(services.Disable("org.opencpn.iweather-routing", &diagnostic));
    CHECK(services.Disable("org.opencpn.igrib", &diagnostic));
    services.Shutdown();
  }

  const std::string author_package_id = "org.opencpn.portable-template";
  const fs::path author_root =
      fs::temp_directory_path() /
      fs::path("ppm api v03 author " + std::to_string(stamp));
  const fs::path author_package_root =
      author_root / "packages" / author_package_id;
  fs::create_directories(author_package_root / "component", error);
  CHECK(!error);
  fs::copy_file(
      PPM_TEST_API_V03_WASM,
      author_package_root / "component" / "portable-plugin-template.wasm",
      fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  fs::create_directories(author_package_root / "ui", error);
  CHECK(!error);
  fs::copy_file(PPM_TEST_API_V03_UI,
                author_package_root / "ui" / "template.ui.json",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  CHECK(Write(author_package_root / "manifest.json",
              "{"
              "\"format_version\":1,"
              "\"id\":\"org.opencpn.portable-template\","
              "\"name\":\"Portable Plugin Template\","
              "\"version\":\"0.1.0\","
              "\"component\":\"component/portable-plugin-template.wasm\","
              "\"runtime\":\">=0.1.0 <0.2.0\","
              "\"portable_api\":\">=0.3.0 <0.4.0\","
              "\"portable_world\":\"plugin\","
              "\"surfaces\":{\"template.main\":\"ui/template.ui.json\"},"
              "\"permissions\":[\"ui.commands\",\"settings.read-write\","
              "\"storage.private\",\"overlay.submit\",\"timers.schedule\","
              "\"plugin.rpc.provide\"],"
              "\"development\":true"
              "}"));
  actions.clear();
  std::atomic_int author_surfaces{0};
  std::atomic_int author_responses{0};
  {
    ppm::RuntimeEngine author(
        author_root.string(), register_action, remove_actions, []() {},
        [&](std::function<void()> task) { ui_loop.Post(std::move(task)); });
    author.SetSurfaceOpenedCallback(
        [&](const std::string& id, const ppm::DeclarativeSurface& surface) {
          if (id == author_package_id && surface.id == "template.main")
            ++author_surfaces;
        });
    author.SetSurfaceResponseCallback(
        [&](const std::string& id, const std::string& surface,
            const std::string& control, const std::string& state,
            const std::string&) {
          if (id == author_package_id && surface == "template.main" &&
              control == "hello" &&
              state.find("API 0.3 surface callback is working") !=
                  std::string::npos)
            ++author_responses;
        });
    author.SetAuthorUiRequestCallback(
        [&](const std::string& id, const std::string& operation,
            const std::string&, std::string* response) {
          if (id != author_package_id || (operation != "actions.set-state" &&
                                          operation != "actions.unregister"))
            return -1;
          *response = "{}";
          return 0;
        });
    CHECK(author.LoadInstalled(true));
    std::string diagnostic;
    CHECK(author.SetGrantedPermissions(
        author_package_id, author.RequestedPermissions(author_package_id),
        &diagnostic));
    CHECK(author.Enable(author_package_id, &diagnostic));
    CHECK(author.IsEnabled(author_package_id));
    CHECK(actions.size() == 1);
    const auto scenes = author.Scenes();
    CHECK(scenes.size() == 1);
    CHECK(scenes[0].layers.size() == 1);
    CHECK(scenes[0].layers[0].primitives.size() == 1);
    CHECK(scenes[0].layers[0].primitives[0].interactive);
    CHECK(author.HandleAction(author_package_id, "template.hello"));
    CHECK(author.WaitForIdle(author_package_id, std::chrono::seconds(2)));
    CHECK(ui_loop.WaitIdle(std::chrono::seconds(2)));
    CHECK(author_surfaces == 1);
    CHECK(author.HandleSurfaceEvent(author_package_id, "template.main", "hello",
                                    "{}"));
    CHECK(author.WaitForIdle(author_package_id, std::chrono::seconds(2)));
    CHECK(ui_loop.WaitIdle(std::chrono::seconds(2)));
    CHECK(author_responses == 1);
    CHECK(author.Disable(author_package_id, &diagnostic));
    CHECK(!author.IsEnabled(author_package_id));
    CHECK(author.Scenes().empty());
    CHECK(actions.empty());
    author.Shutdown();
  }

  fs::remove_all(test_root, error);
  fs::remove_all(dependency_root, error);
  fs::remove_all(author_root, error);
  return 0;
}
