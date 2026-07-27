#include "permission_store.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ppm {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kGrantFileLimit = 64 * 1024;

StoreResult Failure(std::string code, std::string message,
                    std::string package_id = {}) {
  return {false, std::move(code), std::move(message), std::move(package_id), {},
          {}};
}

bool SafePackageId(const std::string& value) {
  if (value.empty() || value.size() > 128 ||
      value.find('.') == std::string::npos) {
    return false;
  }
  bool label_start = true;
  char previous = '\0';
  for (const unsigned char character : value) {
    if (character == '.') {
      if (label_start || previous == '-') return false;
      label_start = true;
    } else if ((character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9')) {
      label_start = false;
    } else if (character == '-' && !label_start) {
      label_start = false;
    } else {
      return false;
    }
    previous = static_cast<char>(character);
  }
  return !label_start && previous != '-';
}

struct GrantRecord {
  std::string manifest_digest;
  std::vector<std::string> permissions;
};

bool ReadGrant(const fs::path& path, GrantRecord* grant,
               std::string* diagnostic) {
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error) {
    *diagnostic = "permission approval has not been recorded";
    return false;
  }
  if (size > kGrantFileLimit) {
    *diagnostic = "permission grant file exceeds policy";
    return false;
  }
  std::ifstream input(path);
  if (!input) {
    *diagnostic = "permission grant file is unreadable";
    return false;
  }
  std::string line;
  bool format_seen = false;
  bool digest_seen = false;
  std::set<std::string> unique;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == "format=1") {
      if (format_seen) {
        *diagnostic = "duplicate permission grant format";
        return false;
      }
      format_seen = true;
    } else if (line.rfind("manifest_sha256=", 0) == 0) {
      if (digest_seen) {
        *diagnostic = "duplicate permission grant digest";
        return false;
      }
      grant->manifest_digest = line.substr(16);
      digest_seen = true;
    } else if (line.rfind("permission=", 0) == 0) {
      const std::string permission = line.substr(11);
      if (!FindPermission(permission) || !unique.insert(permission).second) {
        *diagnostic = "invalid or duplicate stored permission";
        return false;
      }
      grant->permissions.push_back(permission);
    } else {
      *diagnostic = "unknown permission grant field";
      return false;
    }
  }
  if (!input.eof() || !format_seen || !digest_seen ||
      grant->manifest_digest.size() != 64 ||
      !std::all_of(grant->manifest_digest.begin(), grant->manifest_digest.end(),
                   [](unsigned char value) {
                     return std::isdigit(value) ||
                            (value >= 'a' && value <= 'f');
                   })) {
    *diagnostic = "malformed permission grant";
    return false;
  }
  std::sort(grant->permissions.begin(), grant->permissions.end());
  return true;
}

}  // namespace

