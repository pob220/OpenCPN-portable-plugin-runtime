#ifndef PORTABLE_PLUGIN_MANAGER_WEATHER_ROUTING_HOST_H
#define PORTABLE_PLUGIN_MANAGER_WEATHER_ROUTING_HOST_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <wx/string.h>

class wxDC;
class wxFileConfig;
class wxWindow;
struct PlugIn_ViewPort;

#include "routing_service.h"

/** Stable, value-based snapshot of an OpenCPN navigation position. */
struct PortableNavigationPosition {
  wxString id;
  wxString name;
  double latitude = 0.0;
  double longitude = 0.0;
};

/** Stable, value-based snapshot of an ordered OpenCPN route. */
struct PortableNavigationRoute {
  wxString id;
  wxString name;
  std::vector<PortableNavigationPosition> points;
};

class PortableWeatherRoutingHost {
public:
  using CalculateRoute = std::function<bool(
      ppm::RoutingRequest, ppm::RoutingOutcome*, std::string*)>;
  using CalculatePassage = std::function<bool(
      ppm::RoutingPassageRequest, ppm::RoutingOutcome*, std::string*)>;
  using BeginRouteAttempt = std::function<bool(wxString*)>;

  PortableWeatherRoutingHost(
      wxWindow* parent, wxFileConfig* config, CalculateRoute calculate_route,
      CalculatePassage calculate_passage, BeginRouteAttempt begin_route_attempt,
      std::function<void()> cancel_routes, const wxString& package_root,
      const wxString& plugin_id, const wxString& surface_resource,
      std::function<wxString()> dataset_summary,
      std::function<std::vector<PortableNavigationPosition>()> list_waypoints,
      std::function<std::vector<PortableNavigationRoute>()> list_routes,
      std::function<bool(const wxString&,
                         const std::vector<PortableNavigationPosition>&,
                         wxString*)>
          create_route,
      std::function<bool(PortableNavigationPosition*)> vessel_position,
      std::function<bool(PortableNavigationPosition*)> cursor_position,
      std::function<bool(int64_t*)> displayed_environment_time,
      std::function<bool(double, double, const std::vector<int64_t>&,
                         std::vector<uint8_t>*, wxString*)>
          preflight_environment,
      double start_latitude, double start_longitude);
  ~PortableWeatherRoutingHost();
  bool Show(wxString* error);
  bool ShowRouteAnalysis(const wxString& route_id, wxString* error);
  bool Render(wxDC& dc, PlugIn_ViewPort* viewport);
  bool RenderGL(PlugIn_ViewPort* viewport);
  void SetColorScheme(int scheme);
  void CursorChanged();
  void ReportProgress(unsigned percent, const wxString& message);
  bool Cancelled() const;
  void Shutdown();

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

#endif
