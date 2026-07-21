/***************************************************************************
 * Host-owned environmental services for the experimental portable runtime.
 *
 * Portable components never receive wxWidgets, OpenCPN, ecCodes or graphics
 * objects.  This adapter owns the UI, supervised native helpers and retained
 * overlay data used by the iGRIB architecture reference implementation.
 ***************************************************************************/

#ifndef GUI_PORTABLE_GRIB_HOST_H_
#define GUI_PORTABLE_GRIB_HOST_H_

#include <memory>
#include <cstdint>
#include <vector>

#include <wx/string.h>

class ocpnDC;
class ViewPort;
class wxWindow;

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

class PortableGribHost {
public:
  explicit PortableGribHost(wxWindow* parent, const wxString& package_root,
                            bool credential_access);
  ~PortableGribHost();

  PortableGribHost(const PortableGribHost&) = delete;
  PortableGribHost& operator=(const PortableGribHost&) = delete;

  bool Show(wxString* error);
  bool Render(ocpnDC& dc, const ViewPort& viewport);
  void SetCursorPosition(double latitude, double longitude);
  bool SampleBatch(const std::vector<PortableEnvironmentRequest>& requests,
                   std::vector<PortableEnvironmentSample>* results,
                   wxString* error) const;
  wxString DatasetSummary() const;
  bool DisplayedTime(int64_t* unix_time) const;
  void Shutdown();

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

#endif  // GUI_PORTABLE_GRIB_HOST_H_
