#include "portable_plugin_manager_pi.h"

#include <wx/app.h>
#include <wx/base64.h>
#include <wx/dcmemory.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/jsonwriter.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/utils.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <iomanip>
#include <limits>
#include <new>
#include <sstream>
#include <utility>
#include <vector>

#if defined(__WXOSX__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "manager_dialog.h"
#include "chart_safety_service.h"
#include "environment_workbench.h"
#include "surface_dialog.h"
#include "weather_routing_host.h"
#include "window_activation.h"

#ifndef DECL_EXP
#ifdef __WXMSW__
#define DECL_EXP __declspec(dllexport)
#else
#define DECL_EXP __attribute__((visibility("default")))
#endif
#endif

namespace {

std::string NavigationRevision(const wxJSONValue& value) {
  wxString encoded;
  wxJSONWriter writer(wxJSONWRITER_NONE);
  writer.Write(value, encoded);
  const std::string bytes = encoded.ToStdString();
  auto fnv = [&bytes](std::uint64_t seed) {
    std::uint64_t hash = seed;
    for (const unsigned char byte : bytes) {
      hash ^= byte;
      hash *= 1099511628211ULL;
    }
    return hash;
  };
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16)
         << fnv(1469598103934665603ULL) << std::setw(16)
         << fnv(1099511628211ULL);
  return stream.str();
}

const char* const kManagerXpm[] = {"32 32 6 1",
                                   "  c None",
                                   ". c #19324A",
                                   "+ c #126F89",
                                   "@ c #FFF7DF",
                                   "# c #E45B3C",
                                   "$ c #FFFFFF",
                                   "                                ",
                                   "           ..........           ",
                                   "        ...++++++++++...        ",
                                   "      ..++++++++++++++++..      ",
                                   "     .++++++++++++++++++++.     ",
                                   "    .++++++@@++++@@++++++++.    ",
                                   "   .+++++++@@++++@@+++++++++.   ",
                                   "  .++++++++@@++++@@++++++++++.  ",
                                   "  .++++++++@@++++@@++++++++++.  ",
                                   " .+++++++++@@++++@@+++++++++++. ",
                                   " .+++++..................+++++. ",
                                   " .++++.@@@@@@@@@@@@@@@@@@.++++. ",
                                   ".+++++.@@@@@@@@@@@@@@@@@@.+++++.",
                                   ".+++++.@@@@@###@@###@@@@@.+++++.",
                                   ".+++++.@@@@##@@@@@@##@@@@.+++++.",
                                   ".+++++.@@@@@##@@@@##@@@@@.+++++.",
                                   ".+++++.@@@@@@##@@##@@@@@@.+++++.",
                                   ".+++++.@@@@@@@@@@@@@@@@@@.+++++.",
                                   ".++++++.@@@@@@@@@@@@@@@@.++++++.",
                                   ".+++++++.@@@@@@@@@@@@@@.+++++++.",
                                   " .++++++..@@@@@@@@@@@@..++++++. ",
                                   " .++++++++..@@@@@@@@..++++++++. ",
                                   " .++++++++++........++++++++++. ",
                                   "  .++++++++++++@@++++++++++++.  ",
                                   "  .++++++++++++@@++++++++++++.  ",
                                   "   .+++++++++++@@+++++++++++.   ",
                                   "    .++++++++++@@++++++++++.    ",
                                   "     .+++++++++@@@@@++++++.     ",
                                   "      ..++++++++++@@@@@@..      ",
                                   "        ...++++++++++...        ",
                                   "           ..........           ",
                                   "                                "};

const ppm::ActionKey kManagerAction{"org.opencpn.portable-plugin-manager",
                                    "open-manager"};
const ppm::ActionKey kWeatherRoutingAction{"org.opencpn.iweather-routing",
                                           "iweather-routing.open"};

wxString ResolveStorageRoot() {
  wxString configured;
  if (wxGetEnv("OCPN_PORTABLE_PLUGIN_ROOT", &configured) &&
      !configured.IsEmpty()) {
    wxFileName path = wxFileName::DirName(configured);
    path.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
    return path.GetFullPath();
  }
  return *GetpPrivateApplicationDataLocation() + wxFILE_SEP_PATH +
         "portable-plugin-manager";
}

wxString ResolveManagerDataRoot() {
  const wxString installed = GetPluginDataDir("portable_plugin_manager_pi");
  if (!installed.empty() &&
      wxFileName::FileExists(installed + wxFileName::GetPathSeparator() +
                             "manager.svg")) {
    return installed;
  }
  return wxString::FromUTF8(PPM_SOURCE_DATA_DIR);
}

wxString ResolveManagerIcon() {
  const wxString separator = wxFileName::GetPathSeparator();
  const wxString icon = ResolveManagerDataRoot() + separator + "manager.svg";
  return wxFileName::FileExists(icon) ? icon : wxString();
}

/**
 * Render the wxDC-based environmental workbench into the current desktop
 * OpenGL canvas without using any OpenCPN-private drawing classes.
 *
 * Stock desktop plugins (including grib_pi) use glDrawPixels for this
 * compatibility path.  A keyed background is used as a defensive fallback
 * because wxGTK backends differ in how reliably they preserve alpha while
 * drawing into a 32-bit wxBitmap.  Pixels whose alpha survives retain it;
 * drawn pixels with a zero alpha are promoted to opaque.
 */
bool RenderEnvironmentWithDesktopGl(PortableEnvironmentHost* workbench,
                                    PlugIn_ViewPort* viewport) {
  if (!workbench || !viewport || viewport->pix_width <= 0 ||
      viewport->pix_height <= 0)
    return false;

  // This compatibility path holds one wxBitmap, one wxImage and one RGBA
  // upload.  Keep its worst-case transient allocation reasonable on modest
  // navigation computers while still covering a 5K display.
  constexpr int kMaximumCanvasDimension = 8'192;
  constexpr std::uint64_t kMaximumCanvasPixels = 16'777'216;
  const std::uint64_t width = static_cast<std::uint64_t>(viewport->pix_width);
  const std::uint64_t height = static_cast<std::uint64_t>(viewport->pix_height);
  if (viewport->pix_width > kMaximumCanvasDimension ||
      viewport->pix_height > kMaximumCanvasDimension ||
      width * height > kMaximumCanvasPixels) {
    wxLogWarning(
        "PPM iGRIB OpenGL overlay skipped: canvas %dx%d exceeds the "
        "bounded compatibility surface",
        viewport->pix_width, viewport->pix_height);
    return false;
  }

  // This deliberately unusual colour is only a transparency key.  The
  // environmental palette never emits it, and exact equality prevents nearby
  // anti-aliased colours from being discarded.
  constexpr unsigned char kKeyRed = 1;
  constexpr unsigned char kKeyGreen = 2;
  constexpr unsigned char kKeyBlue = 3;
  wxBitmap bitmap(viewport->pix_width, viewport->pix_height, 32);
  if (!bitmap.IsOk()) return false;
  bitmap.UseAlpha();
  wxMemoryDC memory_dc;
  memory_dc.SelectObject(bitmap);
  memory_dc.SetBackground(wxBrush(wxColour(kKeyRed, kKeyGreen, kKeyBlue, 255)));
  memory_dc.Clear();
  const bool rendered = workbench->Render(memory_dc, viewport);
  memory_dc.SelectObject(wxNullBitmap);
  if (!rendered) return false;

  wxImage image = bitmap.ConvertToImage();
  if (!image.IsOk() || !image.GetData()) return false;
  const unsigned char* rgb = image.GetData();
  const unsigned char* source_alpha =
      image.HasAlpha() ? image.GetAlpha() : nullptr;
  std::vector<unsigned char> rgba;
  try {
    rgba.resize(width * height * 4);
  } catch (const std::bad_alloc&) {
    wxLogWarning(
        "PPM iGRIB OpenGL overlay skipped: could not allocate the bounded "
        "RGBA compatibility surface");
    return false;
  }
  for (std::uint64_t index = 0; index < width * height; ++index) {
    const unsigned char red = rgb[index * 3];
    const unsigned char green = rgb[index * 3 + 1];
    const unsigned char blue = rgb[index * 3 + 2];
    const bool background =
        red == kKeyRed && green == kKeyGreen && blue == kKeyBlue;
    rgba[index * 4] = red;
    rgba[index * 4 + 1] = green;
    rgba[index * 4 + 2] = blue;
    if (background) {
      rgba[index * 4 + 3] = 0;
    } else {
      const unsigned char alpha = source_alpha ? source_alpha[index] : 255;
      rgba[index * 4 + 3] = alpha == 0 ? 255 : alpha;
    }
  }

  glPushAttrib(GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_ENABLE_BIT |
               GL_PIXEL_MODE_BIT);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
  GLint previous_unpack_alignment = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  // OpenCPN's desktop overlay projection is top-left based.  The negative
  // pixel zoom is the same orientation used by stock grib_pi.
  glRasterPos2i(0, 0);
  glPixelZoom(1.0F, -1.0F);
  glDrawPixels(viewport->pix_width, viewport->pix_height, GL_RGBA,
               GL_UNSIGNED_BYTE, rgba.data());
  glPixelZoom(1.0F, 1.0F);
  glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
  glPopAttrib();
  return true;
}

wxColour SceneColour(const ppm::OverlayColor& value) {
  return wxColour(value.red, value.green, value.blue, value.alpha);
}

std::vector<std::pair<const ppm::OverlayScene*, const ppm::OverlayLayer*>>
OrderedSceneLayers(const std::vector<ppm::OverlayScene>& scenes) {
  std::vector<std::pair<const ppm::OverlayScene*, const ppm::OverlayLayer*>>
      layers;
  for (const auto& scene : scenes) {
    for (const auto& layer : scene.layers)
      if (layer.visible) layers.push_back({&scene, &layer});
  }
  std::stable_sort(layers.begin(), layers.end(),
                   [](const auto& left, const auto& right) {
                     return left.second->z_index < right.second->z_index;
                   });
  return layers;
}

bool SceneTargetsCanvas(const ppm::OverlayScene& scene, int canvas_index,
                        int priority) {
  const int phase_priority = scene.render_phase == "above-ui"        ? 2
                             : scene.render_phase == "above-vessels" ? 1
                                                                     : 0;
  if (priority != phase_priority) return false;
  if (scene.canvas_target == "primary") return canvas_index == 0;
  if (scene.canvas_target == "selected")
    return std::find(scene.selected_canvases.begin(),
                     scene.selected_canvases.end(),
                     static_cast<std::uint32_t>(std::max(0, canvas_index))) !=
           scene.selected_canvases.end();
  return true;
}

