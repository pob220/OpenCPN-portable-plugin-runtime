/***************************************************************************
 * Experimental portable plugin runtime for OpenCPN.
 *
 * This interface is deliberately small: all runtime implementation details
 * and the native bridge are private to portable_plugin_manager.cpp.
 ***************************************************************************/

#ifndef OCPN_PORTABLE_PLUGIN_MANAGER_H
#define OCPN_PORTABLE_PLUGIN_MANAGER_H

#include <memory>

class PlugInManager;
class ViewPort;
class ocpnDC;

class PortablePluginManager {
public:
  explicit PortablePluginManager(PlugInManager* plugin_manager);
  ~PortablePluginManager();

  PortablePluginManager(const PortablePluginManager&) = delete;
  PortablePluginManager& operator=(const PortablePluginManager&) = delete;

  /** Discover, validate, instantiate and enable configured packages. */
  bool Load();

  /** Cancel all work, disable all components and release runtimes. */
  void Shutdown();

  /** Dispatch a native toolbar id to its portable, string-valued action. */
  bool HandleToolbarAction(int toolbar_id);

  /** Render retained geographic scenes using OpenCPN's renderer. */
  bool Render(ocpnDC& dc, const ViewPort& viewport, int priority);

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

#endif  // OCPN_PORTABLE_PLUGIN_MANAGER_H
