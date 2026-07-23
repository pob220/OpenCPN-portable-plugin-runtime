#include "package_store.h"
#include "permission_store.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

void Usage() {
  std::cerr
      << "usage: ppm_package_cli --root STORE [--trust DIR] [--developer] "
         "[--replace] [--yes] COMMAND [ARG ...]\n"
         "commands:\n"
         "  list\n"
         "  inspect ARCHIVE\n"
         "  install ARCHIVE\n"
         "  audit PACKAGE_ID\n"
         "  state PACKAGE_ID on|off\n"
         "  permissions PACKAGE_ID\n"
         "  approve PACKAGE_ID --yes\n"
         "  revoke PACKAGE_ID\n"
         "  rollback PACKAGE_ID\n"
         "  remove PACKAGE_ID\n";
}

int Report(const ppm::StoreResult& result) {
  std::ostream& output = result.okay ? std::cout : std::cerr;
  output << (result.okay ? "ok" : "error") << '\t' << result.code << '\t'
         << result.package_id << '\t' << result.message;
  if (!result.destination.empty())
    output << "\tdestination=" << result.destination.string();
  if (!result.recovery.empty())
    output << "\trecovery=" << result.recovery.string();
  output << '\n';
  return result.okay ? 0 : 1;
}

const ppm::StoredPackage* FindPackage(
    const std::vector<ppm::StoredPackage>& packages, const std::string& id) {
  const auto item =
      std::find_if(packages.begin(), packages.end(),
                   [&](const auto& package) { return package.id == id; });
  return item == packages.end() ? nullptr : &*item;
}

}  // namespace

int main(int argc, char** argv) {
  fs::path root;
  fs::path trust;
  bool developer = false;
  bool replace = false;
  bool confirmed = false;
  std::vector<std::string> positional;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--root" && index + 1 < argc)
      root = argv[++index];
    else if (argument == "--trust" && index + 1 < argc)
      trust = argv[++index];
    else if (argument == "--developer")
      developer = true;
    else if (argument == "--replace")
      replace = true;
    else if (argument == "--yes")
      confirmed = true;
    else
      positional.push_back(argument);
  }
  if (root.empty() || positional.empty()) {
    Usage();
    return 2;
  }

  ppm::PackageStore packages(root, trust);
  packages.SetDeveloperMode(developer);
  ppm::PermissionStore permissions(root);
  const std::string& command = positional[0];
  if (command == "list" && positional.size() == 1) {
    std::string diagnostic;
    const auto installed = packages.Installed(&diagnostic);
    for (const auto& package : installed) {
      const auto evaluation = permissions.Evaluate(package);
      std::cout << package.id << '\t' << package.version << '\t'
                << (package.enabled ? "enabled" : "disabled") << '\t'
                << (evaluation.current ? "approved" : "approval-required")
                << '\n';
    }
    if (!diagnostic.empty()) {
      std::cerr << "error\tscan-failed\t\t" << diagnostic << '\n';
      return 1;
    }
    return 0;
  }
  if (command == "inspect" && positional.size() == 2)
    return Report(packages.Inspect(positional[1]));
  if (command == "install" && positional.size() == 2)
    return Report(packages.Install(positional[1], replace));
  if (command == "audit" && positional.size() == 2)
    return Report(packages.AuditInstalled(positional[1]));
  if (command == "state" && positional.size() == 3 &&
      (positional[2] == "on" || positional[2] == "off")) {
    if (positional[2] == "on") {
      const ppm::StoreResult audit =
          packages.AuditInstalled(positional[1]);
      if (!audit.okay) return Report(audit);
      const auto installed = packages.Installed();
      const ppm::StoredPackage* package =
          FindPackage(installed, positional[1]);
      if (!package) {
        std::cerr << "error\tnot-installed\t" << positional[1]
                  << "\tPackage is not installed\n";
        return 1;
      }
      const ppm::PermissionEvaluation evaluation =
          permissions.Evaluate(*package);
      if (!evaluation.okay || !evaluation.current) {
        std::cerr << "error\tapproval-required\t" << positional[1]
                  << "\tReview and approve the exact requested permissions "
                     "before enabling\n";
        return 1;
      }
    }
    return Report(
        packages.SetEnabled(positional[1], positional[2] == "on"));
  }
  if ((command == "permissions" || command == "approve") &&
      positional.size() == 2) {
    const auto installed = packages.Installed();
    const ppm::StoredPackage* package =
        FindPackage(installed, positional[1]);
    if (!package) {
      std::cerr << "error\tnot-installed\t" << positional[1]
                << "\tPackage is not installed\n";
      return 1;
    }
    const ppm::StoreResult audit =
        packages.AuditInstalled(positional[1]);
    if (!audit.okay) return Report(audit);
    const ppm::PermissionEvaluation evaluation =
        permissions.Evaluate(*package);
    if (!evaluation.okay) {
      std::cerr << "error\tgrant-invalid\t" << positional[1] << '\t'
                << evaluation.message << '\n';
      return 1;
    }
    std::cout << "requested permissions:\n";
    for (const auto& id : evaluation.requested) {
      const ppm::PermissionDescriptor* descriptor =
          ppm::FindPermission(id);
      std::cout << "  " << id;
      if (descriptor) std::cout << " — " << descriptor->description;
      std::cout << '\n';
    }
    if (command == "permissions") return 0;
    if (!confirmed) {
      std::cerr << "error\tconfirmation-required\t" << positional[1]
                << "\tReview the list above, then use --yes to approve it.\n";
      return 1;
    }
    return Report(permissions.Grant(*package));
  }
  if (command == "revoke" && positional.size() == 2)
    return Report(permissions.Revoke(positional[1]));
  if (command == "rollback" && positional.size() == 2)
    return Report(packages.Rollback(positional[1]));
  if (command == "remove" && positional.size() == 2)
    return Report(packages.Remove(positional[1]));

  Usage();
  return 2;
}
