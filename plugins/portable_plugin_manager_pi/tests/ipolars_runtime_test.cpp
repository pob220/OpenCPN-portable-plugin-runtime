#include "runtime_engine.h"
#include "portable_polar.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

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

std::string JoinTabs(const std::vector<std::string>& fields) {
  std::string result;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) result += '\t';
    result += fields[index];
  }
  return result;
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
  fs::copy_file(PPM_TEST_IPOLARS_MANIFEST, package / "manifest.json",
                fs::copy_options::overwrite_existing, error);
  CHECK(!error);
  const std::string manifest = Read(package / "manifest.json");
  CHECK(manifest.find(R"("portable_api": ">=0.4.0 <0.5.0")") !=
        std::string::npos);
  CHECK(manifest.find(R"("event": "navigation.nmea0183")") !=
        std::string::npos);

  std::set<std::string> actions;
  bool action_valid = true;
  std::string last_state;
  std::string last_error;
  int opened = 0;
  bool opened_valid = true;
  ppm::RuntimeEngine engine(
      root.string(),
      [&](const ppm::RuntimeAction& action, std::uint32_t* host_id) {
        if (!action.toolbar || action.context_menu ||
            action.locations != std::vector<std::string>{"toolbar"})
          action_valid = false;
        actions.insert(action.action_id);
        *host_id = 1;
        return 0;
      },
      [&](const std::string&) { actions.clear(); }, []() {});
  engine.SetSurfaceOpenedCallback(
      [&](const std::string& package_id,
          const ppm::DeclarativeSurface& surface) {
        if (package_id != id || surface.id != "polars.editor" ||
            surface.role != "tool-window")
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
  CHECK(action_valid);
  CHECK(engine.HandleAction(id, "ipolars.open"));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(opened == 1);
  CHECK(opened_valid);

  const fs::path polar_pi_matrix = root / "polar-pi-matrix.pol";
  CHECK(Write(
      polar_pi_matrix,
      "TWA\\TWS;0;6;12;60\n"
      "0;0;0;0;0\n40;0;3.1;4.2;0\n90;0;4.8;6.5;0\n"
      "180;0;3.8;5.7;0\n"));
  std::string polar_pi_grant;
  CHECK(engine.RegisterUserFileGrant(id, polar_pi_matrix.string(), false,
                                     &polar_pi_grant, &diagnostic));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "open",
                                  "\"" + polar_pi_grant + "\""));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_error.empty());
  CHECK(last_state.find("4 angles × 2 wind speeds") != std::string::npos);

  const fs::path expedition = root / "expedition.pol";
  CHECK(Write(expedition,
              "6 40 3.1 90 4.8 180 3.8\n"
              "12 40 4.2 90 6.5 180 5.7\n"));
  std::string expedition_grant;
  CHECK(engine.RegisterUserFileGrant(id, expedition.string(), false,
                                     &expedition_grant, &diagnostic));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "open",
                                  "\"" + expedition_grant + "\""));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_error.empty());
  CHECK(last_state.find("3 angles × 2 wind speeds") != std::string::npos);

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
  CHECK(last_state.find("\"polar-plot\":{\"columns\":[\"TWA / TWS\"") !=
        std::string::npos);

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
  PortablePolarSet iweather_polar;
  CHECK(LoadPortablePolarSet(output, &iweather_polar, &diagnostic));
  CHECK(iweather_polar.grids.size() == 1);
  CHECK(iweather_polar.grids[0].true_wind_speeds_knots.size() == 2);

  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "new-polar", "null"));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor",
                                  "measurement-wind-speed", "\"12\""));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor",
                                  "measurement-wind-angle", "\"90\""));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "measurement-stw",
                                  "\"5.7\""));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "add-measurement",
                                  "null"));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_error.empty());
  CHECK(last_state.find("Added manual observation 1") != std::string::npos);
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor",
                                  "measurement-apparent", "true"));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor",
                                  "measurement-wind-speed", "\"14\""));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor",
                                  "measurement-wind-angle", "\"70\""));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "measurement-stw",
                                  "\"6.1\""));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "add-measurement",
                                  "null"));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_error.empty());
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor",
                                  "generate-from-measurements", "null"));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_error.empty());
  CHECK(last_state.find("Generated a measured polar from 2 manual") !=
        std::string::npos);

  const fs::path second_polar = root / "second.pol";
  CHECK(Write(second_polar,
              "Second sail\nTWA/TWS\t6\t12\n40\t3.0\t4.0\n90\t4.5\t6.0\n"
              "180\t3.5\t5.4\n"));
  const fs::path boat = root / "boat.xml";
  CHECK(Write(
      boat,
      "<?xml version=\"1.0\"?>\n"
      "<OpenCPNWeatherRoutingBoat version=\"1.10\" Name=\"Test boat\">\n"
      "  <Polar FileName=\"source.pol\" CrossOverPercentage=\"0\"/>\n"
      "</OpenCPNWeatherRoutingBoat>\n"));
  std::string boat_grant;
  CHECK(engine.RegisterUserFileGrant(id, boat.string(), false, &boat_grant,
                                     &diagnostic));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "open",
                                  "\"" + boat_grant + "\""));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_error.empty());
  CHECK(last_state.find("OpenCPN Weather Routing boat XML") !=
        std::string::npos);
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "boat-grid",
                                  "{\"row\":0,\"column\":0}"));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "boat-name",
                                  "\"Cruising test boat\""));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "new-polar-file",
                                  "\"second.pol\""));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "new-crossover",
                                  "\"12.5\""));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor",
                                  "add-polar-reference", "null"));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_error.empty());
  CHECK(last_state.find("second.pol") != std::string::npos);
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "move-polar-up",
                                  "null"));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_error.empty());
  const fs::path saved_boat = root / "saved-boat.xml";
  std::string saved_boat_grant;
  CHECK(engine.RegisterUserFileGrant(id, saved_boat.string(), true,
                                     &saved_boat_grant, &diagnostic));
  CHECK(engine.HandleSurfaceEvent(id, "polars.editor", "save-as",
                                  "\"" + saved_boat_grant + "\""));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_error.empty());
  CHECK(Read(saved_boat).find("Name=\"Cruising test boat\"") !=
        std::string::npos);
  PortablePolarSet iweather_boat;
  CHECK(LoadPortablePolarSet(saved_boat, &iweather_boat, &diagnostic));
  CHECK(iweather_boat.grids.size() == 2);

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

  std::vector<std::string> logbook_fields(41);
  logbook_fields[7] = "S";
  logbook_fields[15] = "5.8 kn";
  logbook_fields[19] = "90 T";
  logbook_fields[20] = "12 kn";
  const fs::path logbook = root / "logbook-konni.tsv";
  CHECK(Write(logbook, JoinTabs(logbook_fields) + "\n"));
  std::string logbook_grant;
  CHECK(engine.RegisterUserFileGrant(id, logbook.string(), false,
                                     &logbook_grant, &diagnostic));
  std::string multi_vdr_grant;
  CHECK(engine.RegisterUserFileGrant(id, vdr.string(), false,
                                     &multi_vdr_grant, &diagnostic));
  CHECK(engine.HandleSurfaceEvent(
      id, "polars.editor", "import-logbooks",
      "[\"" + multi_vdr_grant + "\",\"" + logbook_grant + "\"]"));
  CHECK(engine.WaitForIdle(id, std::chrono::seconds(2)));
  CHECK(last_error.empty());
  CHECK(last_state.find(
            "Imported 2 logbook file(s) (NMEA/VDR, LogbookKonni)") !=
        std::string::npos);
  CHECK(last_state.find(
            "2 complete STW/wind observations accepted, 2 rows or sentences "
            "rejected") != std::string::npos);
  CHECK(last_state.find("\"sample-count\":\"2\"") != std::string::npos);

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
