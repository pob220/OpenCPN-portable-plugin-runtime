#ifndef PORTABLE_PLUGIN_MANAGER_PACKAGE_STORE_H
#define PORTABLE_PLUGIN_MANAGER_PACKAGE_STORE_H

#include <filesystem>
#include <string>
#include <vector>

namespace ppm {

struct StoreResult {
  bool okay = false;
  std::string code;
  std::string message;
  std::string package_id;
  std::filesystem::path destination;
  std::filesystem::path recovery;
};

struct StoredPackage {
  std::string id;
  std::string name;
  std::string version;
  bool development = false;
  bool enabled = false;
  std::filesystem::path root;
};

class PackageStore {
 public:
  explicit PackageStore(std::filesystem::path root);

  void SetDeveloperMode(bool enabled) { developer_mode_ = enabled; }
  StoreResult Inspect(const std::filesystem::path& archive) const;
  StoreResult Install(const std::filesystem::path& archive, bool replace);
  StoreResult Remove(const std::string& package_id);
  StoreResult Rollback(const std::string& package_id);
  StoreResult SetEnabled(const std::string& package_id, bool enabled);
  std::vector<StoredPackage> Installed(std::string* diagnostic = nullptr) const;

  const std::filesystem::path& Root() const { return root_; }
  std::filesystem::path PackagesRoot() const { return root_ / "packages"; }
  std::filesystem::path TrustRoot() const { return root_ / "trust"; }

 private:
  std::filesystem::path root_;
  bool developer_mode_ = false;
};

}  // namespace ppm

#endif