bool RenderPortableScenes(const std::vector<ppm::OverlayScene>& scenes,
                          wxDC& dc, PlugIn_ViewPort* viewport,
                          int canvas_index = 0, int priority = 0) {
  if (!viewport) return false;
  bool rendered = false;
  for (const auto& scene : scenes) {
    if (!SceneTargetsCanvas(scene, canvas_index, priority)) continue;
    if (scene.points.size() < 2) continue;
    dc.SetPen(wxPen(wxColour(scene.red, scene.green, scene.blue, scene.alpha),
                    std::max(1, static_cast<int>(scene.width_pixels))));
    wxPoint previous;
    GetCanvasPixLL(viewport, &previous, scene.points.front().latitude,
                   scene.points.front().longitude);
    for (std::size_t index = 1; index < scene.points.size(); ++index) {
      wxPoint next;
      GetCanvasPixLL(viewport, &next, scene.points[index].latitude,
                     scene.points[index].longitude);
      dc.DrawLine(previous, next);
      previous = next;
    }
    rendered = true;
  }

  for (const auto& [scene, layer] : OrderedSceneLayers(scenes)) {
    if (!SceneTargetsCanvas(*scene, canvas_index, priority)) continue;
    for (const auto& primitive : layer->primitives) {
      std::vector<wxPoint> points;
      points.reserve(primitive.points.size());
      for (const auto& value : primitive.points) {
        wxPoint point;
        GetCanvasPixLL(viewport, &point, value.latitude, value.longitude);
        points.push_back(point);
      }
      const auto set_style = [&]() {
        if (primitive.style.has_stroke) {
          wxPen pen(SceneColour(primitive.style.stroke),
                    std::max(1, static_cast<int>(std::lround(
                                    primitive.style.width_pixels))));
          if (!primitive.style.dash_pattern.empty()) {
            std::vector<wxDash> dashes;
            dashes.reserve(primitive.style.dash_pattern.size());
            for (const float dash : primitive.style.dash_pattern)
              dashes.push_back(static_cast<wxDash>(
                  std::max(1, static_cast<int>(std::lround(dash)))));
            pen.SetStyle(wxPENSTYLE_USER_DASH);
            pen.SetDashes(static_cast<int>(dashes.size()), dashes.data());
          }
          dc.SetPen(pen);
        } else {
          dc.SetPen(*wxTRANSPARENT_PEN);
        }
        dc.SetBrush(primitive.style.has_fill
                        ? wxBrush(SceneColour(primitive.style.fill))
                        : *wxTRANSPARENT_BRUSH);
      };
      switch (primitive.kind) {
        case ppm::OverlayPrimitiveKind::kPolyline:
          set_style();
          if (points.size() >= 2)
            dc.DrawLines(static_cast<int>(points.size()), points.data());
          break;
        case ppm::OverlayPrimitiveKind::kPolygon:
          set_style();
          if (points.size() >= 3)
            dc.DrawPolygon(static_cast<int>(points.size()), points.data());
          break;
        case ppm::OverlayPrimitiveKind::kCircle: {
          set_style();
          wxPoint centre;
          wxPoint edge;
          GetCanvasPixLL(viewport, &centre, primitive.centre.latitude,
                         primitive.centre.longitude);
          GetCanvasPixLL(
              viewport, &edge,
              std::min(90.0, primitive.centre.latitude +
                                 primitive.radius_metres / 111'320.0),
              primitive.centre.longitude);
          const int radius =
              std::max(1, static_cast<int>(std::lround(std::hypot(
                              static_cast<double>(edge.x - centre.x),
                              static_cast<double>(edge.y - centre.y)))));
          dc.DrawCircle(centre, radius);
          break;
        }
        case ppm::OverlayPrimitiveKind::kIcon: {
          wxPoint centre;
          GetCanvasPixLL(viewport, &centre, primitive.centre.latitude,
                         primitive.centre.longitude);
          wxImage image(wxString::FromUTF8(primitive.resource_path));
          if (!image.IsOk()) break;
          const int width = std::max(
              1, static_cast<int>(std::lround(primitive.width_pixels)));
          const int height = std::max(
              1, static_cast<int>(std::lround(primitive.height_pixels)));
          image.Rescale(width, height, wxIMAGE_QUALITY_HIGH);
          if (std::abs(primitive.rotation_degrees) > 0.01F)
            image = image.Rotate(
                primitive.rotation_degrees * 3.14159265358979323846 / 180.0,
                wxPoint(static_cast<int>(primitive.anchor_x * width),
                        static_cast<int>(primitive.anchor_y * height)),
                true);
          dc.DrawBitmap(
              wxBitmap(image),
              centre.x - static_cast<int>(primitive.anchor_x * width),
              centre.y - static_cast<int>(primitive.anchor_y * height), true);
          break;
        }
        case ppm::OverlayPrimitiveKind::kText: {
          wxPoint position;
          GetCanvasPixLL(viewport, &position, primitive.centre.latitude,
                         primitive.centre.longitude);
          const wxFont previous = dc.GetFont();
          wxFont font = previous;
          font.SetPixelSize(wxSize(
              0, std::max(
                     6, static_cast<int>(std::lround(primitive.size_pixels)))));
          dc.SetFont(font);
          dc.SetTextForeground(SceneColour(primitive.text_color));
          const wxString text = wxString::FromUTF8(primitive.text);
          const wxSize extent = dc.GetTextExtent(text);
          if (primitive.horizontal_alignment == "centre")
            position.x -= extent.x / 2;
          else if (primitive.horizontal_alignment == "right")
            position.x -= extent.x;
          if (std::abs(primitive.rotation_degrees) > 0.01F)
            dc.DrawRotatedText(text, position, primitive.rotation_degrees);
          else
            dc.DrawText(text, position);
          dc.SetFont(previous);
          break;
        }
      }
      rendered = true;
    }
  }
  return rendered;
}

bool RenderPortableScenesWithDesktopGl(
    const std::vector<ppm::OverlayScene>& scenes, PlugIn_ViewPort* viewport,
    int canvas_index = 0, int priority = 0) {
  if (!viewport || scenes.empty() || viewport->pix_width <= 0 ||
      viewport->pix_height <= 0)
    return false;
  constexpr int kMaximumCanvasDimension = 8'192;
  constexpr std::uint64_t kMaximumCanvasPixels = 16'777'216;
  const std::uint64_t width = viewport->pix_width;
  const std::uint64_t height = viewport->pix_height;
  if (viewport->pix_width > kMaximumCanvasDimension ||
      viewport->pix_height > kMaximumCanvasDimension ||
      width * height > kMaximumCanvasPixels)
    return false;

  constexpr unsigned char kKeyRed = 1;
  constexpr unsigned char kKeyGreen = 2;
  constexpr unsigned char kKeyBlue = 3;
  wxBitmap bitmap(viewport->pix_width, viewport->pix_height, 32);
  if (!bitmap.IsOk()) return false;
  bitmap.UseAlpha();
  wxMemoryDC dc;
  dc.SelectObject(bitmap);
  dc.SetBackground(wxBrush(wxColour(kKeyRed, kKeyGreen, kKeyBlue, 255)));
  dc.Clear();
  const bool rendered =
      RenderPortableScenes(scenes, dc, viewport, canvas_index, priority);
  dc.SelectObject(wxNullBitmap);
  if (!rendered) return false;

  wxImage image = bitmap.ConvertToImage();
  if (!image.IsOk() || !image.GetData()) return false;
  const unsigned char* rgb = image.GetData();
  const unsigned char* alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
  std::vector<unsigned char> rgba;
  try {
    rgba.resize(width * height * 4);
  } catch (const std::bad_alloc&) {
    return false;
  }
  for (std::uint64_t index = 0; index < width * height; ++index) {
    const unsigned char red = rgb[index * 3];
    const unsigned char green = rgb[index * 3 + 1];
    const unsigned char blue = rgb[index * 3 + 2];
    const bool background =
        red == kKeyRed && green == kKeyGreen && blue == kKeyBlue;
    rgba[index * 4] = red;
    rgba[index * 4 + 1] = green;
    rgba[index * 4 + 2] = blue;
    rgba[index * 4 + 3] =
        background ? 0 : (alpha && alpha[index] ? alpha[index] : 255);
  }
  glPushAttrib(GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_ENABLE_BIT |
               GL_PIXEL_MODE_BIT);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
  GLint unpack_alignment = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glRasterPos2i(0, 0);
  glPixelZoom(1.0F, -1.0F);
  glDrawPixels(viewport->pix_width, viewport->pix_height, GL_RGBA,
               GL_UNSIGNED_BYTE, rgba.data());
  glPixelZoom(1.0F, 1.0F);
  glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
  glPopAttrib();
  return true;
}

}  // namespace

extern "C" DECL_EXP opencpn_plugin* create_pi(void* manager) {
  return new ppm::PortablePluginManagerPi(manager);
}

extern "C" DECL_EXP void destroy_pi(opencpn_plugin* plugin) { delete plugin; }

