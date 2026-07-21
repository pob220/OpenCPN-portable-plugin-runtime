/***************************************************************************
 * Host-owned environmental services for the experimental portable runtime.
 *
 * Portable components never receive wxWidgets, OpenCPN, ecCodes or graphics
 * objects.  This adapter implements generic environmental dataset,
 * declarative-surface, supervised-helper and retained-overlay services.
 * Plugin identity, controls, fields and provider catalogues are supplied by
 * the portable package.
 ***************************************************************************/

#ifndef GUI_PORTABLE_ENVIRONMENT_HOST_H_
#define GUI_PORTABLE_ENVIRONMENT_HOST_H_

#include <memory>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include <wx/string.h>

class ocpnDC;
class ViewPort;
class wxWindow;
struct ocpn_portable_runtime;

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
  explicit PortableEnvironmentHost(wxWindow* parent, const wxString& plugin_id,
                                   const wxString& package_root,
                                   const wxString& surface_resource,
                                   bool credential_access,
                                   ocpn_portable_runtime* runtime,
                                   std::shared_ptr<std::mutex> runtime_mutex,
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
  bool Render(ocpnDC& dc, const ViewPort& viewport);
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

#endif  // GUI_PORTABLE_ENVIRONMENT_HOST_H_
