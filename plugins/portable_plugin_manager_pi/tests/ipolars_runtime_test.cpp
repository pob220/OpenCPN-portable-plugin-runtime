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
  output << contents;
  return static_cast<bool>(output);
}

std::string Read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}
}  // namespace

extern "C" bool PlugIn_GSHHS_CrossesLand(double, double, double, double) {
  return false;
}

int main() {
  wxInitializer wx;
  CHECK(wx.IsOk());
  const fs::path root =
      fs::temp_directory_path() /
      ("ppm-ipolars-" + std::to_string(
                            std::chrono::high_resolution_clock::now()
                                .time_since_epoch()
                                .count()));
  const std::string id = "org.opencpn.ipolars";
  const fs::path package = root / "packages" / id;
  std::error_code error;
  fs::create_directories(package / "component", error);
  fs::copy_file(PPM_TEST_IPOLARS_WASM,
                package / "component" / "ipolars.wasm",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  fs::create_directories(package / "ui", error);
  fs::copy_file(PPM_TEST_IPOLARS_UI, package / "ui" / "ipolars.ui.json",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  CHECK(Write(package / "resources" / "ipolars.svg", "<svg/>"));
  CHECK(Write(
      package / "manifest.json",
      "{\"format_version\":1,\"id\":\"org.opencpn.ipolars\","
      "\"name\":\"iPolars\",\"version\":\"0.1.2\","
      "\"component\":\"component/ipolars.wasm\","
      "\"runtime\":\">=0.1.0 <0.2.0\","
      "\"portable_api\":\">=0.1.0 <0.2.0\","
      "\"surfaces\":{\"polars.editor\":\"ui/ipolars.ui.json\"},"
      "\"permissions\":[\"ui.commands\",\"settings.read-write\","
      "\"storage.user-selected\",\"navigation.nmea.read\"],"
      "\"development\":true}"));

  std::set<std::string> actions;
  std::string last_state;
  std::string last_error;
  int opened = 0;
  bool opened_valid = true;
  ppm::RuntimeEngine engine(
      root.string(),
      [&](const ppm::RuntimeAction& action, std::uint32_t* host_id) {
        actions.insert(action.action_id);
        *host_id = 1;
        return 0;
      },
      [&](const std::string&) { actions.clear(); }, []() {});
  engine.SetSurfaceOpenedCallback(
      [&](const std::string& package_id,
          const ppm::DeclarativeSurface& surface) {
        if (package_id != id || surface.id != "polars.editor")
          opened_valid = false;
        ++opened;
      });
  engine.SetSurfaceResponseCallback(
      [&](const std::string&, const std::string&, const std::string&,
          const std::string& state, const std::string& diagnostic) {
        last_state = state;
        last_error = diagnostic;
      });
  CHECK(engine.LoadInstalled(true));
  std::string diagnostic;
  CHECK(engine.SetGrantedPermissions(
      id, engine.RequestedPermissions(id), &diagnostic));
  CHECK(engine.Enable(id, &diagnostic));
  CHECK(actions.count("ipolars.open") == 1);
  CHECK(engine.HandleAction(id, "ipolars.open"));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(opened == 1);
  CHECK(opened_valid);

  const fs::path input = root / "source.pol";
  CHECK(Write(input,
              "Test boat\nTWA/TWS\t6\t12\n40\t3.1\t4.2\n90\t4.8\t6.5\n"
              "180\t3.8\t5.7\n"));
  std::string read_grant;
  CHECK(engine.RegisterUserFileGrant(id, input.string(), false, &read_grant,
                                     &diagnostic));
  CHECK(read_grant.rfind("ufg-", 0) == 0);
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "open",
                                  "\"" + read_grant + "\""));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_error.empty());
  CHECK(last_state.find("3 angles × 2 wind speeds") != std::string::npos);

  CHECK(engine.HandleSurfaceEvent(
      id, "polars.editor", "polar-grid",
      "{\"row\":1,\"column\":1,\"value\":\"5.25\"}"));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  const fs::path output = root / "saved.pol";
  std::string write_grant;
  CHECK(engine.RegisterUserFileGrant(id, output.string(), true, &write_grant,
                                     &diagnostic));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "save-as",
                                  "\"" + write_grant + "\""));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_error.empty());
  CHECK(Read(output).find("90\t5.25\t6.5") != std::string::npos);

  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "capture", "true"));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  engine.DeliverNavigationSentence(
      "$IIVWT,90.0,R,12.0,N,6.2,M,22.2,K*73");
  engine.DeliverNavigationSentence(
      "$IIVHW,0.0,T,0.0,M,5.50,N,10.2,K*56");
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "refresh", "null"));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_state.find("1 samples retained") != std::string::npos);
  CHECK(last_state.find("5.5") != std::string::npos);
  engine.DeliverNavigationSentence(
      "$IIVHW,0.0,T,0.0,M,99.00,N,10.2,K*00");
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "refresh", "null"));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_state.find("1 samples retained") != std::string::npos);

  const fs::path vdr = root / "recording.vdr";
  CHECK(Write(vdr,
              "2026-07-23T10:00:00Z $IIVWT,90.0,R,12.0,N,6.2,M,22.2,K*73\n"
              "2026-07-23T10:00:01Z $IIVHW,0.0,T,0.0,M,5.50,N,10.2,K*56\n"
              "corrupt line\n"));
  std::string vdr_grant;
  CHECK(engine.RegisterUserFileGrant(id, vdr.string(), false, &vdr_grant,
                                     &diagnostic));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "import-vdr",
                                  "\"" + vdr_grant + "\""));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_error.empty());
  CHECK(last_state.find("2 valid sentences, 1 rejected lines") !=
        std::string::npos);
  CHECK(last_state.find("1 complete samples retained") != std::string::npos);

  const fs::path observations = root / "observations.csv";
  CHECK(Write(observations,
              "TWS,TWA,STW,engineOn,manoeuvring,steady\n"
              "12,90,5.4,false,false,true\n"
              "12,90,5.8,false,false,true\n"
              "12,90,9.9,true,false,true\n"
              "12,90,9.9,false,true,true\n"
              "12,90,9.9,false,false,false\n"));
  std::string csv_grant;
  CHECK(engine.RegisterUserFileGrant(id, observations.string(), false,
                                     &csv_grant, &diagnostic));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "import-csv",
                                  "\"" + csv_grant + "\""));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_error.empty());
  CHECK(last_state.find("2 accepted, 3 rejected") != std::string::npos);
  CHECK(last_state.find("5.6") != std::string::npos);

  last_error.clear();
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "save-as",
                                  "\"" + write_grant + "\""));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(!last_error.empty());

  std::string revoked_grant;
  CHECK(engine.RegisterUserFileGrant(id, input.string(), false,
                                     &revoked_grant, &diagnostic));
  CHECK(engine.Disable(id, &diagnostic));
  CHECK(engine.Enable(id, &diagnostic));
  last_error.clear();
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "open",
                                  "\"" + revoked_grant + "\""));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(!last_error.empty());
  engine.Shutdown();
  fs::remove_all(root, error);
  return 0;
}