namespace ppm {

PortablePluginManagerPi::PortablePluginManagerPi(void* manager)
    : opencpn_plugin_121(manager), plugin_bitmap_(kManagerXpm) {}

PortablePluginManagerPi::~PortablePluginManagerPi() {
  if (initialized_) DeInit();
}

int PortablePluginManagerPi::Init() {
  if (initialized_) {
    wxLogWarning("PPM event=duplicate-init");
    return WANTS_TOOLBAR_CALLBACK | INSTALLS_TOOLBAR_TOOL | WANTS_CONFIG |
           WANTS_NMEA_EVENTS | WANTS_NMEA_SENTENCES | WANTS_OVERLAY_CALLBACK |
           WANTS_OPENGL_OVERLAY_CALLBACK | WANTS_CURSOR_LATLON |
           WANTS_PLUGIN_MESSAGING | INSTALLS_CONTEXTMENU_ITEMS |
           WANTS_MOUSE_EVENTS | WANTS_KEYBOARD_EVENTS | WANTS_AIS_SENTENCES;
  }
  initialized_ = true;
  storage_root_ = ResolveStorageRoot();
  wxLogMessage("PPM event=init api=1.21 version=0.2.1");
  wxLogMessage("PPM event=storage-root path=%s", storage_root_);
  {
    std::vector<std::string> chart_roots;
    const wxArrayString configured_chart_roots = GetChartDBDirArrayString();
    chart_roots.reserve(configured_chart_roots.size());
    for (const auto& root : configured_chart_roots)
      chart_roots.push_back(root.ToStdString());
    ChartSafetyService::ConfigureChartRoots(chart_roots);
    wxLogMessage("PPM event=chart-safety-provider status=%s",
                 wxString::FromUTF8(ChartSafetyService().Summary()));
  }
  if (!RegisterManagerAction()) {
    wxLogError("PPM event=manager-action-registration-failed");
  }
  wxString developer;
  developer_mode_ =
      wxGetEnv("OCPN_PPM_DEVELOPER_MODE", &developer) && developer == "1";
  const wxString trust_root =
      ResolveManagerDataRoot() + wxFileName::GetPathSeparator() + "trust";
  package_store_ = std::make_unique<PackageStore>(storage_root_.ToStdString(),
                                                  trust_root.ToStdString());
  package_store_->SetDeveloperMode(developer_mode_);
  permission_store_ =
      std::make_unique<PermissionStore>(storage_root_.ToStdString());
  for (const auto& package : package_store_->Installed()) {
    if (!package.enabled) continue;
    const StoreResult audit = package_store_->AuditInstalled(package.id);
    if (!audit.okay) {
      package_store_->SetEnabled(package.id, false);
      wxLogError(
          "PPM event=installed-integrity-failed package=%s "
          "diagnostic=%s",
          package.id, audit.message);
    }
  }
  runtime_engine_ = std::make_unique<RuntimeEngine>(
      storage_root_.ToStdString(),
      [this](const RuntimeAction& action, std::uint32_t* host_action_id) {
        return RegisterPortableAction(action, host_action_id);
      },
      [this](const std::string& package_id) {
        RemovePackageActions(package_id);
      },
      [this]() { OnEngineStateChanged(); },
      [gate = (ui_callback_gate_ = std::make_shared<std::atomic_bool>(true))](
          std::function<void()> task) {
        if (!wxTheApp || !task) return;
        wxTheApp->CallAfter([gate, task = std::move(task)]() mutable {
          if (gate->load()) task();
        });
      });
  runtime_engine_->SetSurfaceOpenedCallback(
      [this](const std::string& package_id, const DeclarativeSurface& surface) {
        OpenPackageSurface(package_id, surface);
      });
  runtime_engine_->SetSurfaceResponseCallback(
      [this](const std::string& package_id, const std::string& surface_id,
             const std::string& control_id, const std::string& state_json,
             const std::string& diagnostic) {
        ApplySurfaceResponse(package_id, surface_id, control_id, state_json,
                             diagnostic);
      });
  runtime_engine_->SetRoutingProgressCallback(
      [this](const std::string& package_id, std::uint8_t percent,
             const std::string& message) {
        if (package_id == "org.opencpn.iweather-routing" &&
            weather_routing_host_) {
          weather_routing_host_->ReportProgress(percent,
                                                wxString::FromUTF8(message));
        }
      });
  runtime_engine_->SetPluginMessageSender(
      [](const std::string& message_id, const std::string& message_body) {
        ::SendPluginMessage(wxString::FromUTF8(message_id),
                            wxString::FromUTF8(message_body));
      });
  runtime_engine_->SetAuthorUiRequestCallback(
      [this](const std::string& package_id, const std::string& operation,
             const std::string& request_json, std::string* response_json) {
        return HandleAuthorUiRequest(package_id, operation, request_json,
                                     response_json);
      });
  if (!runtime_engine_->LoadInstalled(developer_mode_)) {
    wxLogWarning(
        "PPM event=runtime-engine-load-completed-with-package-failures");
  }
  for (const auto& package : package_store_->Installed()) {
    if (!package.enabled) continue;
    wxString diagnostic;
    std::string runtime_diagnostic;
    if (!PreparePermissions(package.id, false, &diagnostic) ||
        !runtime_engine_->Enable(package.id, &runtime_diagnostic)) {
      package_store_->SetEnabled(package.id, false);
      wxLogWarning(
          "PPM event=startup-package-disabled package=%s "
          "diagnostic=%s%s",
          package.id, diagnostic, wxString::FromUTF8(runtime_diagnostic));
    }
  }
  if (developer_mode_) {
    wxString startup_action;
    if (wxGetEnv("OCPN_PPM_DEVELOPER_STARTUP_ACTION", &startup_action) &&
        !startup_action.empty()) {
      const int separator = startup_action.Find(':');
      if (separator > 0 &&
          separator + 1 < static_cast<int>(startup_action.size())) {
        const std::string package_id =
            startup_action.Left(separator).ToStdString();
        const std::string action_id =
            startup_action.Mid(separator + 1).ToStdString();
        wxTheApp->CallAfter([this, package_id, action_id]() {
          if (!initialized_ || !runtime_engine_ ||
              !runtime_engine_->HandleAction(package_id, action_id)) {
            wxLogWarning(
                "PPM developer startup action failed package=%s action=%s",
                package_id, action_id);
          } else {
            wxLogMessage(
                "PPM developer startup action invoked package=%s action=%s",
                package_id, action_id);
          }
        });
      } else {
        wxLogWarning("PPM ignored malformed OCPN_PPM_DEVELOPER_STARTUP_ACTION");
      }
    }
  }
  static constexpr std::uint32_t kPortableNmea2000Pgns[] = {
      127245, 127250, 127257, 128259, 128267, 128275, 129025,
      129026, 129029, 129540, 130306, 130310, 130313};
  nmea2000_handler_ = std::make_unique<wxEvtHandler>();
  nmea2000_listeners_.reserve(std::size(kPortableNmea2000Pgns));
  nmea2000_event_types_.reserve(std::size(kPortableNmea2000Pgns));
  for (const std::uint32_t pgn : kPortableNmea2000Pgns) {
    const wxEventType event_type = wxNewEventType();
    nmea2000_event_types_.push_back(event_type);
    nmea2000_listeners_.push_back(GetListener(NMEA2000Id(static_cast<int>(pgn)),
                                              event_type,
                                              nmea2000_handler_.get()));
    nmea2000_handler_->Bind(wxEventTypeTag<ObservedEvt>(event_type),
                            [this, pgn](ObservedEvt event) {
                              HandleNmea2000(pgn, std::move(event));
                            });
  }
  return WANTS_TOOLBAR_CALLBACK | INSTALLS_TOOLBAR_TOOL | WANTS_CONFIG |
         WANTS_NMEA_EVENTS | WANTS_NMEA_SENTENCES | WANTS_OVERLAY_CALLBACK |
         WANTS_OPENGL_OVERLAY_CALLBACK | WANTS_CURSOR_LATLON |
         WANTS_PLUGIN_MESSAGING | INSTALLS_CONTEXTMENU_ITEMS |
         WANTS_MOUSE_EVENTS | WANTS_KEYBOARD_EVENTS | WANTS_AIS_SENTENCES;
}

bool PortablePluginManagerPi::DeInit() {
  if (!initialized_) return true;
  nmea2000_listeners_.clear();
  nmea2000_event_types_.clear();
  nmea2000_handler_.reset();
  if (manager_dialog_) {
    manager_dialog_->Hide();
    manager_dialog_->Destroy();
    manager_dialog_.release();
  }
  if (weather_routing_host_) {
    weather_routing_host_->Shutdown();
    weather_routing_host_.reset();
  }
  if (environment_workbench_) {
    environment_workbench_->Shutdown();
    environment_workbench_.reset();
  }
  surface_dialogs_.clear();
  if (runtime_engine_) {
    runtime_engine_->Shutdown();
    if (ui_callback_gate_) ui_callback_gate_->store(false);
    runtime_engine_.reset();
  }
  ui_callback_gate_.reset();
  permission_store_.reset();
  package_store_.reset();
  RemoveAllActions();
  initialized_ = false;
  wxLogMessage("PPM event=deinit remaining-actions=%zu", actions_.Size());
  return true;
}

wxString PortablePluginManagerPi::GetShortDescription() {
  return "Installs and manages capability-limited portable runtime plugins.";
}

wxString PortablePluginManagerPi::GetLongDescription() {
  return "A conventional OpenCPN plugin which verifies, installs, enables, "
         "disables and unloads portable WebAssembly packages without changes "
         "to OpenCPN core.";
}

bool PortablePluginManagerPi::RegisterManagerAction() {
  const wxString icon = ResolveManagerIcon();
  const int tool_id =
      icon.empty()
          ? InsertPlugInTool("Portable Plugin Manager", &plugin_bitmap_,
                             &plugin_bitmap_, wxITEM_NORMAL,
                             "Portable Plugin Manager",
                             "Install and manage portable runtime plugins",
                             nullptr, -1, 0, this)
          : InsertPlugInToolSVG("Portable Plugin Manager", icon, icon, icon,
                                wxITEM_NORMAL, "Portable Plugin Manager",
                                "Install and manage portable runtime plugins",
                                nullptr, -1, 0, this);
  if (!actions_.Add(kManagerAction, tool_id)) {
    if (tool_id >= 0) RemovePlugInTool(tool_id);
    return false;
  }
  wxLogMessage("PPM event=action-registered package=%s action=%s tool=%d",
               kManagerAction.package_id, kManagerAction.action_id, tool_id);
  return true;
}

int PortablePluginManagerPi::RegisterPortableAction(
    const RuntimeAction& action, std::uint32_t* host_action_id) {
  if (!host_action_id) return -1;
  const ActionKey key{action.package_id, action.action_id};
  if (actions_.Find(key)) return -5;
  int tool_id = -1;
  const wxString label = wxString::FromUTF8(action.label);
  const wxString tooltip = wxString::FromUTF8(action.tooltip);
  if (action.toolbar) {
    if (!action.icon_path.empty()) {
      const wxString icon = wxString::FromUTF8(action.icon_path);
      tool_id = InsertPlugInToolSVG(label, icon, icon, icon, wxITEM_NORMAL,
                                    tooltip, tooltip, nullptr, -1, 0, this);
    } else {
      tool_id = InsertPlugInTool(label, &plugin_bitmap_, &plugin_bitmap_,
                                 wxITEM_NORMAL, tooltip, tooltip, nullptr, -1,
                                 0, this);
    }
  }
  int context_id = -1;
  std::vector<int> context_ids;
  const bool weather_route_analysis = key == kWeatherRoutingAction;
  std::vector<std::string> locations = action.locations;
  if (locations.empty() && action.context_menu)
    locations.push_back("chart-context-menu");
  if (weather_route_analysis &&
      std::find(locations.begin(), locations.end(), "chart-context-menu") ==
          locations.end())
    locations.push_back("chart-context-menu");
  for (const auto& location : locations) {
    if (location == "toolbar") continue;
    auto* item = new wxMenuItem(
        nullptr, wxID_ANY,
        weather_route_analysis ? "Weather Route Analysis…" : label, tooltip);
    if (location == "chart-context-menu") {
      context_id = AddCanvasContextMenuItem(item, this);
    } else {
      static const std::map<std::string, std::string> object_types{
          {"ais-context-menu", "AIS"},
          {"route-context-menu", "Route"},
          {"waypoint-context-menu", "Waypoint"},
          {"track-context-menu", "Track"}};
      const auto object_type = object_types.find(location);
      if (object_type == object_types.end()) {
        delete item;
        continue;
      }
      context_id = AddCanvasContextMenuItemExt(item, this, object_type->second);
    }
    if (context_id >= 0) context_ids.push_back(context_id);
  }
  if (!actions_.Add(key, tool_id, context_ids)) {
    if (tool_id >= 0) RemovePlugInTool(tool_id);
    for (const int registered_id : context_ids)
      RemoveCanvasContextMenuItem(registered_id);
    return -2;
  }
  if (tool_id >= 0) SetToolbarToolViz(tool_id, true);
  if (weather_route_analysis && context_id >= 0)
    SetCanvasContextMenuItemViz(context_id, false);
  *host_action_id =
      static_cast<std::uint32_t>(tool_id >= 0 ? tool_id : context_id);
  wxLogMessage(
      "PPM event=package-action-registered package=%s action=%s tool=%d "
      "context=%d",
      action.package_id, action.action_id, tool_id, context_id);
  return 0;
}

void PortablePluginManagerPi::RemoveAllActions() {
  for (const auto& action : actions_.Clear()) {
    if (action.tool_id >= 0) RemovePlugInTool(action.tool_id);
    for (const int context_id : action.context_ids)
      RemoveCanvasContextMenuItem(context_id);
    wxLogMessage("PPM event=action-removed package=%s action=%s tool=%d",
                 action.key.package_id, action.key.action_id, action.tool_id);
  }
}

void PortablePluginManagerPi::RemovePackageActions(
    const std::string& package_id) {
  for (const auto& action : actions_.RemovePackage(package_id)) {
    if (action.tool_id >= 0) RemovePlugInTool(action.tool_id);
    for (const int context_id : action.context_ids)
      RemoveCanvasContextMenuItem(context_id);
    wxLogMessage("PPM event=action-removed package=%s action=%s tool=%d",
                 action.key.package_id, action.key.action_id, action.tool_id);
  }
}

void PortablePluginManagerPi::OnToolbarToolCallback(int id) {
  const auto action = actions_.FindByToolId(id);
  if (!action || !action->dispatchable) {
    wxLogWarning("PPM event=unmapped-or-blocked-toolbar-click tool=%d", id);
    return;
  }
  if (action->key == kManagerAction) {
    ShowManager(nullptr);
  } else if (runtime_engine_) {
    RuntimeActionContext context;
    context.location = "toolbar";
    runtime_engine_->HandleAction(action->key.package_id, action->key.action_id,
                                  std::move(context));
  }
}

void PortablePluginManagerPi::OnContextMenuItemCallback(int id) {
  const auto action = actions_.FindByContextId(id);
  if (!action || !action->dispatchable) {
    wxLogWarning("PPM event=unmapped-or-blocked-context-command item=%d", id);
    return;
  }
  if (action->key == kWeatherRoutingAction) {
    pending_weather_route_guid_ = GetSelectedRouteGUID_Plugin();
    if (pending_weather_route_guid_.empty()) {
      wxLogWarning("PPM event=weather-route-analysis-no-route");
      return;
    }
  }
  if (runtime_engine_) {
    RuntimeActionContext context;
    context.location = "chart-context-menu";
    context.canvas_index =
        static_cast<std::uint32_t>(std::max(0, context_canvas_index_));
    context.has_canvas_index = true;
    context.latitude = cursor_latitude_;
    context.longitude = cursor_longitude_;
    context.has_position = cursor_position_valid_;
    runtime_engine_->HandleAction(action->key.package_id, action->key.action_id,
                                  std::move(context));
  }
}

void PortablePluginManagerPi::PrepareContextMenu(int canvas_index) {
  context_canvas_index_ = canvas_index;
  const Action* action = actions_.Find(kWeatherRoutingAction);
  if (!action || action->context_id < 0) return;
  const bool route_selected = !GetSelectedRouteGUID_Plugin().empty();
  SetCanvasContextMenuItemViz(action->context_id,
                              action->dispatchable && route_selected);
  SetCanvasContextMenuItemGrey(action->context_id, !action->dispatchable);
}

void PortablePluginManagerPi::OnContextMenuItemCallbackExt(
    int id, std::string object_ident, std::string object_type, double latitude,
    double longitude) {
  const auto action = actions_.FindByContextId(id);
  if (!action || !action->dispatchable || !runtime_engine_) return;
  std::string location = "chart-context-menu";
  if (object_type == "AIS")
    location = "ais-context-menu";
  else if (object_type == "Route")
    location = "route-context-menu";
  else if (object_type == "Waypoint")
    location = "waypoint-context-menu";
  else if (object_type == "Track")
    location = "track-context-menu";
  RuntimeActionContext context;
  context.location = location;
  context.canvas_index =
      static_cast<std::uint32_t>(std::max(0, context_canvas_index_));
  context.has_canvas_index = true;
  context.latitude = latitude;
  context.longitude = longitude;
  context.has_position = std::isfinite(latitude) && std::isfinite(longitude);
  context.object_kind = std::move(object_type);
  context.object_id = std::move(object_ident);
  runtime_engine_->HandleAction(action->key.package_id, action->key.action_id,
                                std::move(context));
}

void PortablePluginManagerPi::ShowPreferencesDialog(wxWindow* parent) {
  ShowManager(parent);
}

void PortablePluginManagerPi::ShowManager(wxWindow* parent) {
  if (!manager_dialog_) {
    ManagerCallbacks callbacks;
    callbacks.install = [this](const std::string& path) {
      InstallPackage(path);
    };
    callbacks.enable = [this](const std::string& id) { EnablePackage(id); };
    callbacks.disable = [this](const std::string& id) { DisablePackage(id); };
    callbacks.unload = [this](const std::string& id) { UnloadPackage(id); };
    callbacks.remove = [this](const std::string& id) { RemovePackage(id); };
    callbacks.rollback = [this](const std::string& id) { RollbackPackage(id); };
    callbacks.revoke_permissions = [this](const std::string& id) {
      RevokePackagePermissions(id);
    };
    manager_dialog_ = std::make_unique<ManagerDialog>(
        ResolveOpenCpnTopLevelParent(parent), std::move(callbacks));
    manager_dialog_->SetRuntimeSummary(
        "Host plugin loaded in stock OpenCPN.\nPackage store: " +
        storage_root_);
  }
  RefreshManager();
  ShowAndActivateWindow(manager_dialog_.get());
  wxLogMessage("PPM event=manager-shown");
}

void PortablePluginManagerPi::SetManagerStatus(const wxString& status) {
  if (manager_dialog_) manager_dialog_->SetStatus(status);
  wxLogMessage("PPM event=manager-operation status=%s", status);
}

void PortablePluginManagerPi::OpenPackageSurface(
    const std::string& package_id, const DeclarativeSurface& surface) {
  if (package_id == "org.opencpn.igrib" && surface.id == "environment.viewer") {
    if (!environment_workbench_) {
      const wxString separator = wxFileName::GetPathSeparator();
      const wxString package_root = storage_root_ + separator + "packages" +
                                    separator + wxString::FromUTF8(package_id);
      bool credential_access = false;
      if (package_store_) {
        for (const auto& package : package_store_->Installed()) {
          if (package.id == package_id) {
            credential_access =
                std::find(package.permissions.begin(),
                          package.permissions.end(),
                          "credentials.provider") != package.permissions.end();
            break;
          }
        }
      }
      environment_workbench_ = std::make_unique<PortableEnvironmentHost>(
          ResolveOpenCpnTopLevelParent(), wxString::FromUTF8(package_id),
          package_root, "ui/igrib-viewer.ui.json", credential_access,
          [this, package_id](const wxString& control_id,
                             const wxString& value_json, wxString* error,
                             wxString* accepted_state) {
            if (accepted_state) accepted_state->clear();
            const wxScopedCharBuffer control = control_id.utf8_str();
            const wxScopedCharBuffer value = value_json.utf8_str();
            if (!control || !value || !runtime_engine_ ||
                !runtime_engine_->HandleSurfaceEvent(
                    package_id, "environment.viewer",
                    std::string(control.data(), control.length()),
                    std::string(value.data(), value.length()))) {
              if (error)
                *error = "portable environmental controller is unavailable";
              return false;
            }
            return true;
          },
          [this, package_id](const wxString& path) {
            const wxScopedCharBuffer value = path.utf8_str();
            if (!value || !runtime_engine_ ||
                !runtime_engine_->SelectEnvironmentDataset(
                    package_id, {std::string(value.data(), value.length())})) {
              SetManagerStatus(
                  "iGRIB opened the dataset, but the typed routing provider "
                  "could not adopt it.");
            }
          },
          [this](double* west, double* south, double* east, double* north) {
            if (!view_bounds_valid_ || !west || !south || !east || !north)
              return false;
            *west = view_west_;
            *south = view_south_;
            *east = view_east_;
            *north = view_north_;
            return true;
          },
          []() { RequestRefresh(GetOCPNCanvasWindow()); },
          [this]() {
            std::vector<PortableEnvironmentPosition> result;
            for (const auto& waypoint : ListWaypoints()) {
              result.push_back({waypoint.id, waypoint.name, waypoint.latitude,
                                waypoint.longitude});
            }
            return result;
          },
          [this](PortableEnvironmentPosition* position) {
            if (!position || !vessel_position_valid_) return false;
            *position = {"opencpn:vessel", "Current boat position",
                         vessel_latitude_, vessel_longitude_};
            return true;
          });
    }
    wxString error;
    if (!environment_workbench_->Show(&error)) {
      SetManagerStatus("Could not open iGRIB: " + error);
      return;
    }
    if (developer_mode_ && !developer_smoke_fixture_opened_) {
      wxString fixture;
      if (wxGetEnv("OCPN_PPM_IGRIB_SMOKE_FIXTURE", &fixture) &&
          !fixture.empty()) {
        developer_smoke_fixture_opened_ = true;
        if (!environment_workbench_->OpenDataset({fixture}, &error)) {
          SetManagerStatus("Could not open the iGRIB smoke fixture: " + error);
        } else {
          wxLogMessage("PPM iGRIB developer smoke fixture requested: %s",
                       fixture);
        }
      }
    }
    return;
  }

  if (package_id == "org.opencpn.iweather-routing" &&
      surface.id == "routing.workbench") {
    if (!weather_routing_host_) {
      const wxString separator = wxFileName::GetPathSeparator();
      const wxString package_root = storage_root_ + separator + "packages" +
                                    separator + wxString::FromUTF8(package_id);
      weather_routing_host_ = std::make_unique<PortableWeatherRoutingHost>(
          ResolveOpenCpnTopLevelParent(), GetOCPNConfigObject(),
          [this, package_id](RoutingRequest request, RoutingOutcome* outcome,
                             std::string* diagnostic) {
            return runtime_engine_ &&
                   runtime_engine_->CalculateRouteBlocking(
                       package_id, std::move(request), outcome, diagnostic);
          },
          [this, package_id](RoutingPassageRequest request,
                             RoutingOutcome* outcome, std::string* diagnostic) {
            return runtime_engine_ &&
                   runtime_engine_->CalculatePassageBlocking(
                       package_id, std::move(request), outcome, diagnostic);
          },
          [this, package_id](wxString* diagnostic) {
            std::string error;
            const bool okay =
                runtime_engine_ &&
                runtime_engine_->BeginRouteAttempt(package_id, &error);
            if (!okay && diagnostic) *diagnostic = wxString::FromUTF8(error);
            return okay;
          },
          [this, package_id]() {
            if (runtime_engine_) runtime_engine_->CancelRoute(package_id);
          },
          package_root, wxString::FromUTF8(package_id),
          "ui/iweather-routing.ui.json",
          [this]() {
            return runtime_engine_
                       ? wxString::FromUTF8(runtime_engine_->EnvironmentSummary(
                             "org.opencpn.igrib"))
                       : wxString("Environmental provider unavailable");
          },
          [this]() { return ListWaypoints(); },
          [this]() { return ListRoutes(); },
          [this](const wxString& name,
                 const std::vector<PortableNavigationPosition>& points,
                 wxString* diagnostic) {
            return CreateOpenCpnRoute(name, points, diagnostic);
          },
          [this](PortableNavigationPosition* position) {
            if (!position || !vessel_position_valid_) return false;
            *position = {"vessel", "Vessel position", vessel_latitude_,
                         vessel_longitude_};
            return true;
          },
          [this](PortableNavigationPosition* position) {
            if (!position || !cursor_position_valid_) return false;
            *position = {"cursor", "Chart cursor", cursor_latitude_,
                         cursor_longitude_};
            return true;
          },
          [this](std::int64_t* unix_time) {
            return environment_workbench_ &&
                   environment_workbench_->DisplayedTime(unix_time);
          },
          [this, package_id](double latitude, double longitude,
                             const std::vector<std::int64_t>& unix_times,
                             std::vector<std::uint8_t>* availability,
                             wxString* diagnostic) {
            std::string error;
            const bool okay =
                runtime_engine_ && runtime_engine_->PreflightEnvironment(
                                       package_id, latitude, longitude,
                                       unix_times, availability, &error);
            if (!okay && diagnostic) *diagnostic = wxString::FromUTF8(error);
            return okay;
          },
          vessel_position_valid_ ? vessel_latitude_ : 0.0,
          vessel_position_valid_ ? vessel_longitude_ : 0.0);
      weather_routing_host_->SetColorScheme(static_cast<int>(colour_scheme_));
    }
    wxString error;
    const bool opened = pending_weather_route_guid_.empty()
                            ? weather_routing_host_->Show(&error)
                            : weather_routing_host_->ShowRouteAnalysis(
                                  pending_weather_route_guid_, &error);
    pending_weather_route_guid_.clear();
    if (!opened) SetManagerStatus("Could not open iWeatherRouting: " + error);
    return;
  }

  const std::string key = package_id + "\n" + surface.id;
  const auto existing = surface_dialogs_.find(key);
  if (existing != surface_dialogs_.end()) {
    ShowAndActivateWindow(existing->second.get());
    return;
  }
  auto dialog = std::make_unique<SurfaceDialog>(
      ResolveOpenCpnTopLevelParent(), surface,
      [this, package_id, surface_id = surface.id](
          const std::string& control_id, const std::string& value_json,
          const std::vector<SurfaceDialog::UserFileSelection>& selections) {
        std::string delivered_value = value_json;
        if (!selections.empty()) {
          if (package_id == "org.opencpn.igrib" &&
              surface_id == "environment.viewer" && control_id == "open") {
            std::vector<std::string> paths;
            paths.reserve(selections.size());
            for (const auto& selection : selections) {
              paths.push_back(selection.path);
            }
            if (!runtime_engine_ ||
                !runtime_engine_->SelectEnvironmentDataset(package_id, paths)) {
              ApplySurfaceResponse(
                  package_id, surface_id, control_id, {},
                  "Environmental provider is disabled or unavailable.");
              return;
            }
            // The component receives only an opaque host-state marker. GRIB
            // paths and bytes remain in the host-owned provider boundary.
            delivered_value = "\"host-environment-snapshot\"";
          } else {
            std::vector<std::string> tokens;
            tokens.reserve(selections.size());
            for (const auto& selection : selections) {
              std::string token;
              std::string diagnostic;
              if (!runtime_engine_ ||
                  !runtime_engine_->RegisterUserFileGrant(
                      package_id, selection.path, selection.writable, &token,
                      &diagnostic)) {
                ApplySurfaceResponse(package_id, surface_id, control_id, {},
                                     diagnostic.empty()
                                         ? "Could not grant access to the "
                                           "selected file."
                                         : diagnostic);
                return;
              }
              tokens.push_back(std::move(token));
            }
            if (tokens.size() == 1) {
              delivered_value = "\"" + tokens.front() + "\"";
            } else {
              delivered_value = "[";
              for (std::size_t index = 0; index < tokens.size(); ++index) {
                if (index != 0) delivered_value += ",";
                delivered_value += "\"" + tokens[index] + "\"";
              }
              delivered_value += "]";
            }
          }
        }
        if (!runtime_engine_ ||
            !runtime_engine_->HandleSurfaceEvent(package_id, surface_id,
                                                 control_id, delivered_value)) {
          ApplySurfaceResponse(package_id, surface_id, control_id, {},
                               "Package is disabled, busy, or unavailable.");
        }
      });
  ShowAndActivateWindow(dialog.get());
  surface_dialogs_.emplace(key, std::move(dialog));
  if (runtime_engine_)
    runtime_engine_->HandleSurfaceEvent(package_id, surface.id,
                                        "surface-opened", "null");
  wxLogMessage("PPM event=surface-opened package=%s surface=%s", package_id,
               surface.id);
}

void PortablePluginManagerPi::ApplySurfaceResponse(
    const std::string& package_id, const std::string& surface_id,
    const std::string& control_id, const std::string& state_json,
    const std::string& diagnostic) {
  const auto item = surface_dialogs_.find(package_id + "\n" + surface_id);
  if (item == surface_dialogs_.end()) return;
  item->second->ApplyResponse(control_id, state_json, diagnostic);
}

void PortablePluginManagerPi::ClosePackageSurfaces(
    const std::string& package_id) {
  if (package_id == "org.opencpn.igrib" && environment_workbench_) {
    environment_workbench_->Shutdown();
    environment_workbench_.reset();
  }
  if (package_id == "org.opencpn.iweather-routing" && weather_routing_host_) {
    weather_routing_host_->Shutdown();
    weather_routing_host_.reset();
    pending_weather_route_guid_.clear();
  }
  const std::string prefix = package_id + "\n";
  for (auto item = surface_dialogs_.begin(); item != surface_dialogs_.end();) {
    if (item->first.rfind(prefix, 0) == 0)
      item = surface_dialogs_.erase(item);
    else
      ++item;
  }
}

bool PortablePluginManagerPi::PreparePermissions(const std::string& package_id,
                                                 bool interactive,
                                                 wxString* diagnostic) {
  const auto installed = package_store_->Installed();
  const auto item =
      std::find_if(installed.begin(), installed.end(),
                   [&](const auto& value) { return value.id == package_id; });
  if (item == installed.end()) {
    if (diagnostic) *diagnostic = "Package is not installed.";
    return false;
  }
  const PermissionEvaluation evaluation = permission_store_->Evaluate(*item);
  if (!evaluation.okay) {
    if (diagnostic) *diagnostic = wxString::FromUTF8(evaluation.message);
    return false;
  }
  if (!evaluation.added.empty()) {
    if (!interactive) {
      if (diagnostic)
        *diagnostic =
            "Permission approval is required before this package "
            "can run.";
      return false;
    }
    if (!ConfirmPermissionApproval(manager_dialog_.get(), *item, evaluation)) {
      if (diagnostic) *diagnostic = "Permission approval was cancelled.";
      return false;
    }
  }
  if (!evaluation.current || !evaluation.manifest_current) {
    const StoreResult granted = permission_store_->Grant(*item);
    if (!granted.okay) {
      if (diagnostic)
        *diagnostic = "Could not save permission approval: " +
                      wxString::FromUTF8(granted.message);
      return false;
    }
  }
  std::string runtime_diagnostic;
  if (!runtime_engine_->SetGrantedPermissions(package_id, item->permissions,
                                              &runtime_diagnostic)) {
    if (diagnostic) *diagnostic = wxString::FromUTF8(runtime_diagnostic);
    return false;
  }
  return true;
}

bool PortablePluginManagerPi::RestorePreviousPackage(
    const std::string& package_id, bool enable_after_restore,
    wxString* diagnostic) {
  const StoreResult rollback = package_store_->Rollback(package_id);
  if (!rollback.okay) {
    if (diagnostic)
      *diagnostic = "Rollback failed: " + wxString::FromUTF8(rollback.message);
    return false;
  }
  const StoreResult audit = package_store_->AuditInstalled(package_id);
  if (!audit.okay) {
    if (diagnostic)
      *diagnostic =
          "Previous package was restored but failed integrity "
          "verification: " +
          wxString::FromUTF8(audit.message);
    return false;
  }
  std::string runtime_diagnostic;
  if (!runtime_engine_->RefreshPackage(package_id, developer_mode_,
                                       &runtime_diagnostic)) {
    if (diagnostic)
      *diagnostic =
          "Previous package was restored on disk but could not be "
          "loaded: " +
          wxString::FromUTF8(runtime_diagnostic);
    return false;
  }
  if (enable_after_restore) {
    wxString permission_diagnostic;
    if (!PreparePermissions(package_id, false, &permission_diagnostic) ||
        !runtime_engine_->Enable(package_id, &runtime_diagnostic) ||
        !package_store_->SetEnabled(package_id, true).okay) {
      if (diagnostic)
        *diagnostic =
            "Previous package was restored but could not be "
            "re-enabled: " +
            permission_diagnostic + wxString::FromUTF8(runtime_diagnostic);
      return false;
    }
  }
  return true;
}

void PortablePluginManagerPi::InstallPackage(const std::string& archive_path) {
  if (!package_store_ || !runtime_engine_) return;
  wxBusyCursor busy;
  SetManagerStatus("Verifying package signature, manifest and contents…");
  const StoreResult inspected = package_store_->Inspect(archive_path);
  if (!inspected.okay) {
    SetManagerStatus("Package rejected: " +
                     wxString::FromUTF8(inspected.message));
    return;
  }
  const auto installed = package_store_->Installed();
  const bool replacing = std::any_of(
      installed.begin(), installed.end(),
      [&](const auto& package) { return package.id == inspected.package_id; });
  if (replacing &&
      wxMessageBox(
          "A version of " + wxString::FromUTF8(inspected.package_id) +
              " is already installed.\n\nVerify and install this package as "
              "an update? The current version will be retained for rollback.",
          "Update portable package", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION,
          manager_dialog_.get()) != wxYES) {
    SetManagerStatus("Update cancelled; no files were changed.");
    return;
  }

  const bool was_enabled =
      replacing && runtime_engine_->IsEnabled(inspected.package_id);
  if (replacing) {
    package_store_->SetEnabled(inspected.package_id, false);
    std::string ignored;
    runtime_engine_->Unload(inspected.package_id, &ignored);
  }
  const StoreResult result = package_store_->Install(archive_path, replacing);
  if (!result.okay) {
    if (replacing) {
      std::string refresh_diagnostic;
      runtime_engine_->RefreshPackage(inspected.package_id, developer_mode_,
                                      &refresh_diagnostic);
      wxString permission_diagnostic;
      if (was_enabled &&
          PreparePermissions(inspected.package_id, false,
                             &permission_diagnostic) &&
          runtime_engine_->Enable(inspected.package_id, &refresh_diagnostic)) {
        package_store_->SetEnabled(inspected.package_id, true);
      }
    }
    SetManagerStatus(
        "Installation failed without replacing the current "
        "package: " +
        wxString::FromUTF8(result.message));
    return;
  }

  const StoreResult audit = package_store_->AuditInstalled(result.package_id);
  if (!audit.okay) {
    wxString recovery;
    if (replacing)
      RestorePreviousPackage(result.package_id, was_enabled, &recovery);
    else
      package_store_->Remove(result.package_id);
    SetManagerStatus(
        "Installed package failed the independent on-disk integrity audit: " +
        wxString::FromUTF8(audit.message) +
        (recovery.empty() ? wxString() : "\n" + recovery));
    return;
  }

  std::string runtime_diagnostic;
  if (!runtime_engine_->RefreshPackage(result.package_id, developer_mode_,
                                       &runtime_diagnostic)) {
    wxString recovery;
    if (replacing) {
      RestorePreviousPackage(result.package_id, was_enabled, &recovery);
    } else {
      package_store_->Remove(result.package_id);
      runtime_engine_->RefreshPackage(result.package_id, developer_mode_,
                                      &runtime_diagnostic);
    }
    SetManagerStatus(
        "The package archive was valid, but its runtime could not be loaded: " +
        wxString::FromUTF8(runtime_diagnostic) +
        (recovery.empty() ? wxString() : "\n" + recovery));
    return;
  }

  if (was_enabled) {
    wxString permission_diagnostic;
    if (!PreparePermissions(result.package_id, true, &permission_diagnostic)) {
      SetManagerStatus("Package updated and left disabled: " +
                       permission_diagnostic);
      return;
    }
    if (!runtime_engine_->Enable(result.package_id, &runtime_diagnostic)) {
      wxString recovery;
      RestorePreviousPackage(result.package_id, true, &recovery);
      SetManagerStatus(
          "Updated runtime failed to enable; the previous "
          "version was restored. " +
          wxString::FromUTF8(runtime_diagnostic) +
          (recovery.empty() ? wxString() : "\n" + recovery));
      return;
    }
    const StoreResult persisted =
        package_store_->SetEnabled(result.package_id, true);
    if (!persisted.okay) {
      runtime_engine_->Disable(result.package_id, &runtime_diagnostic);
      SetManagerStatus(
          "Package updated but was left disabled because its "
          "state could not be saved: " +
          wxString::FromUTF8(persisted.message));
      return;
    }
  }
  RefreshManager();
  SetManagerStatus(
      wxString::FromUTF8(result.message) +
      (was_enabled ? " and re-enabled." : ". It is disabled by default."));
}

void PortablePluginManagerPi::EnablePackage(const std::string& package_id) {
  const StoreResult audit = package_store_->AuditInstalled(package_id);
  if (!audit.okay) {
    package_store_->SetEnabled(package_id, false);
    SetManagerStatus("Refusing to enable " + wxString::FromUTF8(package_id) +
                     ": installed-package integrity check failed: " +
                     wxString::FromUTF8(audit.message));
    return;
  }
  wxString permission_diagnostic;
  if (!PreparePermissions(package_id, true, &permission_diagnostic)) {
    package_store_->SetEnabled(package_id, false);
    SetManagerStatus("Package remains disabled: " + permission_diagnostic);
    return;
  }
  std::string diagnostic;
  if (!runtime_engine_->Enable(package_id, &diagnostic)) {
    package_store_->SetEnabled(package_id, false);
    SetManagerStatus("Could not enable " + wxString::FromUTF8(package_id) +
                     ": " + wxString::FromUTF8(diagnostic));
    return;
  }
  const StoreResult persisted = package_store_->SetEnabled(package_id, true);
  if (!persisted.okay) {
    runtime_engine_->Disable(package_id, &diagnostic);
    SetManagerStatus(
        "The package started, but was stopped because its "
        "enabled state could not be saved: " +
        wxString::FromUTF8(persisted.message));
    return;
  }
  RefreshManager();
  SetManagerStatus(wxString::FromUTF8(package_id) + " enabled.");
}

void PortablePluginManagerPi::DisablePackage(const std::string& package_id) {
  const StoreResult persisted = package_store_->SetEnabled(package_id, false);
  if (!persisted.okay) {
    SetManagerStatus("Could not safely disable package: " +
                     wxString::FromUTF8(persisted.message));
    return;
  }
  std::string diagnostic;
  const bool clean = runtime_engine_->Disable(package_id, &diagnostic);
  RefreshManager();
  SetManagerStatus(
      wxString::FromUTF8(package_id) +
      (clean ? " disabled; its runtime remains resident for quick restart."
             : " was forcibly stopped after its disable callback failed: " +
                   wxString::FromUTF8(diagnostic)));
}

void PortablePluginManagerPi::UnloadPackage(const std::string& package_id) {
  const StoreResult persisted = package_store_->SetEnabled(package_id, false);
  if (!persisted.okay) {
    SetManagerStatus("Could not safely unload package: " +
                     wxString::FromUTF8(persisted.message));
    return;
  }
  std::string diagnostic;
  const bool clean = runtime_engine_->Unload(package_id, &diagnostic);
  RefreshManager();
  SetManagerStatus(wxString::FromUTF8(package_id) +
                   (clean ? " unloaded; its Wasmtime memory has been released."
                          : " was forcibly unloaded after an error: " +
                                wxString::FromUTF8(diagnostic)));
}

void PortablePluginManagerPi::RemovePackage(const std::string& package_id) {
  package_store_->SetEnabled(package_id, false);
  std::string ignored;
  runtime_engine_->Unload(package_id, &ignored);
  const StoreResult removed = package_store_->Remove(package_id);
  std::string refresh_diagnostic;
  runtime_engine_->RefreshPackage(package_id, developer_mode_,
                                  &refresh_diagnostic);
  RefreshManager();
  SetManagerStatus(
      removed.okay
          ? wxString::FromUTF8(package_id) +
                " removed to recoverable storage; private data was retained."
          : "Removal failed: " + wxString::FromUTF8(removed.message));
}

void PortablePluginManagerPi::RollbackPackage(const std::string& package_id) {
  package_store_->SetEnabled(package_id, false);
  std::string ignored;
  runtime_engine_->Unload(package_id, &ignored);
  const StoreResult rolled_back = package_store_->Rollback(package_id);
  std::string runtime_diagnostic;
  const StoreResult audit = rolled_back.okay
                                ? package_store_->AuditInstalled(package_id)
                                : StoreResult{};
  const bool loaded = rolled_back.okay && audit.okay &&
                      runtime_engine_->RefreshPackage(
                          package_id, developer_mode_, &runtime_diagnostic);
  RefreshManager();
  SetManagerStatus(
      loaded ? wxString::FromUTF8(package_id) +
                   " rolled back and left disabled for review."
             : "Rollback failed: " +
                   wxString::FromUTF8(!rolled_back.okay ? rolled_back.message
                                      : !audit.okay     ? audit.message
                                                        : runtime_diagnostic));
}

void PortablePluginManagerPi::RevokePackagePermissions(
    const std::string& package_id) {
  package_store_->SetEnabled(package_id, false);
  std::string ignored;
  runtime_engine_->Unload(package_id, &ignored);
  const StoreResult revoked = permission_store_->Revoke(package_id);
  RefreshManager();
  SetManagerStatus(
      revoked.okay
          ? wxString::FromUTF8(package_id) +
                " disabled and unloaded; all stored access approval was "
                "revoked."
          : "Could not revoke package access: " +
                wxString::FromUTF8(revoked.message));
}

void PortablePluginManagerPi::SetPositionFixEx(PlugIn_Position_Fix_Ex& fix) {
  vessel_position_valid_ = std::isfinite(fix.Lat) && std::isfinite(fix.Lon) &&
                           fix.Lat >= -90.0 && fix.Lat <= 90.0 &&
                           fix.Lon >= -180.0 && fix.Lon <= 180.0;
  if (vessel_position_valid_) {
    vessel_latitude_ = fix.Lat;
    vessel_longitude_ = fix.Lon;
  }
  if (runtime_engine_) runtime_engine_->SetPositionFix(fix);
}

void PortablePluginManagerPi::SetCursorLatLon(double latitude,
                                              double longitude) {
  cursor_position_valid_ = std::isfinite(latitude) &&
                           std::isfinite(longitude) && latitude >= -90.0 &&
                           latitude <= 90.0 && longitude >= -180.0 &&
                           longitude <= 180.0;
  if (cursor_position_valid_) {
    cursor_latitude_ = latitude;
    cursor_longitude_ = longitude;
    if (environment_workbench_)
      environment_workbench_->SetCursorPosition(latitude, longitude);
    if (runtime_engine_)
      runtime_engine_->SetCursorPosition(latitude, longitude);
    if (weather_routing_host_) weather_routing_host_->CursorChanged();
  }
}

std::vector<PortableNavigationPosition> PortablePluginManagerPi::ListWaypoints()
    const {
  std::vector<PortableNavigationPosition> result;
  const wxArrayString identifiers = GetWaypointGUIDArray();
  result.reserve(std::min<std::size_t>(identifiers.size(), 50'000));
  for (std::size_t index = 0;
       index < identifiers.size() && result.size() < 50'000; ++index) {
    const auto waypoint = GetWaypoint_Plugin(identifiers[index]);
    if (!waypoint || !std::isfinite(waypoint->m_lat) ||
        !std::isfinite(waypoint->m_lon) || std::abs(waypoint->m_lat) > 90.0 ||
        std::abs(waypoint->m_lon) > 180.0) {
      continue;
    }
    result.push_back(
        {waypoint->m_GUID,
         waypoint->m_MarkName.empty() ? waypoint->m_GUID : waypoint->m_MarkName,
         waypoint->m_lat, waypoint->m_lon});
  }
  return result;
}

std::vector<PortableNavigationRoute> PortablePluginManagerPi::ListRoutes()
    const {
  std::vector<PortableNavigationRoute> result;
  const wxArrayString identifiers = GetRouteGUIDArray();
  result.reserve(std::min<std::size_t>(identifiers.size(), 10'000));
  for (std::size_t index = 0;
       index < identifiers.size() && result.size() < 10'000; ++index) {
    const auto route = GetRoute_Plugin(identifiers[index]);
    if (!route || !route->pWaypointList) continue;
    PortableNavigationRoute copied;
    copied.id = route->m_GUID;
    copied.name =
        route->m_NameString.empty() ? route->m_GUID : route->m_NameString;
    for (auto node = route->pWaypointList->GetFirst();
         node && copied.points.size() < 20'000; node = node->GetNext()) {
      const PlugIn_Waypoint* waypoint = node->GetData();
      if (!waypoint || !std::isfinite(waypoint->m_lat) ||
          !std::isfinite(waypoint->m_lon) || std::abs(waypoint->m_lat) > 90.0 ||
          std::abs(waypoint->m_lon) > 180.0) {
        continue;
      }
      copied.points.push_back({waypoint->m_GUID,
                               waypoint->m_MarkName.empty()
                                   ? waypoint->m_GUID
                                   : waypoint->m_MarkName,
                               waypoint->m_lat, waypoint->m_lon});
    }
    if (copied.points.size() >= 2) result.push_back(std::move(copied));
  }
  return result;
}

bool PortablePluginManagerPi::CreateOpenCpnRoute(
    const wxString& name, const std::vector<PortableNavigationPosition>& points,
    wxString* diagnostic) {
  if (points.size() < 2 || points.size() > 2'000) {
    if (diagnostic)
      *diagnostic = "A route must contain between 2 and 2,000 points.";
    return false;
  }
  PlugIn_Route route;
  route.m_NameString = name;
  route.m_StartString = points.front().name;
  route.m_EndString = points.back().name;
  for (const auto& point : points) {
    if (!std::isfinite(point.latitude) || !std::isfinite(point.longitude) ||
        std::abs(point.latitude) > 90.0 || std::abs(point.longitude) > 180.0) {
      if (diagnostic) *diagnostic = "The route contains an invalid position.";
      return false;
    }
    route.pWaypointList->Append(new PlugIn_Waypoint(
        point.latitude, point.longitude, "circle", point.name));
  }
  if (!AddPlugInRoute(&route, true)) {
    if (diagnostic) *diagnostic = "OpenCPN rejected the generated route.";
    return false;
  }
  return true;
}

int PortablePluginManagerPi::HandleAuthorUiRequest(
    const std::string& package_id, const std::string& operation,
    const std::string& request_json, std::string* response_json) {
  if (!response_json) return -1;
  wxJSONValue request;
  wxJSONReader reader;
  if (reader.Parse(wxString::FromUTF8(request_json), &request) != 0 ||
      !request.IsObject()) {
    return -2;
  }
  auto encode = [response_json](const wxJSONValue& value) {
    wxString encoded;
    wxJSONWriter writer;
    writer.Write(value, encoded);
    *response_json = encoded.ToStdString();
  };
  auto error = [&](const wxString& code, const wxString& message) {
    wxJSONValue value;
    value["error"]["code"] = code;
    value["error"]["message"] = message;
    value["error"]["retryable"] = false;
    encode(value);
    return -3;
  };

  if (operation == "host-environment.get") {
    wxJSONValue value;
#ifdef VERSION_FULL
    value["host_version"] = VERSION_FULL;
#else
    value["host_version"] = "unknown";
#endif
    value["locale"] = GetLocaleCanonicalName();
    value["color_scheme"] =
        colour_scheme_ == PI_GLOBAL_COLOR_SCHEME_NIGHT  ? "night"
        : colour_scheme_ == PI_GLOBAL_COLOR_SCHEME_DUSK ? "dusk"
                                                        : "day";
    wxWindow* canvas = GetOCPNCanvasWindow();
    value["display_scale"] = canvas ? canvas->GetContentScaleFactor() : 1.0;
    value["distance_unit"] = getUsrDistanceUnit_Plugin();
    value["speed_unit"] = getUsrSpeedUnit_Plugin();
    value["wind_speed_unit"] = getUsrWindSpeedUnit_Plugin();
    value["depth_unit"] = getUsrDepthUnit_Plugin();
    value["temperature_unit"] = getUsrTempUnit_Plugin();
    encode(value);
    return 0;
  }

  if (operation == "actions.set-state") {
    const ActionKey key{package_id,
                        request["action_id"].AsString().ToStdString()};
    Action* action = actions_.Find(key);
    if (!action) return error("not-found", "The command is not registered.");
    action->checked = request["checked"].AsBool();
    action->dispatchable = request["enabled"].AsBool();
    const bool visible = request["visible"].AsBool();
    if (action->tool_id >= 0) {
      SetToolbarToolViz(action->tool_id, visible);
      if (request["checkable"].AsBool())
        SetToolbarItemState(action->tool_id, action->checked);
    }
    for (const int context_id : action->context_ids) {
      SetCanvasContextMenuItemViz(context_id, visible);
      SetCanvasContextMenuItemGrey(context_id, !action->dispatchable);
    }
    encode(wxJSONValue(wxJSONTYPE_OBJECT));
    return 0;
  }
  if (operation == "actions.unregister") {
    const ActionKey key{package_id,
                        request["action_id"].AsString().ToStdString()};
    const Action* found = actions_.Find(key);
    if (!found) return error("not-found", "The command is not registered.");
    const Action action = *found;
    if (action.tool_id >= 0) RemovePlugInTool(action.tool_id);
    for (const int context_id : action.context_ids)
      RemoveCanvasContextMenuItem(context_id);
    actions_.Remove(key);
    encode(wxJSONValue(wxJSONTYPE_OBJECT));
    return 0;
  }

  auto point_json = [](const PlugIn_Waypoint& point) {
    wxJSONValue value;
    value["id"] = point.m_GUID;
    value["name"] = point.m_MarkName;
    value["latitude"] = point.m_lat;
    value["longitude"] = point.m_lon;
    value["description"] = point.m_MarkDescription;
    value["icon_name"] = point.m_IconName.empty()
                             ? wxJSONValue(wxJSONTYPE_NULL)
                             : wxJSONValue(point.m_IconName);
    value["visible"] = point.m_IsVisible;
    if (point.m_CreateTime.IsValid())
      value["unix_time"] =
          static_cast<wxLongLong_t>(point.m_CreateTime.GetTicks());
    else
      value["unix_time"] = wxJSONValue(wxJSONTYPE_NULL);
    return value;
  };
  auto append_points = [&point_json](Plugin_WaypointList* points,
                                     wxJSONValue* destination) {
    if (!points || !destination) return;
    for (auto node = points->GetFirst(); node && destination->Size() < 20'000;
         node = node->GetNext()) {
      const PlugIn_Waypoint* point = node->GetData();
      if (point && std::isfinite(point->m_lat) && std::isfinite(point->m_lon) &&
          std::abs(point->m_lat) <= 90.0 && std::abs(point->m_lon) <= 180.0) {
        destination->Append(point_json(*point));
      }
    }
  };
  auto waypoint_object = [&point_json](const PlugIn_Waypoint& point) {
    wxJSONValue value;
    value["id"] = point.m_GUID;
    value["kind"] = "waypoint";
    value["name"] = point.m_MarkName;
    value["description"] = point.m_MarkDescription;
    value["points"].Append(point_json(point));
    value["visible"] = point.m_IsVisible;
    value["active"] = GetActiveWaypointGUID() == point.m_GUID;
    value["revision"] = NavigationRevision(value);
    return value;
  };
  auto route_object = [&append_points](const PlugIn_Route& route) {
    wxJSONValue value;
    value["id"] = route.m_GUID;
    value["kind"] = "route";
    value["name"] = route.m_NameString;
    value["description"] = "";
    append_points(route.pWaypointList, &value["points"]);
    value["visible"] = true;
    value["active"] = GetActiveRouteGUID() == route.m_GUID;
    value["revision"] = NavigationRevision(value);
    return value;
  };
  auto track_object = [&append_points](const PlugIn_Track& track) {
    wxJSONValue value;
    value["id"] = track.m_GUID;
    value["kind"] = "track";
    value["name"] = track.m_NameString;
    value["description"] = "";
    append_points(track.pWaypointList, &value["points"]);
    value["visible"] = true;
    value["active"] = false;
    value["revision"] = NavigationRevision(value);
    return value;
  };

  if (operation == "navigation.list" || operation == "navigation.list-page" ||
      operation == "navigation.get") {
    const wxString kind = request["kind"].AsString();
    const wxString requested_id = request["id"].AsString();
    const bool paged = operation == "navigation.list-page";
    if (operation == "navigation.get" && requested_id.empty())
      return error("invalid-id", "A navigation object identifier is required.");
    const std::size_t limit =
        operation == "navigation.get"
            ? 1
            : std::clamp<std::size_t>(
                  static_cast<std::size_t>(request["limit"].AsLong()), 1,
                  10'000);
    std::size_t offset = 0;
    if (paged && request["cursor"].IsString()) {
      const wxString cursor = request["cursor"].AsString();
      const wxString prefix = "opp-nav-v1:" + kind + ":";
      wxULongLong_t parsed = 0;
      if (!cursor.StartsWith(prefix) ||
          !cursor.Mid(prefix.length()).ToULongLong(&parsed) ||
          parsed > 1'000'000) {
        return error("invalid-cursor",
                     "The navigation cursor is invalid or expired.");
      }
      offset = static_cast<std::size_t>(parsed);
    }
    wxJSONValue result(operation == "navigation.get" ? wxJSONTYPE_NULL
                                                     : wxJSONTYPE_ARRAY);
    std::size_t seen = 0;
    bool has_more = false;
    auto accept = [&](const wxString& id, wxJSONValue value) {
      if (operation == "navigation.get") {
        if (id == requested_id) result = std::move(value);
      } else if (seen++ < offset) {
        return;
      } else if (static_cast<std::size_t>(result.Size()) < limit) {
        result.Append(std::move(value));
      } else {
        has_more = true;
      }
    };
    if (kind == "waypoint") {
      const wxArrayString ids = GetWaypointGUIDArray();
      for (const auto& id : ids) {
        if (operation == "navigation.get" && id != requested_id) continue;
        const auto point = GetWaypoint_Plugin(id);
        if (point) accept(id, waypoint_object(*point));
        if (operation == "navigation.get" && !result.IsNull()) break;
        if ((paged && has_more) ||
            (!paged && static_cast<std::size_t>(result.Size()) >= limit))
          break;
      }
    } else if (kind == "route") {
      const wxArrayString ids = GetRouteGUIDArray();
      for (const auto& id : ids) {
        if (operation == "navigation.get" && id != requested_id) continue;
        const auto route = GetRoute_Plugin(id);
        if (route) accept(id, route_object(*route));
        if (operation == "navigation.get" && !result.IsNull()) break;
        if ((paged && has_more) ||
            (!paged && static_cast<std::size_t>(result.Size()) >= limit))
          break;
      }
    } else if (kind == "track") {
      const wxArrayString ids = GetTrackGUIDArray();
      for (const auto& id : ids) {
        if (operation == "navigation.get" && id != requested_id) continue;
        const auto track = GetTrack_Plugin(id);
        if (track) accept(id, track_object(*track));
        if (operation == "navigation.get" && !result.IsNull()) break;
        if ((paged && has_more) ||
            (!paged && static_cast<std::size_t>(result.Size()) >= limit))
          break;
      }
    } else {
      return error("invalid-kind", "Unknown navigation object kind.");
    }
    if (paged) {
      wxJSONValue page;
      page["objects"] = result;
      page["next_cursor"] =
          has_more
              ? wxJSONValue(wxString::Format(
                    "opp-nav-v1:%s:%llu", kind,
                    static_cast<unsigned long long>(offset + result.Size())))
              : wxJSONValue(wxJSONTYPE_NULL);
      encode(page);
    } else {
      encode(result);
    }
    return 0;
  }

  if (operation == "navigation.mutate") {
    const wxString mutation = request["operation"].AsString();
    const wxString kind = mutation == "delete"
                              ? request["kind"].AsString()
                              : request["object"]["kind"].AsString();
    const wxString id = mutation == "delete"
                            ? request["id"].AsString()
                            : request["object"]["id"].AsString();
    const wxString name =
        mutation == "delete" ? id : request["object"]["name"].AsString();
    if ((mutation != "create" && mutation != "update" &&
         mutation != "delete") ||
        (kind != "waypoint" && kind != "route" && kind != "track") ||
        (mutation != "create" && id.empty())) {
      return error("invalid-mutation",
                   "The navigation object mutation is invalid.");
    }
    if (mutation == "update") {
      const wxString expected_revision =
          request["object"]["revision"].AsString();
      wxString current_revision;
      if (kind == "waypoint") {
        const auto current = GetWaypoint_Plugin(id);
        if (current)
          current_revision = waypoint_object(*current)["revision"].AsString();
      } else if (kind == "route") {
        const auto current = GetRoute_Plugin(id);
        if (current)
          current_revision = route_object(*current)["revision"].AsString();
      } else {
        const auto current = GetTrack_Plugin(id);
        if (current)
          current_revision = track_object(*current)["revision"].AsString();
      }
      if (current_revision.empty())
        return error("not-found", "The navigation object no longer exists.");
      if (expected_revision.empty() || expected_revision != current_revision)
        return error(
            "revision-conflict",
            "The navigation object changed since it was read; refresh it "
            "before applying this edit.");
    }
    const wxString prompt = wxString::Format(
        "Portable plugin “%s” requests permission to %s the %s “%s”.\n\n"
        "Apply this change to OpenCPN?",
        wxString::FromUTF8(package_id), wxString::FromUTF8(mutation), kind,
        name);
    if (wxMessageBox(prompt, "Confirm navigation change",
                     wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
                     GetOCPNCanvasWindow()) != wxYES) {
      return error("user-declined",
                   "The user declined the navigation object change.");
    }

    bool changed = false;
    wxString result_id = id;
    if (mutation == "delete") {
      wxString mutable_id = id;
      if (kind == "waypoint")
        changed = DeleteSingleWaypoint(mutable_id);
      else if (kind == "route")
        changed = DeletePlugInRoute(mutable_id);
      else
        changed = DeletePlugInTrack(mutable_id);
    } else {
      wxJSONValue object = request["object"];
      wxJSONValue points = object["points"];
      if (!points.IsArray() || points.Size() == 0 || points.Size() > 20'000) {
        return error("invalid-points",
                     "The navigation object has an invalid point list.");
      }
      auto make_point = [](wxJSONValue value) {
        const double latitude = value["latitude"].AsDouble();
        const double longitude = value["longitude"].AsDouble();
        if (!std::isfinite(latitude) || !std::isfinite(longitude) ||
            std::abs(latitude) > 90.0 || std::abs(longitude) > 180.0) {
          return std::unique_ptr<PlugIn_Waypoint>{};
        }
        auto point = std::make_unique<PlugIn_Waypoint>(
            latitude, longitude,
            value["icon_name"].IsString() ? value["icon_name"].AsString()
                                          : "circle",
            value["name"].AsString(), value["id"].AsString());
        point->m_MarkDescription = value["description"].AsString();
        point->m_IsVisible =
            !value.HasMember("visible") || value["visible"].AsBool();
        if (value["unix_time"].IsInt() || value["unix_time"].IsUInt() ||
            value["unix_time"].IsLong()) {
          point->m_CreateTime.Set(
              static_cast<time_t>(value["unix_time"].AsLong()));
        }
        return point;
      };
      if (kind == "waypoint") {
        if (points.Size() != 1)
          return error("invalid-points",
                       "A waypoint must contain exactly one point.");
        auto point = make_point(points[0]);
        if (!point)
          return error("invalid-points", "Waypoint position is invalid.");
        point->m_GUID = id;
        changed = mutation == "create" ? AddSingleWaypoint(point.get(), true)
                                       : UpdateSingleWaypoint(point.get());
        result_id = point->m_GUID;
      } else if (kind == "route") {
        if (points.Size() < 2)
          return error("invalid-points",
                       "A route must contain at least two points.");
        PlugIn_Route route;
        route.m_GUID = id;
        route.m_NameString = object["name"].AsString();
        for (int index = 0; index < points.Size(); ++index) {
          auto point = make_point(points[index]);
          if (!point)
            return error("invalid-points", "Route position is invalid.");
          route.pWaypointList->Append(point.release());
        }
        route.m_StartString =
            route.pWaypointList->GetFirst()->GetData()->m_MarkName;
        route.m_EndString =
            route.pWaypointList->GetLast()->GetData()->m_MarkName;
        changed = mutation == "create" ? AddPlugInRoute(&route, true)
                                       : UpdatePlugInRoute(&route);
        result_id = route.m_GUID;
      } else {
        PlugIn_Track track;
        track.m_GUID = id;
        track.m_NameString = object["name"].AsString();
        for (int index = 0; index < points.Size(); ++index) {
          auto point = make_point(points[index]);
          if (!point)
            return error("invalid-points", "Track position is invalid.");
          track.pWaypointList->Append(point.release());
        }
        changed = mutation == "create" ? AddPlugInTrack(&track, true)
                                       : UpdatePlugInTrack(&track);
        result_id = track.m_GUID;
      }
    }
    if (!changed)
      return error("opencpn-rejected",
                   "OpenCPN rejected the navigation object change.");
    wxJSONValue response;
    response["id"] = result_id;
    wxString result_revision;
    if (mutation != "delete") {
      if (kind == "waypoint") {
        const auto current = GetWaypoint_Plugin(result_id);
        if (current)
          result_revision = waypoint_object(*current)["revision"].AsString();
      } else if (kind == "route") {
        const auto current = GetRoute_Plugin(result_id);
        if (current)
          result_revision = route_object(*current)["revision"].AsString();
      } else {
        const auto current = GetTrack_Plugin(result_id);
        if (current)
          result_revision = track_object(*current)["revision"].AsString();
      }
    }
    response["revision"] = result_revision;
    response["diagnostic"] = "OpenCPN accepted the user-confirmed change.";
    encode(response);
    return 0;
  }

  if (operation == "navigation.send-nmea0183") {
    wxString sentence = request["sentence"].AsString();
    sentence.Replace("\r", "");
    sentence.Replace("\n", "");
    if (sentence.length() < 2 || sentence.length() > 1021 ||
        (sentence[0] != '$' && sentence[0] != '!'))
      return error("invalid-sentence", "The NMEA 0183 sentence is invalid.");
    const int marker = sentence.Find('*');
    unsigned char checksum = 0;
    const int checksum_end =
        marker == wxNOT_FOUND ? static_cast<int>(sentence.length()) : marker;
    for (int index = 1; index < checksum_end; ++index)
      checksum ^= static_cast<unsigned char>(sentence[index].GetValue());
    const wxString expected = wxString::Format("%02X", checksum);
    if (marker == wxNOT_FOUND) {
      sentence += "*" + expected;
    } else if (marker + 2 >= static_cast<int>(sentence.length()) ||
               sentence.Mid(marker + 1, 2).Upper() != expected) {
      return error("invalid-checksum", "The NMEA 0183 checksum is invalid.");
    }
    const auto now = std::chrono::steady_clock::now();
    auto& history = nmea_output_history_[package_id];
    while (!history.empty() && now - history.front() > std::chrono::seconds(10))
      history.pop_front();
    if (history.size() >= 50)
      return error("rate-limited",
                   "The package exceeded the NMEA output rate limit.");
    history.push_back(now);
    sentence += "\r\n";
    PushNMEABuffer(sentence);
    encode(wxJSONValue(wxJSONTYPE_OBJECT));
    return 0;
  }

  if (operation == "communications.list-outputs") {
    const std::string package_prefix = package_id + "\n";
    for (auto item = communication_endpoints_.begin();
         item != communication_endpoints_.end();) {
      if (item->first.rfind(package_prefix, 0) == 0)
        item = communication_endpoints_.erase(item);
      else
        ++item;
    }
    wxJSONValue endpoints(wxJSONTYPE_ARRAY);
    std::size_t sequence = 0;
    for (const auto& handle : GetActiveDrivers()) {
      const auto attributes = GetAttributes(handle);
      const auto protocol_item = attributes.find("protocol");
      if (protocol_item == attributes.end()) continue;
      std::string protocol = protocol_item->second;
      std::transform(protocol.begin(), protocol.end(), protocol.begin(),
                     [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                     });
      if (protocol != "nmea0183" && protocol != "nmea2000" &&
          protocol != "internal")
        continue;
      const std::string token = "output-" + std::to_string(++sequence);
      communication_endpoints_[package_prefix + token] = handle;
      wxJSONValue endpoint;
      endpoint["id"] = token;
      endpoint["protocol"] = protocol;
      const auto label = attributes.find("name");
      endpoint["label"] =
          label != attributes.end()
              ? wxString::FromUTF8(label->second)
              : wxString::Format("%s output %zu", protocol, sequence);
      endpoints.Append(endpoint);
    }
    encode(endpoints);
    return 0;
  }

  if (operation == "navigation.send-nmea2000") {
    static const std::set<int> safe_informational_pgns{
        126992, 127250, 127251, 127257, 127258, 128259, 128267,
        129025, 129026, 129029, 129033, 129283, 129284, 130306,
        130310, 130311, 130312, 130313, 130314, 130316};
    const std::string endpoint =
        request["endpoint_id"].AsString().ToStdString();
    const long pgn = request["pgn"].AsLong();
    const long destination = request["destination"].AsLong();
    const long priority = request["priority"].AsLong();
    const wxMemoryBuffer decoded = wxBase64Decode(
        request["payload_base64"].AsString(), wxBase64DecodeMode_Strict);
    if (safe_informational_pgns.count(static_cast<int>(pgn)) == 0)
      return error("unsafe-pgn",
                   "This NMEA 2000 PGN is not in the informational allowlist.");
    if (destination < 0 || destination > 255 || priority < 0 || priority > 7 ||
        decoded.GetDataLen() == 0 || decoded.GetDataLen() > 223)
      return error(
          "invalid-message",
          "The NMEA 2000 destination, priority or payload is invalid.");
    const std::string endpoint_key = package_id + "\n" + endpoint;
    const auto mapped = communication_endpoints_.find(endpoint_key);
    if (mapped == communication_endpoints_.end())
      return error("invalid-endpoint",
                   "Refresh the communication output list before sending.");
    const auto active = GetActiveDrivers();
    if (std::find(active.begin(), active.end(), mapped->second) == active.end())
      return error("endpoint-unavailable",
                   "The selected communication output is no longer active.");
    const auto attributes = GetAttributes(mapped->second);
    const auto protocol = attributes.find("protocol");
    std::string normalized_protocol =
        protocol == attributes.end() ? std::string() : protocol->second;
    std::transform(normalized_protocol.begin(), normalized_protocol.end(),
                   normalized_protocol.begin(), [](unsigned char value) {
                     return static_cast<char>(std::tolower(value));
                   });
    if (normalized_protocol != "nmea2000")
      return error("wrong-protocol",
                   "The selected output is not an NMEA 2000 connection.");
    const auto now = std::chrono::steady_clock::now();
    auto& history = nmea2000_output_history_[package_id];
    while (!history.empty() && now - history.front() > std::chrono::seconds(1))
      history.pop_front();
    if (history.size() >= 20)
      return error("rate-limited",
                   "The package exceeded the NMEA 2000 output rate limit.");
    auto& registered = registered_nmea2000_pgns_[mapped->second];
    if (registered.insert(static_cast<int>(pgn)).second) {
      std::vector<int> pgns(registered.begin(), registered.end());
      if (RegisterTXPGNs(mapped->second, pgns) != RESULT_COMM_NO_ERROR) {
        registered.erase(static_cast<int>(pgn));
        return error("registration-failed",
                     "OpenCPN could not register the transmit PGN.");
      }
    }
    auto payload = std::make_shared<std::vector<std::uint8_t>>(
        static_cast<const std::uint8_t*>(decoded.GetData()),
        static_cast<const std::uint8_t*>(decoded.GetData()) +
            decoded.GetDataLen());
    if (WriteCommDriverN2K(mapped->second, static_cast<int>(pgn),
                           static_cast<int>(destination),
                           static_cast<int>(priority),
                           payload) != RESULT_COMM_NO_ERROR)
      return error("transmit-failed",
                   "OpenCPN rejected the NMEA 2000 transmission.");
    history.push_back(now);
    encode(wxJSONValue(wxJSONTYPE_OBJECT));
    return 0;
  }

  return error("unsupported-operation",
               "This OpenCPN author operation is unavailable.");
}

void PortablePluginManagerPi::SetNMEASentence(wxString& sentence) {
  if (!runtime_engine_) return;
  const wxScopedCharBuffer value = sentence.utf8_str();
  if (value)
    runtime_engine_->DeliverNavigationSentence(
        std::string(value.data(), value.length()));
}

void PortablePluginManagerPi::SetAISSentence(wxString& sentence) {
  if (!runtime_engine_) return;
  const wxScopedCharBuffer value = sentence.utf8_str();
  if (value)
    runtime_engine_->DeliverAisSentence(
        std::string(value.data(), value.length()));
}

void PortablePluginManagerPi::HandleNmea2000(std::uint32_t pgn,
                                             ObservedEvt event) {
  if (!runtime_engine_) return;
  const NMEA2000Id id(static_cast<int>(pgn));
  runtime_engine_->DeliverNmea2000(pgn, GetN2000Source(id, event),
                                   GetN2000Payload(id, event));
}

void PortablePluginManagerPi::SetActiveLegInfo(
    Plugin_Active_Leg_Info& leg_info) {
  if (!runtime_engine_) return;
  const wxScopedCharBuffer name = leg_info.wp_name.utf8_str();
  runtime_engine_->SetActiveLeg(
      leg_info.Xte, leg_info.Btw, leg_info.Dtw,
      name ? std::string(name.data(), name.length()) : std::string(),
      leg_info.arrival);
}

void PortablePluginManagerPi::RefreshSceneHitRegions(PlugIn_ViewPort* viewport,
                                                     int canvas_index) {
  scene_hit_regions_.erase(
      std::remove_if(scene_hit_regions_.begin(), scene_hit_regions_.end(),
                     [&](const SceneHitRegion& value) {
                       return value.canvas_index == canvas_index;
                     }),
      scene_hit_regions_.end());
  if (!runtime_engine_ || !viewport) return;
  constexpr int kHitSlop = 6;
  for (const auto& scene : runtime_engine_->Scenes()) {
    const bool targets_canvas =
        scene.canvas_target == "all" ||
        (scene.canvas_target == "primary" && canvas_index == 0) ||
        (scene.canvas_target == "selected" &&
         std::find(scene.selected_canvases.begin(),
                   scene.selected_canvases.end(),
                   static_cast<std::uint32_t>(std::max(0, canvas_index))) !=
             scene.selected_canvases.end());
    if (!targets_canvas) continue;
    for (const auto& layer : scene.layers) {
      if (!layer.visible) continue;
      for (const auto& primitive : layer.primitives) {
        if (!primitive.interactive) continue;
        SceneHitRegion region;
        region.package_id = scene.package_id;
        region.scene_id = scene.scene_id;
        region.primitive_id = primitive.primitive_id;
        region.canvas_index = canvas_index;
        region.z_index = layer.z_index;
        int left = std::numeric_limits<int>::max();
        int top = std::numeric_limits<int>::max();
        int right = std::numeric_limits<int>::min();
        int bottom = std::numeric_limits<int>::min();
        const auto include = [&](const wxPoint& point) {
          left = std::min(left, point.x);
          top = std::min(top, point.y);
          right = std::max(right, point.x);
          bottom = std::max(bottom, point.y);
        };
        if (primitive.kind == OverlayPrimitiveKind::kPolyline ||
            primitive.kind == OverlayPrimitiveKind::kPolygon) {
          for (const auto& value : primitive.points) {
            wxPoint point;
            GetCanvasPixLL(viewport, &point, value.latitude, value.longitude);
            include(point);
          }
        } else {
          wxPoint centre;
          GetCanvasPixLL(viewport, &centre, primitive.centre.latitude,
                         primitive.centre.longitude);
          include(centre);
          int horizontal = kHitSlop;
          int vertical = kHitSlop;
          if (primitive.kind == OverlayPrimitiveKind::kCircle) {
            wxPoint edge;
            GetCanvasPixLL(
                viewport, &edge,
                std::min(90.0, primitive.centre.latitude +
                                   primitive.radius_metres / 111'320.0),
                primitive.centre.longitude);
            horizontal = vertical =
                std::max(kHitSlop, static_cast<int>(std::lround(std::hypot(
                                       edge.x - centre.x, edge.y - centre.y))));
          } else if (primitive.kind == OverlayPrimitiveKind::kIcon) {
            horizontal =
                static_cast<int>(std::ceil(primitive.width_pixels / 2.0F));
            vertical =
                static_cast<int>(std::ceil(primitive.height_pixels / 2.0F));
          } else if (primitive.kind == OverlayPrimitiveKind::kText) {
            horizontal = static_cast<int>(std::ceil(
                primitive.size_pixels * 0.6F * primitive.text.size()));
            vertical = static_cast<int>(std::ceil(primitive.size_pixels));
          }
          left -= horizontal;
          right += horizontal;
          top -= vertical;
          bottom += vertical;
        }
        if (left > right || top > bottom) continue;
        region.left = left - kHitSlop;
        region.top = top - kHitSlop;
        region.right = right + kHitSlop;
        region.bottom = bottom + kHitSlop;
        scene_hit_regions_.push_back(std::move(region));
      }
    }
  }
  std::stable_sort(scene_hit_regions_.begin(), scene_hit_regions_.end(),
                   [](const SceneHitRegion& left, const SceneHitRegion& right) {
                     return left.z_index > right.z_index;
                   });
}

bool PortablePluginManagerPi::MouseEventHook(wxMouseEvent& event) {
  if (!runtime_engine_) return false;
  std::uint32_t kind = 0;
  if (event.LeftDown() || event.MiddleDown() || event.RightDown())
    kind = 1;
  else if (event.LeftUp() || event.MiddleUp() || event.RightUp())
    kind = 2;
  else if (event.LeftDClick() || event.MiddleDClick() || event.RightDClick())
    kind = 3;
  else if (event.GetWheelRotation() != 0)
    kind = 4;
  std::uint32_t button = 0;
  if (event.LeftDown() || event.LeftUp() || event.LeftDClick())
    button = 1;
  else if (event.MiddleDown() || event.MiddleUp() || event.MiddleDClick())
    button = 2;
  else if (event.RightDown() || event.RightUp() || event.RightDClick())
    button = 3;
  std::uint32_t modifiers = 0;
  if (event.ShiftDown()) modifiers |= 1;
  if (event.ControlDown()) modifiers |= 2;
  if (event.AltDown()) modifiers |= 4;
  if (event.MetaDown()) modifiers |= 8;
  const int canvas_index = std::max(0, GetCanvasIndexUnderMouse());
  std::string hit_package;
  std::string hit_scene;
  std::string hit_primitive;
  for (const auto& region : scene_hit_regions_) {
    if (region.canvas_index == canvas_index && event.GetX() >= region.left &&
        event.GetX() <= region.right && event.GetY() >= region.top &&
        event.GetY() <= region.bottom) {
      hit_package = region.package_id;
      hit_scene = region.scene_id;
      hit_primitive = region.primitive_id;
      break;
    }
  }
  return runtime_engine_->DeliverPointerEvent(
      kind, button, static_cast<std::uint32_t>(canvas_index), event.GetX(),
      event.GetY(), cursor_latitude_, cursor_longitude_, cursor_position_valid_,
      event.GetWheelRotation(), modifiers, hit_package, hit_scene,
      hit_primitive);
}

bool PortablePluginManagerPi::KeyboardEventHook(wxKeyEvent& event) {
  if (!runtime_engine_) return false;
  std::uint32_t modifiers = 0;
  if (event.ShiftDown()) modifiers |= 1;
  if (event.ControlDown()) modifiers |= 2;
  if (event.AltDown()) modifiers |= 4;
  if (event.MetaDown()) modifiers |= 8;
  const int unicode = event.GetUnicodeKey();
  return runtime_engine_->DeliverKeyEvent(
      static_cast<std::uint32_t>(std::max(0, event.GetKeyCode())),
      unicode == WXK_NONE ? 0U : static_cast<std::uint32_t>(unicode),
      unicode != WXK_NONE, event.GetEventType() != wxEVT_KEY_UP, false,
      modifiers);
}

void PortablePluginManagerPi::SetPluginMessage(wxString& message_id,
                                               wxString& message_body) {
  if (!runtime_engine_) return;
  const wxScopedCharBuffer id = message_id.utf8_str();
  const wxScopedCharBuffer body = message_body.utf8_str();
  if (!id || !body) return;
  if (message_id == "OCPN_CORE_SIGNALK")
    runtime_engine_->DeliverSignalK(std::string(body.data(), body.length()));
  runtime_engine_->DeliverPluginMessage(
      std::string(id.data(), id.length()),
      std::string(body.data(), body.length()));
}

void PortablePluginManagerPi::SetColorScheme(PI_ColorScheme scheme) {
  colour_scheme_ = scheme;
  if (weather_routing_host_)
    weather_routing_host_->SetColorScheme(static_cast<int>(scheme));
  PublishHostEnvironment();
  RequestRefresh(GetOCPNCanvasWindow());
}

void PortablePluginManagerPi::PublishHostEnvironment() {
  if (!runtime_engine_) return;
  wxJSONValue value;
#ifdef VERSION_FULL
  value["host_version"] = VERSION_FULL;
#else
  value["host_version"] = "unknown";
#endif
  value["locale"] = GetLocaleCanonicalName();
  value["color_scheme"] =
      colour_scheme_ == PI_GLOBAL_COLOR_SCHEME_NIGHT  ? "night"
      : colour_scheme_ == PI_GLOBAL_COLOR_SCHEME_DUSK ? "dusk"
                                                      : "day";
  wxWindow* canvas = GetOCPNCanvasWindow();
  value["display_scale"] = canvas ? canvas->GetContentScaleFactor() : 1.0;
  value["distance_unit"] = getUsrDistanceUnit_Plugin();
  value["speed_unit"] = getUsrSpeedUnit_Plugin();
  value["wind_speed_unit"] = getUsrWindSpeedUnit_Plugin();
  value["depth_unit"] = getUsrDepthUnit_Plugin();
  value["temperature_unit"] = getUsrTempUnit_Plugin();
  wxString encoded;
  wxJSONWriter writer(wxJSONWRITER_NONE);
  writer.Write(value, encoded);
  runtime_engine_->DeliverHostEnvironment(encoded.ToStdString());
}

bool PortablePluginManagerPi::RenderOverlayMultiCanvas(
    wxDC& dc, PlugIn_ViewPort* viewport, int canvas_index, int priority) {
  if (!runtime_engine_ || !viewport || priority < 0 || priority > 2)
    return false;
  view_bounds_valid_ = viewport->bValid && std::isfinite(viewport->lon_min) &&
                       std::isfinite(viewport->lat_min) &&
                       std::isfinite(viewport->lon_max) &&
                       std::isfinite(viewport->lat_max) &&
                       viewport->lon_min < viewport->lon_max &&
                       viewport->lat_min < viewport->lat_max;
  if (view_bounds_valid_) {
    view_west_ = viewport->lon_min;
    view_south_ = viewport->lat_min;
    view_east_ = viewport->lon_max;
    view_north_ = viewport->lat_max;
    runtime_engine_->SetViewport(view_west_, view_south_, view_east_,
                                 view_north_, viewport->view_scale_ppm,
                                 viewport->rotation, canvas_index);
  }
  bool rendered = priority == 0 && weather_routing_host_ &&
                  weather_routing_host_->Render(dc, viewport);
  const bool environment_rendered =
      priority == 0 && environment_workbench_ &&
      environment_workbench_->Render(dc, viewport);
  rendered = environment_rendered || rendered;
  const auto scenes = runtime_engine_->Scenes();
  rendered =
      RenderPortableScenes(scenes, dc, viewport, canvas_index, priority) ||
      rendered;
  RefreshSceneHitRegions(viewport, canvas_index);
  if (developer_mode_ && environment_rendered &&
      !developer_software_overlay_logged_) {
    wxString dataset_error;
    if (environment_workbench_->AcquireDataset(&dataset_error)) {
      developer_software_overlay_logged_ = true;
      wxLogMessage(
          "PPM event=environment-overlay-rendered path=wxdc dataset=ready "
          "renderer-compatible=software,vulkan");
    }
  }
  return rendered;
}

bool PortablePluginManagerPi::RenderGLOverlayMultiCanvas(
    wxGLContext*, PlugIn_ViewPort* viewport, int canvas_index, int priority) {
  if (!runtime_engine_ || !viewport || priority < 0 || priority > 2)
    return false;
  view_bounds_valid_ = viewport->bValid && std::isfinite(viewport->lon_min) &&
                       std::isfinite(viewport->lat_min) &&
                       std::isfinite(viewport->lon_max) &&
                       std::isfinite(viewport->lat_max) &&
                       viewport->lon_min < viewport->lon_max &&
                       viewport->lat_min < viewport->lat_max;
  if (view_bounds_valid_) {
    view_west_ = viewport->lon_min;
    view_south_ = viewport->lat_min;
    view_east_ = viewport->lon_max;
    view_north_ = viewport->lat_max;
    runtime_engine_->SetViewport(view_west_, view_south_, view_east_,
                                 view_north_, viewport->view_scale_ppm,
                                 viewport->rotation, canvas_index);
  }
  bool rendered = priority == 0 && weather_routing_host_ &&
                  weather_routing_host_->RenderGL(viewport);
  const bool environment_rendered =
      priority == 0 && environment_workbench_ &&
      RenderEnvironmentWithDesktopGl(environment_workbench_.get(), viewport);
  rendered = environment_rendered || rendered;
  const auto scenes = runtime_engine_->Scenes();
  rendered = RenderPortableScenesWithDesktopGl(scenes, viewport, canvas_index,
                                               priority) ||
             rendered;
  RefreshSceneHitRegions(viewport, canvas_index);
  if (developer_mode_ && environment_rendered &&
      !developer_opengl_overlay_logged_) {
    wxString dataset_error;
    if (environment_workbench_->AcquireDataset(&dataset_error)) {
      developer_opengl_overlay_logged_ = true;
      wxLogMessage(
          "PPM event=environment-overlay-rendered "
          "path=opengl-compatibility dataset=ready");
    }
  }
  return rendered;
}

void PortablePluginManagerPi::OnEngineStateChanged() {
  if (package_store_ && runtime_engine_) {
    for (const auto& package : runtime_engine_->Packages()) {
      if (package.state == "Failed")
        package_store_->SetEnabled(package.id, false);
    }
  }
  RefreshManager();
  RequestRefresh(GetOCPNCanvasWindow());
}

void PortablePluginManagerPi::RefreshManager() {
  if (manager_dialog_ && runtime_engine_) {
    auto packages = runtime_engine_->Packages();
    const auto installed = package_store_->Installed();
    for (auto& package : packages) {
      const auto item = std::find_if(
          installed.begin(), installed.end(),
          [&](const auto& value) { return value.id == package.id; });
      if (item == installed.end()) {
        package.access = "Unknown";
        continue;
      }
      const PermissionEvaluation access = permission_store_->Evaluate(*item);
      package.access = !access.okay           ? "Blocked"
                       : access.current       ? "Approved"
                       : access.added.empty() ? "Access reduced"
                                              : "Approval required";
    }
    manager_dialog_->SetPackages(packages);
    const auto failures = std::count_if(
        packages.begin(), packages.end(),
        [](const auto& value) { return value.state == "Failed"; });
    manager_dialog_->SetStatus(
        failures == 0
            ? wxString::Format("%zu installed package%s.", packages.size(),
                               packages.size() == 1 ? "" : "s")
            : wxString::Format("%zu installed package%s; %zu "
                               "requires attention.",
                               packages.size(), packages.size() == 1 ? "" : "s",
                               failures));
  }
  if (runtime_engine_) {
    for (const auto& package : runtime_engine_->Packages()) {
      if (package.state != "Enabled") ClosePackageSurfaces(package.id);
    }
  }
}

}  // namespace ppm
