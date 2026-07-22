#ifndef RUNTIME_HOST_TOOLBAR_PROBE_RUNTIME_BRIDGE_PROBE_H
#define RUNTIME_HOST_TOOLBAR_PROBE_RUNTIME_BRIDGE_PROBE_H

#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "ocpn_portable_runtime.h"

class RuntimeBridgeProbe {
 public:
  RuntimeBridgeProbe() = default;
  ~RuntimeBridgeProbe();

  bool Start(const std::string& component_path);
  void Stop();
  void StartBoundedJob();

  std::map<std::string, std::string> settings;
  unsigned action_count = 0;
  unsigned scene_count = 0;
  unsigned job_count = 0;
  std::vector<ocpn_portable_geo_point> points;

 private:
  void StopBoundedJob();

  ocpn_portable_runtime* runtime_ = nullptr;
  std::atomic<bool> cancel_job_{false};
  std::thread job_thread_;
};

#endif
