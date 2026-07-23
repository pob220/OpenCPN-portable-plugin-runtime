#ifndef PORTABLE_PLUGIN_MANAGER_PERMISSION_STORE_H
#define PORTABLE_PLUGIN_MANAGER_PERMISSION_STORE_H

#include <filesystem>
#include <string>
#include <vector>

#include "package_store.h"

namespace ppm {

enum class PermissionRisk { kLow, kModerate, kHigh, kNative };

struct PermissionDescriptor {
  std::string id;
  std::string label;
  std::string description;
  PermissionRisk risk = PermissionRisk::kLow;
};

struct PermissionEvaluation {
  bool okay = false;
  bool current = false;
  bool manifest_current = false;
  std::string message;
  std::vector<std::string> requested;
  std::vector<std::string> added;
  std::vector<std::string> removed;
};

const std::vector<PermissionDescriptor>& PermissionCatalogue();
const PermissionDescriptor* FindPermission(const std::string& id);

class PermissionStore {
 public:
  explicit PermissionStore(std::filesystem::path root);

  PermissionEvaluation Evaluate(const StoredPackage& package) const;
  StoreResult Grant(const StoredPackage& package);
  StoreResult Revoke(const std::string& package_id);
  std::filesystem::path GrantsRoot() const { return root_ / "grants"; }

 private:
  std::filesystem::path root_;
};

}  // namespace ppm

#endif
