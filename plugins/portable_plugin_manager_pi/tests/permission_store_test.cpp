#include "permission_store.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#define CHECK(expression)                                                   \
  do {                                                                      \
    if (!(expression)) {                                                    \
      std::cerr << "check failed at " << __FILE__ << ':' << __LINE__       \
                << ": " #expression "\n";                                  \
      return 1;                                                             \
    }                                                                       \
  } while (false)

namespace fs = std::filesystem;

int main() {
  const auto stamp =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const fs::path root =
      fs::temp_directory_path() /
      fs::path("ppm grants \xCE\x94 " + std::to_string(stamp));
  ppm::PermissionStore store(root);

  ppm::StoredPackage package;
  package.id = "org.opencpn.permission-test";
  package.name = "Permission test";
  package.version = "1.0.0";
  package.manifest_digest.assign(64, 'a');
  package.permissions = {"settings.read-write", "ui.commands"};

  auto evaluation = store.Evaluate(package);
  CHECK(evaluation.okay);
  CHECK(!evaluation.current);
  CHECK(evaluation.added.size() == 2);
  CHECK(evaluation.removed.empty());
  CHECK(store.Grant(package).okay);

  evaluation = store.Evaluate(package);
  CHECK(evaluation.okay);
  CHECK(evaluation.current);
  CHECK(evaluation.manifest_current);
  CHECK(evaluation.added.empty());
  CHECK(evaluation.removed.empty());

  package.permissions = {"network.http", "ui.commands"};
  package.manifest_digest.assign(64, 'b');
  evaluation = store.Evaluate(package);
  CHECK(evaluation.okay);
  CHECK(!evaluation.current);
  CHECK(!evaluation.manifest_current);
  CHECK(evaluation.added.size() == 1);
  CHECK(evaluation.added[0] == "network.http");
  CHECK(evaluation.removed.size() == 1);
  CHECK(evaluation.removed[0] == "settings.read-write");
  CHECK(store.Grant(package).okay);
  CHECK(store.Evaluate(package).current);

  package.permissions = {"ui.commands"};
  package.manifest_digest.assign(64, 'c');
  evaluation = store.Evaluate(package);
  CHECK(evaluation.okay);
  CHECK(evaluation.added.empty());
  CHECK(evaluation.removed.size() == 1);
  CHECK(store.Grant(package).okay);
  CHECK(store.Evaluate(package).current);

  ppm::StoredPackage unknown = package;
  unknown.permissions = {"host.everything"};
  CHECK(!store.Evaluate(unknown).okay);

  CHECK(store.Revoke(package.id).okay);
  evaluation = store.Evaluate(package);
  CHECK(evaluation.okay);
  CHECK(!evaluation.current);
  CHECK(evaluation.added.size() == 1);

  fs::create_directories(store.GrantsRoot());
  {
    std::ofstream corrupt(store.GrantsRoot() / (package.id + ".grant"));
    corrupt << "format=1\nmanifest_sha256=" << std::string(64, 'a')
            << "\npermission=ui.commands\npermission=ui.commands\n";
  }
  evaluation = store.Evaluate(package);
  CHECK(evaluation.okay);
  CHECK(!evaluation.current);
  CHECK(evaluation.added == package.permissions);

  std::error_code ignored;
  fs::remove_all(root, ignored);
  return 0;
}
