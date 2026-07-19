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

#include <wx/string.h>

class ocpnDC;
class ViewPort;
class wxWindow;

class PortableGribHost {
public:
  explicit PortableGribHost(wxWindow* parent, const wxString& package_root,
                            bool credential_access);
  ~PortableGribHost();

  PortableGribHost(const PortableGribHost&) = delete;
  PortableGribHost& operator=(const PortableGribHost&) = delete;

  bool Show(wxString* error);
  bool Render(ocpnDC& dc, const ViewPort& viewport);
  void Shutdown();

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

#endif  // GUI_PORTABLE_GRIB_HOST_H_
