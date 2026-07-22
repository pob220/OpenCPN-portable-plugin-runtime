#include "runtime_host_toolbar_probe_pi.h"

#include <wx/filename.h>
#include <wx/log.h>
#include <wx/utils.h>

#if defined(__WXMSW__)
#include <windows.h>
#endif
#if defined(__WXOSX__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#ifdef RUNTIME_HOST_PROBE_WITH_WASMTIME
#include "runtime_bridge_probe.h"
#endif

#ifndef DECL_EXP
#ifdef __WXMSW__
#define DECL_EXP __declspec(dllexport)
#else
#define DECL_EXP __attribute__((visibility("default")))
#endif
#endif

namespace {
const char* const kPluginXpm[] = {
    "16 16 2 1", "  c None", ". c #34495e", "     ......     ",
    "   ..........   ", "  ...      ...  ", " ...   ..   ... ",
    " ..   ....   .. ", "..    ....    ..", "..  ........  ..",
    "..  ........  ..", "..    ....    ..", " ..   ....   .. ",
    " ...   ..   ... ", "  ...      ...  ", "   ..........   ",
    "     ......     ", "                ", "                "};

const LogicalActionKey kManager{"org.opencpn.portable-runtime", "manager"};
const LogicalActionKey kIgrib{"org.opencpn.igrib", "open-main-window"};
const LogicalActionKey kWeatherRouting{"org.opencpn.iweather-routing",
                                       "open-main-window"};

wxString Icon(const char* name) {
  return wxFileName(wxString::FromUTF8(EXPERIMENT_ICON_DIR),
                    wxString::FromUTF8(name))
      .GetFullPath();
}
}  // namespace

extern "C" DECL_EXP opencpn_plugin* create_pi(void* manager) {
  return new RuntimeHostToolbarProbePi(manager);
}

extern "C" DECL_EXP void destroy_pi(opencpn_plugin* plugin) { delete plugin; }

RuntimeHostToolbarProbePi::RuntimeHostToolbarProbePi(void* manager)
    : opencpn_plugin_121(manager), plugin_bitmap_(kPluginXpm), timer_(this) {
  Bind(wxEVT_TIMER, &RuntimeHostToolbarProbePi::OnProbeTimer, this,
       timer_.GetId());
}

RuntimeHostToolbarProbePi::~RuntimeHostToolbarProbePi() {
  if (initialized_) DeInit();
  Unbind(wxEVT_TIMER, &RuntimeHostToolbarProbePi::OnProbeTimer, this,
         timer_.GetId());
}

int RuntimeHostToolbarProbePi::Init() {
  initialized_ = true;
  wxLogMessage("RUNTIME_HOST_PROBE event=init api=1.21");
  const wxString private_root = *GetpPrivateApplicationDataLocation();
  const wxString installed_data =
      GetPluginDataDir("runtime_host_toolbar_probe_pi");
  wxLogMessage(
      "RUNTIME_HOST_PUBLIC_API event=paths private-root=%s installed-data=%s",
      private_root, installed_data);
  wxLogMessage(
      "RUNTIME_HOST_PUBLIC_API event=navigation routes=%zu waypoints=%zu "
      "chart-directories=%zu",
      GetRouteGUIDArray().size(), GetWaypointGUIDArray().size(),
      GetChartDBDirArrayString().size());

  RegisterAction(kManager, "Portable Runtime", "Open Portable Runtime manager",
                 Icon("manager.svg"), false);
  RegisterAction(kIgrib, "iGRIB", "Open iGRIB", Icon("igrib.svg"), false);

#ifdef RUNTIME_HOST_PROBE_WITH_WASMTIME
  wxString component;
  if (wxGetEnv("OCPN_RUNTIME_HOST_PROBE_COMPONENT", &component) &&
      !component.IsEmpty()) {
    runtime_probe_ = std::make_unique<RuntimeBridgeProbe>();
    if (!runtime_probe_->Start(component.ToStdString())) {
      wxLogMessage("RUNTIME_HOST_WASMTIME event=start-failed");
      runtime_probe_.reset();
    }
  }
#endif

  wxString autorun;
  if (wxGetEnv("OCPN_RUNTIME_HOST_PROBE_AUTORUN", &autorun) &&
      autorun == "1") {
    timer_.Start(1200);
    wxLogMessage("RUNTIME_HOST_PROBE event=autorun-start");
  }

  return WANTS_TOOLBAR_CALLBACK | INSTALLS_TOOLBAR_TOOL |
         WANTS_OVERLAY_CALLBACK | WANTS_OPENGL_OVERLAY_CALLBACK;
}

