#ifndef PORTABLE_PLUGIN_MANAGER_HTTPS_SERVICE_H
#define PORTABLE_PLUGIN_MANAGER_HTTPS_SERVICE_H

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ppm {

struct HttpsRequest {
  std::string method;
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::uint8_t> body;
  std::uint32_t timeout_milliseconds = 10'000;
  std::size_t maximum_response_bytes = 1024 * 1024;
};

struct HttpsResponse {
  long status = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::uint8_t> body;
  std::string final_url;
};

bool IsValidHttpsDomain(const std::string& domain);
bool IsHttpsUrlAllowed(const std::string& url,
                       const std::set<std::string>& allowed_domains,
                       std::string* host, std::string* diagnostic);
bool IsPublicNetworkAddress(const void* address, int family);

bool PerformControlledHttps(const HttpsRequest& request,
                            const std::set<std::string>& allowed_domains,
                            HttpsResponse* response, std::string* diagnostic);

}  // namespace ppm

#endif
