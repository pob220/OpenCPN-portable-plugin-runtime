#include "declarative_ui.h"

#include <algorithm>
#include <iostream>
#include <string>

#include <wx/jsonreader.h>
#include <wx/wfstream.h>

#define CHECK(expression)                                            \
  do {                                                               \
    if (!(expression)) {                                             \
      std::cerr << "check failed at " << __FILE__ << ':' << __LINE__ \
                << ": " #expression "\n";                            \
      return 1;                                                      \
    }                                                                \
  } while (false)

namespace {

bool ReadJson(const wxString& path, wxJSONValue* value) {
  wxFileInputStream input(path);
  wxJSONReader reader;
  return input.IsOk() && reader.Parse(input, value) == 0;
}

}  // namespace

int main() {
  wxJSONValue routing;
  wxJSONValue environment;
  wxJSONValue polars;
  CHECK(ReadJson(PPM_TEST_ROUTING_UI, &routing));
  CHECK(ReadJson(PPM_TEST_IGRIB_UI, &environment));
  CHECK(ReadJson(PPM_TEST_IPOLARS_UI, &polars));

  ppm::DeclarativeSurface parsed;
  std::string diagnostic;
  if (!ppm::ParseDeclarativeSurface(routing, "routing-workbench", &parsed,
                                    &diagnostic)) {
    std::cerr << "routing surface: " << diagnostic << '\n';
    return 1;
  }
  CHECK(parsed.tabs.size() == 4);
  CHECK(parsed.tabs[0] == "Basic");
  CHECK(parsed.tabs[1] == "Advanced");
  CHECK(parsed.tabs[2] == "Safety & Engine");
  CHECK(parsed.tabs[3] == "Results");
  CHECK(parsed.menus.size() == 5);
  CHECK(parsed.controls.size() == 80);
  CHECK(parsed.controls.back().id == "progress");
  const auto reset =
      std::find_if(parsed.controls.begin(), parsed.controls.end(),
                   [](const ppm::UiControl& control) {
                     return control.id == "reset-advanced";
                   });
  CHECK(reset != parsed.controls.end());
  CHECK(reset->tab == "Advanced");
  CHECK(routing["manager"]["positions"]["columns"].Size() == 4);
  CHECK(routing["manager"]["positions"]["columns"][1].AsString() == "Lat");
  CHECK(routing["manager"]["routings"]["columns"].Size() == 9);
  CHECK(routing["manager"]["routings"]["columns"][0].AsString() == "Vis");
  CHECK(routing["manager"]["routings"]["columns"][8].AsString() == "State");
  CHECK(routing["manager"]["actions"].Size() == 5);

  if (!ppm::ParseDeclarativeSurface(environment, "environment.viewer", &parsed,
                                    &diagnostic)) {
    std::cerr << "environment surface: " << diagnostic << '\n';
    return 1;
  }
  CHECK(parsed.id == "environment.viewer");
  CHECK(parsed.controls.size() == 15);
  CHECK(parsed.tabs.empty());

  if (!ppm::ParseDeclarativeSurface(polars, "polars.editor", &parsed,
                                    &diagnostic)) {
    std::cerr << "iPolars surface: " << diagnostic << '\n';
    return 1;
  }
  CHECK(parsed.tabs.size() == 5);
  CHECK(parsed.controls.size() == 40);
  CHECK(parsed.controls[0].type == "polar-plot");
  CHECK(parsed.controls[13].type == "grid");
  CHECK(parsed.controls[19].type == "file-open-multiple");

  wxJSONValue invalid = routing;
  invalid["controls"][1]["id"] = invalid["controls"][0]["id"];
  CHECK(!ppm::ParseDeclarativeSurface(invalid, "routing-workbench", &parsed,
                                      &diagnostic));

  invalid = routing;
  invalid["controls"][0]["type"] = "native-window-handle";
  CHECK(!ppm::ParseDeclarativeSurface(invalid, "routing-workbench", &parsed,
                                      &diagnostic));

  invalid = routing;
  invalid["controls"][0]["filter"] = "All files|*";
  CHECK(!ppm::ParseDeclarativeSurface(invalid, "routing-workbench", &parsed,
                                      &diagnostic));

  invalid = routing;
  invalid["controls"][0]["tab"] = "Secret";
  CHECK(!ppm::ParseDeclarativeSurface(invalid, "routing-workbench", &parsed,
                                      &diagnostic));

  invalid = routing;
  invalid["menus"][0]["items"][0]["checkable"] = wxString("true");
  CHECK(!ppm::ParseDeclarativeSurface(invalid, "routing-workbench", &parsed,
                                      &diagnostic));

  invalid = routing;
  invalid["controls"][0]["label"] = wxString('x', 513);
  CHECK(!ppm::ParseDeclarativeSurface(invalid, "routing-workbench", &parsed,
                                      &diagnostic));

  return 0;
}