const std::vector<PermissionDescriptor>& PermissionCatalogue() {
  static const std::vector<PermissionDescriptor> catalogue = {
      {"ui.commands", "Toolbar commands",
       "Add package-owned commands to the OpenCPN toolbar.",
       PermissionRisk::kLow},
      {"ui.surfaces", "Portable user-interface surfaces",
       "Open host-owned tool, preferences, task and dockable surfaces.",
       PermissionRisk::kLow},
      {"navigation.position.read", "Vessel position",
       "Read the current vessel position, course and speed.",
       PermissionRisk::kModerate},
      {"navigation.nmea.read", "Live navigation sentences",
       "Receive bounded NMEA 0183 sentences supplied to OpenCPN.",
       PermissionRisk::kModerate},
      {"navigation.nmea2000.read", "NMEA 2000 navigation data",
       "Receive filtered, bounded NMEA 2000 payloads supplied to OpenCPN.",
       PermissionRisk::kModerate},
      {"navigation.signalk.read", "Signal K navigation data",
       "Receive filtered, bounded Signal K updates supplied to OpenCPN.",
       PermissionRisk::kModerate},
      {"navigation.ais.read", "AIS targets",
       "Receive bounded AIS target updates supplied to OpenCPN.",
       PermissionRisk::kModerate},
      {"navigation.active-leg.read", "Active navigation leg",
       "Receive bounded active-route and waypoint-leg updates.",
       PermissionRisk::kModerate},
      {"navigation.objects.read", "Routes and waypoints",
       "Read OpenCPN route and waypoint values.", PermissionRisk::kModerate},
      {"navigation.objects.write", "Change navigation objects",
       "Request user-confirmed creation, editing or deletion of OpenCPN "
       "waypoints, routes and tracks.",
       PermissionRisk::kHigh},
      {"navigation.nmea.write", "Transmit NMEA 0183",
       "Transmit validated, rate-limited NMEA 0183 sentences through "
       "OpenCPN.",
       PermissionRisk::kHigh},
      {"communications.outputs.read", "Communication outputs",
       "List opaque identifiers for active OpenCPN output connections.",
       PermissionRisk::kModerate},
      {"navigation.nmea2000.write", "Transmit safe NMEA 2000 data",
       "Transmit allowlisted, rate-limited informational NMEA 2000 PGNs.",
       PermissionRisk::kHigh},
      {"chart.cursor.read", "Chart cursor",
       "Receive the current chart-cursor position.", PermissionRisk::kLow},
      {"chart.viewport.read", "Chart viewport",
       "Receive bounded chart viewport and scale updates.",
       PermissionRisk::kLow},
      {"chart.input.pointer", "Chart pointer input",
       "Receive chart pointer events and optionally consume them.",
       PermissionRisk::kModerate},
      {"chart.input.keyboard", "Chart keyboard input",
       "Receive chart keyboard events and optionally consume them.",
       PermissionRisk::kModerate},
      {"plugin.messages.receive", "Plugin messages",
       "Receive explicitly filtered OpenCPN plugin messages.",
       PermissionRisk::kModerate},
      {"plugin.messages.send", "Send plugin messages",
       "Publish bounded OpenCPN plugin messages under the package identity.",
       PermissionRisk::kHigh},
      {"plugin.rpc.request", "Request portable services",
       "Make bounded asynchronous typed requests to explicitly named "
       "portable packages.",
       PermissionRisk::kModerate},
      {"plugin.rpc.provide", "Provide portable services",
       "Register bounded typed services for other portable packages.",
       PermissionRisk::kModerate},
      {"settings.read-write", "Package settings",
       "Read and write settings isolated to this package.",
       PermissionRisk::kLow},
      {"overlay.submit", "Chart overlays",
       "Submit bounded graphics for OpenCPN to draw over charts.",
       PermissionRisk::kModerate},
      {"jobs.compute", "Background computation",
       "Run bounded background work managed by the host.",
       PermissionRisk::kModerate},
      {"timers.schedule", "Package timers",
       "Schedule bounded lifecycle-owned one-shot or repeating timers.",
       PermissionRisk::kLow},
      {"environment.datasets", "Environmental datasets",
       "Publish or inspect host-managed weather and ocean datasets.",
       PermissionRisk::kModerate},
      {"storage.user-selected", "User-selected files",
       "Import or export only files explicitly selected by the user.",
       PermissionRisk::kModerate},
      {"network.providers", "Weather providers",
       "Contact configured environmental-data providers.",
       PermissionRisk::kHigh},
      {"helpers.environment.decode", "Native GRIB decoder",
       "Run a signed target-specific native decoder helper.",
       PermissionRisk::kNative},
      {"helpers.environment.generate", "Native GRIB generator",
       "Run a signed target-specific native generator helper.",
       PermissionRisk::kNative},
      {"charts.coverage", "Advisory land coverage",
       "Query bounded advisory coastline-intersection observations.",
       PermissionRisk::kModerate},
      {"charts.segment-safety", "Authoritative chart safety",
       "Query bounded host-owned land, drying, depth and chart-coverage "
       "assessments for route segments.",
       PermissionRisk::kHigh},
      {"network.http", "Constrained Internet access",
       "Make policy-limited HTTPS requests through the host.",
       PermissionRisk::kHigh},
      {"network.https", "Controlled HTTPS access",
       "Make bounded HTTPS requests only to manifest-declared domains.",
       PermissionRisk::kHigh},
      {"storage.private", "Private package files",
       "Read and write files in this package's isolated private storage.",
       PermissionRisk::kModerate},
      {"credentials.provider", "Provider credentials",
       "Use credentials supplied for a selected data provider without "
       "exposing them to other packages.",
       PermissionRisk::kHigh},
      {"weather-routing.compute", "Weather-route computation",
       "Run bounded weather-routing computations.", PermissionRisk::kModerate},
      {"environment.consume", "Consume environmental data",
       "Read immutable host-managed forecast snapshots.",
       PermissionRisk::kModerate},
      {"navigation.routes.write", "Create OpenCPN routes",
       "Request a user-confirmed route to be added to OpenCPN.",
       PermissionRisk::kHigh},
  };
  return catalogue;
}

