/***************************************************************************
 * Host-owned environmental services for the experimental portable runtime.
 *
 * Portable components never receive wxWidgets, OpenCPN, ecCodes or graphics
 * objects.  This adapter implements generic environmental dataset,
 * declarative-surface, supervised-helper and retained-overlay services.
 * Plugin identity, controls, fields and provider catalogues are supplied by
 * the portable package.
 ***************************************************************************/

#ifndef PORTABLE_PLUGIN_MANAGER_ENVIRONMENT_WORKBENCH_H
#define PORTABLE_PLUGIN_MANAGER_ENVIRONMENT_WORKBENCH_H

#include <memory>
#include <cstdint>
#include <functional>
#include <vector>

#include <wx/string.h>

class wxDC;
class wxWindow;
class PlugIn_ViewPort;

struct PortableEnvironmentRequest {
  double latitude = 0.0;
  double longitude = 0.0;
  int64_t unix_time = 0;
};

struct PortableEnvironmentSample {
  double wind_u_knots = 0.0;
  double wind_v_knots = 0.0;
  double current_u_knots = 0.0;
  double current_v_knots = 0.0;
  double wave_height_metres = 0.0;
  unsigned available = 0;
};

/** Stable, value-based navigation position offered to the weather table. */
struct PortableEnvironmentPosition {
  wxString id;
  wxString name;
  double latitude = 0.0;
  double longitude = 0.0;
};

/** Immutable value snapshot selected by a portable environmental consumer. */
struct PortableEnvironmentDataset {
  uint64_t revision = 0;
  uint64_t byte_size = 0;
  wxString id;
  wxString source;
  wxString display_name;
  wxString sha256;
  std::vector<wxString> forecast_times;
};

class PortableEnvironmentHost {
public:
  using SurfaceEvent = std::function<bool(
      const wxString&, const wxString&, wxString*, wxString*)>;
  using DatasetOpened = std::function<void(const wxString&)>;
  using ViewBounds =
      std::function<bool(double*, double*, double*, double*)>;
  using RefreshCanvas = std::function<void()>;

  explicit PortableEnvironmentHost(wxWindow* parent, const wxString& plugin_id,
                                   const wxString& package_root,
                                   const wxString& surface_resource,
                                   bool credential_access,
                                   SurfaceEvent surface_event,
                                   DatasetOpened dataset_opened,
                                   ViewBounds view_bounds,
                                   RefreshCanvas refresh_canvas,
                                   std::function<std::vector<
                                       PortableEnvironmentPosition>()>
                                       list_waypoints,
                                   std::function<bool(
                                       PortableEnvironmentPosition*)>
                                       vessel_position);
  ~PortableEnvironmentHost();

  PortableEnvironmentHost(const PortableEnvironmentHost&) = delete;
  PortableEnvironmentHost& operator=(const PortableEnvironmentHost&) = delete;

  bool Show(wxString* error);
  bool OpenDataset(const std::vector<wxString>& paths, wxString* error);
  bool Render(wxDC& dc, PlugIn_ViewPort* viewport);
  void SetCursorPosition(double latitude, double longitude);
  bool SampleBatch(const std::vector<PortableEnvironmentRequest>& requests,
                   std::vector<PortableEnvironmentSample>* results,
                   wxString* error) const;
  std::shared_ptr<const PortableEnvironmentDataset> AcquireDataset(
      wxString* error) const;
  bool SampleBatch(
      const std::shared_ptr<const PortableEnvironmentDataset>& dataset,
      const std::vector<PortableEnvironmentRequest>& requests,
      std::vector<PortableEnvironmentSample>* results, wxString* error) const;
  wxString DatasetSummary() const;
  bool DisplayedTime(int64_t* unix_time) const;
  /**
   * Signal in-flight headless service work to stop without destroying the UI
   * or dataset.  Used as the first phase of application/plugin shutdown so a
   * consumer worker cannot keep the GUI thread blocked while it is joined.
   */
  void RequestStop();
  void Shutdown();

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

#endif  // PORTABLE_PLUGIN_MANAGER_ENVIRONMENT_WORKBENCH_H