bool RuntimeHostToolbarProbePi::DeInit() {
  if (!initialized_) return true;
  timer_.Stop();
#ifdef RUNTIME_HOST_PROBE_WITH_WASMTIME
  if (runtime_probe_) {
    runtime_probe_->Stop();
    runtime_probe_.reset();
  }
#endif
  RemoveAction(kWeatherRouting, "plugin-shutdown");
  RemoveAction(kIgrib, "plugin-shutdown");
  RemoveAction(kManager, "plugin-shutdown");
  initialized_ = false;
  wxLogMessage("RUNTIME_HOST_PROBE event=deinit remaining=%zu",
               registry_.Size());
  return true;
}

bool RuntimeHostToolbarProbePi::RegisterAction(
    const LogicalActionKey& key, const wxString& label,
    const wxString& tooltip, const wxString& icon_file, bool force_rebuild) {
  if (registry_.Find(key)) {
    wxLogMessage("RUNTIME_HOST_PROBE event=duplicate-rejected key=%s/%s",
                 key.package_id, key.action_id);
    return false;
  }
  if (!wxFileName::FileExists(icon_file)) {
    wxLogMessage("RUNTIME_HOST_PROBE event=icon-missing key=%s/%s path=%s",
                 key.package_id, key.action_id, icon_file);
    return false;
  }

  const int id = InsertPlugInToolSVG(
      label, icon_file, icon_file, icon_file, wxITEM_CHECK, tooltip, wxEmptyString,
      nullptr, -1, 0, this);
  if (!registry_.Add(key, id)) {
    if (id >= 0) RemovePlugInTool(id);
    wxLogMessage("RUNTIME_HOST_PROBE event=registration-rejected key=%s/%s id=%d",
                 key.package_id, key.action_id, id);
    return false;
  }

  auto* action = registry_.Find(key);
  LogRegistry("registered", *action);

  // InsertPlugInToolSVG stores a new tool but does not request a rebuild in
  // OpenCPN 5.14. SetToolbarToolViz does, even when visibility is unchanged.
  // The probe uses this public-only workaround only for post-Init additions.
  if (force_rebuild) SetToolbarToolViz(id, true);
  return true;
}

bool RuntimeHostToolbarProbePi::RemoveAction(const LogicalActionKey& key,
                                             const char* reason) {
  auto* action = registry_.Find(key);
  if (!action) return false;
  const RegisteredAction copy = *action;
  RemovePlugInTool(copy.tool_id);
  registry_.Remove(key);
  wxLogMessage(
      "RUNTIME_HOST_PROBE event=removed key=%s/%s id=%d reason=%s remaining=%zu",
      key.package_id, key.action_id, copy.tool_id, reason, registry_.Size());
  return true;
}

void RuntimeHostToolbarProbePi::OnToolbarToolCallback(int id) {
  auto action = registry_.FindByToolId(id);
  if (!action) {
    wxLogMessage("RUNTIME_HOST_PROBE event=unmapped-click id=%d", id);
    return;
  }
  auto* mutable_action = registry_.Find(action->key);
  if (!mutable_action || !mutable_action->dispatchable) {
    wxLogMessage("RUNTIME_HOST_PROBE event=blocked-click key=%s/%s id=%d",
                 action->key.package_id, action->key.action_id, id);
    return;
  }
  mutable_action->checked = !mutable_action->checked;
  SetToolbarItemState(id, mutable_action->checked);
  LogRegistry("clicked", *mutable_action);
}