const PermissionDescriptor* FindPermission(const std::string& id) {
  const auto& catalogue = PermissionCatalogue();
  const auto item =
      std::find_if(catalogue.begin(), catalogue.end(),
                   [&](const auto& value) { return value.id == id; });
  return item == catalogue.end() ? nullptr : &*item;
}

PermissionStore::PermissionStore(fs::path root) : root_(std::move(root)) {}

PermissionEvaluation PermissionStore::Evaluate(
    const StoredPackage& package) const {
  PermissionEvaluation result;
  if (!SafePackageId(package.id)) {
    result.message = "invalid package identifier";
    return result;
  }
  result.requested = package.permissions;
  if (package.manifest_digest.size() != 64 ||
      !std::all_of(package.manifest_digest.begin(),
                   package.manifest_digest.end(), [](unsigned char value) {
                     return std::isdigit(value) ||
                            (value >= 'a' && value <= 'f');
                   })) {
    result.message = "package manifest digest is invalid";
    return result;
  }
  std::sort(result.requested.begin(), result.requested.end());
  if (std::adjacent_find(result.requested.begin(), result.requested.end()) !=
      result.requested.end()) {
    result.message = "package requests a permission more than once";
    return result;
  }
  for (const auto& permission : result.requested) {
    if (!FindPermission(permission)) {
      result.message = "unknown requested permission: " + permission;
      return result;
    }
  }
  GrantRecord grant;
  std::string diagnostic;
  if (!ReadGrant(GrantsRoot() / (package.id + ".grant"), &grant, &diagnostic)) {
    result.okay = true;
    result.message = diagnostic;
    result.added = result.requested;
    return result;
  }
  std::set_difference(result.requested.begin(), result.requested.end(),
                      grant.permissions.begin(), grant.permissions.end(),
                      std::back_inserter(result.added));
  std::set_difference(grant.permissions.begin(), grant.permissions.end(),
                      result.requested.begin(), result.requested.end(),
                      std::back_inserter(result.removed));
  result.okay = true;
  result.current = result.added.empty() && result.removed.empty();
  result.manifest_current = grant.manifest_digest == package.manifest_digest;
  result.message = result.current
                       ? "Previously approved permissions still match"
                       : "Package permissions changed since the last approval";
  return result;
}

StoreResult PermissionStore::Grant(const StoredPackage& package) {
  const PermissionEvaluation evaluation = Evaluate(package);
  if (!evaluation.okay)
    return Failure("grant-invalid", evaluation.message, package.id);
  std::error_code error;
  fs::create_directories(GrantsRoot(), error);
  if (error) return Failure("grant-failed", error.message(), package.id);
  const fs::path target = GrantsRoot() / (package.id + ".grant");
  const fs::path temporary = GrantsRoot() / (package.id + ".grant.tmp");
  {
    std::ofstream output(temporary, std::ios::trunc);
    output << "format=1\n"
           << "manifest_sha256=" << package.manifest_digest << '\n';
    for (const auto& permission : evaluation.requested)
      output << "permission=" << permission << '\n';
    if (!output)
      return Failure("grant-failed", "could not write permission grant",
                     package.id);
  }
  fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace, error);
  if (error) {
    fs::remove(temporary, error);
    return Failure("grant-failed", error.message(), package.id);
  }
  fs::rename(temporary, target, error);
  if (error) {
    fs::remove(target, error);
    error.clear();
    fs::rename(temporary, target, error);
  }
  if (error) return Failure("grant-failed", error.message(), package.id);
  return {true, "granted", "Package permissions approved", package.id, {}, {}};
}

StoreResult PermissionStore::Revoke(const std::string& package_id) {
  if (!SafePackageId(package_id))
    return Failure("invalid-id", "invalid package identifier", package_id);
  std::error_code error;
  const bool removed =
      fs::remove(GrantsRoot() / (package_id + ".grant"), error);
  if (error) return Failure("revoke-failed", error.message(), package_id);
  return {true,
          "revoked",
          removed ? "Package permissions revoked"
                  : "Package had no stored permission approval",
          package_id,
          {},
          {}};
}

}  // namespace ppm
