#ifndef PORTABLE_PLUGIN_MANAGER_ENVIRONMENT_PROVIDER_H
#define PORTABLE_PLUGIN_MANAGER_ENVIRONMENT_PROVIDER_H

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "environment_service.h"

namespace ppm {

/**
 * Host-owned adapter for one signed environmental-data provider package.
 *
 * User-selected GRIB data is copied into an immutable private snapshot. The
 * package's signed decoder helper is launched under platform containment and
 * decoded frames are cached behind a bounded, thread-safe service interface.
 */
class EnvironmentProvider {
public:
  using Cancelled = std::function<bool()>;

  EnvironmentProvider(std::string package_root, std::string private_root);
  ~EnvironmentProvider();

  EnvironmentProvider(const EnvironmentProvider&) = delete;
  EnvironmentProvider& operator=(const EnvironmentProvider&) = delete;

  bool OpenDataset(const std::vector<std::string>& selected_paths,
                   const Cancelled& cancelled, std::string* diagnostic);
  bool SampleBatch(const std::vector<EnvironmentRequest>& requests,
                   std::vector<EnvironmentSample>* samples,
                   const Cancelled& cancelled, std::string* diagnostic) const;
  std::string Summary() const;
  std::string Generation() const;
  bool Available() const;
  void Resume();
  void RequestStop();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ppm

#endif
