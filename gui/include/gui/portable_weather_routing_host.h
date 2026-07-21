#ifndef GUI_PORTABLE_WEATHER_ROUTING_HOST_H_
#define GUI_PORTABLE_WEATHER_ROUTING_HOST_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <wx/string.h>

class ocpnDC;
class ViewPort;
class wxWindow;
struct ocpn_portable_runtime;

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
  PortableWeatherRoutingHost(
      wxWindow* parent, ocpn_portable_runtime* runtime,
      std::shared_ptr<std::mutex> runtime_mutex, const wxString& package_root,
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
  bool Render(ocpnDC& dc, const ViewPort& viewport);
  void ReportProgress(unsigned percent, const wxString& message);
  bool Cancelled() const;
  void Shutdown();

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

#endif
