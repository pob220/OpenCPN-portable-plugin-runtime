#include "runtime_engine.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

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

}  // namespace

extern "C" bool PlugIn_GSHHS_CrossesLand(double, double, double, double) {
  return false;
}

int main() {
  wxInitializer wx;
  CHECK(wx.IsOk());
  const auto stamp =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const fs::path root =
      fs::temp_directory_path() /
      fs::path("ppm capability runtime " + std::to_string(stamp));
  const std::string package_id = "org.opencpn.capability-lab";
  const fs::path package = root / "packages" / package_id;
  std::error_code error;
  fs::create_directories(package / "component", error);
  CHECK(!error);
  fs::copy_file(PPM_TEST_CAPABILITY_WASM,
                package / "component" / "capability-lab.wasm",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  CHECK(Write(package / "manifest.json",
              "{"
              "\"format_version\":1,"
              "\"id\":\"org.opencpn.capability-lab\","
              "\"name\":\"Portable Capability Lab\","
              "\"version\":\"0.1.0\","
              "\"component\":\"component/capability-lab.wasm\","
              "\"runtime\":\">=0.1.0 <0.2.0\","
              "\"portable_api\":\">=0.1.0 <0.2.0\","
              "\"event_subscriptions\":["
              "{\"event\":\"navigation.nmea0183\","
              "\"topic_prefix\":\"$GPGGA\",\"queue_limit\":8},"
              "{\"event\":\"opencpn.plugin-message\","
              "\"topic_prefix\":\"CAPABILITY_IN\",\"queue_limit\":8}],"
              "\"permissions\":[\"ui.commands\",\"navigation.nmea.read\","
              "\"plugin.messages.receive\",\"plugin.messages.send\"],"
              "\"development\":true"
              "}"));

  std::set<std::string> actions;
  std::mutex message_mutex;
  std::vector<std::pair<std::string, std::string>> messages;
  ppm::RuntimeEngine engine(
      root.string(),
      [&](const ppm::RuntimeAction& action, std::uint32_t* host_id) {
        actions.insert(action.action_id);
        *host_id = static_cast<std::uint32_t>(actions.size());
        return 0;
      },
      [&](const std::string&) { actions.clear(); }, []() {});
  engine.SetPluginMessageSender(
      [&](const std::string& id, const std::string& body) {
        std::lock_guard<std::mutex> lock(message_mutex);
        messages.emplace_back(id, body);
      });

  CHECK(engine.LoadInstalled(true));
  const auto requested = engine.RequestedPermissions(package_id);
  CHECK(requested.size() == 4);
  std::string diagnostic;
  CHECK(engine.SetGrantedPermissions(package_id, requested, &diagnostic));
  CHECK(engine.Enable(package_id, &diagnostic));
  CHECK(actions.size() == 3);

  CHECK(engine.HandleAction(package_id, "capability-lab.probe"));
  CHECK(engine.WaitForIdle(package_id, std::chrono::seconds(2)));
  {
    std::lock_guard<std::mutex> lock(message_mutex);
    CHECK(messages.size() == 1);
    CHECK(messages.back().first == "CAPABILITY_OUT");
  }

  engine.DeliverPluginMessage("IGNORED", "not delivered");
  engine.DeliverPluginMessage("CAPABILITY_IN_TEST", "native-to-portable");
  CHECK(engine.WaitForIdle(package_id, std::chrono::seconds(2)));
  CHECK(engine.HandleAction(package_id, "capability-lab.echo-message"));
  CHECK(engine.WaitForIdle(package_id, std::chrono::seconds(2)));
  {
    std::lock_guard<std::mutex> lock(message_mutex);
    CHECK(messages.back() == std::make_pair(std::string("CAPABILITY_ECHO"),
                                            std::string("native-to-portable")));
  }

  engine.DeliverNavigationSentence("$GPRMC,ignored*00");
  CHECK(engine.WaitForIdle(package_id, std::chrono::seconds(2)));
  CHECK(engine.HandleAction(package_id, "capability-lab.echo-nmea"));
  CHECK(engine.WaitForIdle(package_id, std::chrono::seconds(2)));
  {
    std::lock_guard<std::mutex> lock(message_mutex);
    CHECK(messages.back() ==
          std::make_pair(std::string("CAPABILITY_NMEA"), std::string()));
  }

  engine.DeliverNavigationSentence("$GPGGA,accepted*00");
  CHECK(engine.WaitForIdle(package_id, std::chrono::seconds(2)));
  CHECK(engine.HandleAction(package_id, "capability-lab.echo-nmea"));
  CHECK(engine.WaitForIdle(package_id, std::chrono::seconds(2)));
  {
    std::lock_guard<std::mutex> lock(message_mutex);
    CHECK(messages.back() == std::make_pair(std::string("CAPABILITY_NMEA"),
                                            std::string("$GPGGA,accepted*00")));
  }

  CHECK(engine.Disable(package_id, &diagnostic));
  CHECK(actions.empty());

  const std::string invalid_id = "org.opencpn.invalid-events";
  const fs::path invalid_package = root / "packages" / invalid_id;
  fs::create_directories(invalid_package / "component", error);
  CHECK(!error);
  fs::copy_file(PPM_TEST_CAPABILITY_WASM,
                invalid_package / "component" / "capability-lab.wasm",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  CHECK(Write(invalid_package / "manifest.json",
              "{"
              "\"format_version\":1,"
              "\"id\":\"org.opencpn.invalid-events\","
              "\"name\":\"Invalid event probe\","
              "\"version\":\"0.1.0\","
              "\"component\":\"component/capability-lab.wasm\","
              "\"runtime\":\">=0.1.0 <0.2.0\","
              "\"portable_api\":\">=0.1.0 <0.2.0\","
              "\"event_subscriptions\":[{\"event\":\"opencpn.plugin-message\","
              "\"topic_prefix\":\"CAPABILITY_IN\"}],"
              "\"permissions\":[\"ui.commands\"],"
              "\"development\":true"
              "}"));
  diagnostic.clear();
  CHECK(!engine.RefreshPackage(invalid_id, true, &diagnostic));
  CHECK(diagnostic.find("lacks its corresponding permission") !=
        std::string::npos);

  fs::remove_all(root, error);
  std::cout << "portable capability runtime test passed\n";
  return 0;
}
