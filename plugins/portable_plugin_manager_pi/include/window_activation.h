#ifndef PORTABLE_PLUGIN_MANAGER_WINDOW_ACTIVATION_H
#define PORTABLE_PLUGIN_MANAGER_WINDOW_ACTIVATION_H

class wxWindow;

namespace ppm {

/**
 * Return OpenCPN's top-level application window when available.
 *
 * Modeless portable surfaces use this as their owner so desktop window
 * managers keep them in the same transient window group as the chart.
 */
wxWindow* ResolveOpenCpnTopLevelParent(wxWindow* preferred = nullptr);

/**
 * Show and foreground a modeless portable surface.
 *
 * Activation is repeated on the next UI-loop turn because some desktop window
 * managers restore focus to the chart when its toolbar event completes.
 */
void ShowAndActivateWindow(wxWindow* window);

}  // namespace ppm

#endif