std::vector<std::pair<double, double>>
RuntimeHostToolbarProbePi::OverlayPoints() const {
#ifdef RUNTIME_HOST_PROBE_WITH_WASMTIME
  if (runtime_probe_ && !runtime_probe_->points.empty()) {
    std::vector<std::pair<double, double>> result;
    result.reserve(runtime_probe_->points.size());
    for (const auto& point : runtime_probe_->points)
      result.emplace_back(point.latitude, point.longitude);
    return result;
  }
#endif
  return {{49.80, -4.40}, {50.00, -4.00}, {50.20, -3.60}};
}

bool RuntimeHostToolbarProbePi::RenderOverlayMultiCanvas(
    wxDC& dc, PlugIn_ViewPort* vp, int canvas_index, int priority) {
  if (!vp || priority != 0) return false;
  const auto points = OverlayPoints();
  if (points.size() < 2) return false;
  dc.SetPen(wxPen(wxColour(255, 80, 30), 3));
  wxPoint previous;
  GetCanvasPixLL(vp, &previous, points.front().first, points.front().second);
  for (size_t i = 1; i < points.size(); ++i) {
    wxPoint next;
    GetCanvasPixLL(vp, &next, points[i].first, points[i].second);
    dc.DrawLine(previous, next);
    previous = next;
  }
  if (!software_render_logged_) {
    software_render_logged_ = true;
    wxLogMessage(
        "RUNTIME_HOST_OVERLAY event=software-render canvas=%d points=%zu",
        canvas_index, points.size());
  }
  return true;
}

bool RuntimeHostToolbarProbePi::RenderGLOverlayMultiCanvas(
    wxGLContext*, PlugIn_ViewPort* vp, int canvas_index, int priority) {
  if (!vp || priority != 0) return false;
  const auto points = OverlayPoints();
  if (points.size() < 2) return false;
  glPushAttrib(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT | GL_LINE_BIT);
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4ub(255, 80, 30, 220);
  glLineWidth(3.0f);
  glBegin(GL_LINE_STRIP);
  for (const auto& point : points) {
    wxPoint pixel;
    GetCanvasPixLL(vp, &pixel, point.first, point.second);
    glVertex2i(pixel.x, vp->pix_height - pixel.y);
  }
  glEnd();
  glPopAttrib();
  if (!gl_render_logged_) {
    gl_render_logged_ = true;
    wxLogMessage("RUNTIME_HOST_OVERLAY event=gl-render canvas=%d points=%zu",
                 canvas_index, points.size());
  }
  return true;
}

void RuntimeHostToolbarProbePi::OnProbeTimer(wxTimerEvent&) {
  switch (++probe_stage_) {
    case 1:
      RegisterAction(kWeatherRouting, "iWeatherRouting",
                     "Open iWeatherRouting", Icon("weather-routing.svg"), true);
      break;
    case 2:
      RegisterAction(kWeatherRouting, "duplicate", "duplicate",
                     Icon("weather-routing.svg"), true);
      break;
    case 3: {
      auto* action = registry_.Find(kIgrib);
      if (action) {
        action->checked = true;
        SetToolbarItemState(action->tool_id, true);
        LogRegistry("checked", *action);
      }
      break;
    }
    case 4:
      RemoveAction(kIgrib, "simulated-package-disable");
      break;
    case 5:
      RegisterAction(kIgrib, "iGRIB", "Open iGRIB", Icon("igrib.svg"), true);
      break;
    case 6:
      RemoveAction(kWeatherRouting, "simulated-component-trap");
      break;
    case 7:
      RegisterAction(kWeatherRouting, "iWeatherRouting",
                     "Open iWeatherRouting", Icon("weather-routing.svg"), true);
      break;
    default:
      timer_.Stop();
      wxLogMessage("RUNTIME_HOST_PROBE event=autorun-complete remaining=%zu",
                   registry_.Size());
      break;
  }
}

void RuntimeHostToolbarProbePi::LogRegistry(
    const char* event, const RegisteredAction& action) const {
  wxLogMessage(
      "RUNTIME_HOST_PROBE event=%s key=%s/%s id=%d checked=%d dispatchable=%d",
      event, action.key.package_id, action.key.action_id, action.tool_id,
      action.checked, action.dispatchable);
}
