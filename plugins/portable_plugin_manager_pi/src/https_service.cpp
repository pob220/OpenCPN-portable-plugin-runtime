#include "https_service.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace ppm {
namespace {

constexpr std::size_t kMaximumRequestBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaximumResponseBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaximumHeaderBytes = 64 * 1024;
constexpr int kMaximumRedirects = 3;

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

bool SafeHeaderName(const std::string& name) {
  if (name.empty() || name.size() > 128) return false;
  for (const unsigned char character : name) {
    if (!std::isalnum(character) && character != '-' && character != '_')
      return false;
  }
  static const std::set<std::string> denied{"connection",
                                            "content-length",
                                            "host",
                                            "proxy-authorization",
                                            "proxy-connection",
                                            "te",
                                            "trailer",
                                            "transfer-encoding",
                                            "upgrade"};
  return denied.count(Lower(name)) == 0;
}

bool SafeHeaderValue(const std::string& value) {
  return value.size() <= 8192 && value.find('\r') == std::string::npos &&
         value.find('\n') == std::string::npos &&
         value.find('\0') == std::string::npos;
}

bool SensitiveAcrossOrigins(const std::string& name) {
  const std::string lower = Lower(name);
  return lower == "authorization" || lower == "cookie";
}

struct Transfer {
  std::size_t maximum_body = 0;
  std::size_t header_bytes = 0;
  bool body_overflow = false;
  bool header_overflow = false;
  std::vector<std::uint8_t> body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string location;
};

std::size_t WriteBody(char* data, std::size_t size, std::size_t count,
                      void* user_data) {
  auto* transfer = static_cast<Transfer*>(user_data);
  const std::size_t bytes = size * count;
  if (!transfer || bytes > transfer->maximum_body - transfer->body.size()) {
    if (transfer) transfer->body_overflow = true;
    return 0;
  }
  transfer->body.insert(transfer->body.end(), data, data + bytes);
  return bytes;
}

std::size_t WriteHeader(char* data, std::size_t size, std::size_t count,
                        void* user_data) {
  auto* transfer = static_cast<Transfer*>(user_data);
  const std::size_t bytes = size * count;
  if (!transfer || bytes > kMaximumHeaderBytes - transfer->header_bytes) {
    if (transfer) transfer->header_overflow = true;
    return 0;
  }
  transfer->header_bytes += bytes;
  std::string line(data, bytes);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
    line.pop_back();
  const auto separator = line.find(':');
  if (separator == std::string::npos) return bytes;
  std::string name = line.substr(0, separator);
  std::string value = line.substr(separator + 1);
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())))
    value.erase(value.begin());
  if (!SafeHeaderName(name) || !SafeHeaderValue(value)) return bytes;
  if (Lower(name) == "location") transfer->location = value;
  transfer->headers.push_back({std::move(name), std::move(value)});
  return bytes;
}

curl_socket_t OpenPublicSocket(void*, curlsocktype purpose,
                               struct curl_sockaddr* address) {
  if (purpose != CURLSOCKTYPE_IPCXN || !address ||
      !IsPublicNetworkAddress(&address->addr, address->family))
    return CURL_SOCKET_BAD;
  return socket(address->family, address->socktype, address->protocol);
}

bool ResolveRedirect(const std::string& current, const std::string& location,
                     std::string* resolved) {
  if (!resolved || location.empty()) return false;
  CURLU* url = curl_url();
  if (!url) return false;
  bool okay =
      curl_url_set(url, CURLUPART_URL, current.c_str(), 0) == CURLUE_OK &&
      curl_url_set(url, CURLUPART_URL, location.c_str(), 0) == CURLUE_OK;
  char* text = nullptr;
  if (okay) okay = curl_url_get(url, CURLUPART_URL, &text, 0) == CURLUE_OK;
  if (okay && text) *resolved = text;
  if (text) curl_free(text);
  curl_url_cleanup(url);
  return okay;
}

bool PerformOne(const HttpsRequest& request, const std::string& url,
                HttpsResponse* response, std::string* redirect,
                std::string* diagnostic) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    *diagnostic = "could not create the HTTPS transport";
    return false;
  }
  Transfer transfer;
  transfer.maximum_body = request.maximum_response_bytes;
  struct curl_slist* headers = nullptr;
  for (const auto& [name, value] : request.headers) {
    if (!SafeHeaderName(name) || !SafeHeaderValue(value)) {
      *diagnostic = "request contains an unsafe HTTP header";
      curl_easy_cleanup(curl);
      curl_slist_free_all(headers);
      return false;
    }
    headers = curl_slist_append(headers, (name + ": " + value).c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
  curl_easy_setopt(curl, CURLOPT_PROXY, "");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(std::min<std::uint32_t>(
                       request.timeout_milliseconds, 10'000)));
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                   static_cast<long>(request.timeout_milliseconds));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteHeader);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
  curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, OpenPublicSocket);
  if (!request.body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(request.body.size()));
  }
  const CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK) {
    *diagnostic = transfer.body_overflow
                      ? "HTTPS response exceeded the declared byte limit"
                  : transfer.header_overflow
                      ? "HTTPS response headers exceeded policy"
                      : std::string("HTTPS transport failed: ") +
                            curl_easy_strerror(result);
    return false;
  }
  response->status = status;
  response->headers = std::move(transfer.headers);
  response->body = std::move(transfer.body);
  if (status >= 300 && status < 400) *redirect = transfer.location;
  return true;
}

}  // namespace

bool IsValidHttpsDomain(const std::string& domain) {
  if (domain.empty() || domain.size() > 253 || domain.front() == '.' ||
      domain.back() == '.')
    return false;
  struct in_addr ipv4{};
  struct in6_addr ipv6{};
  if (inet_pton(AF_INET, domain.c_str(), &ipv4) == 1 ||
      inet_pton(AF_INET6, domain.c_str(), &ipv6) == 1)
    return false;
  bool label_start = true;
  bool has_letter = false;
  char previous = '\0';
  for (const unsigned char character : domain) {
    if (character == '.') {
      if (label_start || previous == '-') return false;
      label_start = true;
    } else if (std::isalnum(character)) {
      has_letter = has_letter || std::isalpha(character);
      label_start = false;
    } else if (character == '-' && !label_start) {
      label_start = false;
    } else {
      return false;
    }
    previous = static_cast<char>(character);
  }
  return has_letter && !label_start && previous != '-';
}

bool IsHttpsUrlAllowed(const std::string& url,
                       const std::set<std::string>& allowed_domains,
                       std::string* host, std::string* diagnostic) {
  CURLU* parsed = curl_url();
  if (!parsed) {
    if (diagnostic) *diagnostic = "could not parse the HTTPS URL";
    return false;
  }
  char* scheme = nullptr;
  char* hostname = nullptr;
  char* user = nullptr;
  const bool parsed_ok =
      curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK &&
      curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
      curl_url_get(parsed, CURLUPART_HOST, &hostname, 0) == CURLUE_OK;
  const bool has_user =
      curl_url_get(parsed, CURLUPART_USER, &user, 0) == CURLUE_OK && user &&
      *user;
  std::string normalized = hostname ? Lower(hostname) : std::string();
  const bool allowed = parsed_ok && scheme && Lower(scheme) == "https" &&
                       !has_user && IsValidHttpsDomain(normalized) &&
                       allowed_domains.count(normalized) != 0;
  if (allowed && host) *host = normalized;
  if (!allowed && diagnostic)
    *diagnostic = "URL must use HTTPS and an exact manifest-declared domain";
  if (scheme) curl_free(scheme);
  if (hostname) curl_free(hostname);
  if (user) curl_free(user);
  curl_url_cleanup(parsed);
  return allowed;
}

bool IsPublicNetworkAddress(const void* address, int family) {
  if (!address) return false;
  if (family == AF_INET) {
    const auto* socket_address =
        static_cast<const struct sockaddr_in*>(address);
    const std::uint32_t ip = ntohl(socket_address->sin_addr.s_addr);
    const unsigned first = ip >> 24;
    const unsigned second = (ip >> 16) & 0xff;
    const unsigned third = (ip >> 8) & 0xff;
    return first != 0 && first != 10 &&
           !(first == 100 && second >= 64 && second <= 127) && first != 127 &&
           !(first == 169 && second == 254) &&
           !(first == 172 && second >= 16 && second <= 31) &&
           !(first == 192 && second == 0 && third == 0) &&
           !(first == 192 && second == 0 && third == 2) &&
           !(first == 192 && second == 88 && third == 99) &&
           !(first == 192 && second == 168) &&
           !(first == 198 && (second == 18 || second == 19)) &&
           !(first == 198 && second == 51 && third == 100) &&
           !(first == 203 && second == 0 && third == 113) && first < 224;
  }
  if (family == AF_INET6) {
    const auto* socket_address =
        static_cast<const struct sockaddr_in6*>(address);
    const auto& bytes = socket_address->sin6_addr.s6_addr;
    bool all_zero = true;
    for (int index = 0; index < 16; ++index)
      all_zero = all_zero && bytes[index] == 0;
    if (all_zero) return false;
    const bool loopback =
        std::all_of(bytes, bytes + 15,
                    [](unsigned char value) { return value == 0; }) &&
        bytes[15] == 1;
    const bool ipv4_mapped =
        std::all_of(bytes, bytes + 10,
                    [](unsigned char value) { return value == 0; }) &&
        bytes[10] == 0xff && bytes[11] == 0xff;
    if (ipv4_mapped) {
      struct sockaddr_in ipv4{};
      std::memcpy(&ipv4.sin_addr, bytes + 12, sizeof(ipv4.sin_addr));
      return IsPublicNetworkAddress(&ipv4, AF_INET);
    }
    const bool ipv4_compatible = std::all_of(
        bytes, bytes + 12, [](unsigned char value) { return value == 0; });
    return !loopback && (bytes[0] & 0xfe) != 0xfc &&
           !(bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80) &&
           bytes[0] != 0xff && !ipv4_compatible &&
           !(bytes[0] == 0x20 && bytes[1] == 0x01 && bytes[2] == 0x0d &&
             bytes[3] == 0xb8);
  }
  return false;
}

bool PerformControlledHttps(const HttpsRequest& request,
                            const std::set<std::string>& allowed_domains,
                            HttpsResponse* response, std::string* diagnostic) {
  if (!response || !diagnostic) return false;
  response->headers.clear();
  response->body.clear();
  static std::once_flag curl_once;
  std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  static const std::set<std::string> methods{"GET", "POST", "PUT", "PATCH",
                                             "DELETE"};
  if (methods.count(request.method) == 0 ||
      request.body.size() > kMaximumRequestBytes ||
      request.maximum_response_bytes == 0 ||
      request.maximum_response_bytes > kMaximumResponseBytes ||
      request.timeout_milliseconds < 250 ||
      request.timeout_milliseconds > 30'000 || request.headers.size() > 64) {
    *diagnostic = "HTTPS request exceeds method, size or timeout policy";
    return false;
  }
  HttpsRequest current = request;
  std::string url = request.url;
  std::string previous_host;
  for (int redirect_count = 0; redirect_count <= kMaximumRedirects;
       ++redirect_count) {
    std::string host;
    if (!IsHttpsUrlAllowed(url, allowed_domains, &host, diagnostic))
      return false;
    if (!previous_host.empty() && host != previous_host) {
      current.headers.erase(
          std::remove_if(current.headers.begin(), current.headers.end(),
                         [](const auto& header) {
                           return SensitiveAcrossOrigins(header.first);
                         }),
          current.headers.end());
    }
    previous_host = host;
    std::string redirect;
    if (!PerformOne(current, url, response, &redirect, diagnostic))
      return false;
    if (response->status < 300 || response->status >= 400) {
      response->final_url = url;
      return true;
    }
    if (redirect_count == kMaximumRedirects || redirect.empty()) {
      *diagnostic = "HTTPS redirect limit exceeded or Location is missing";
      return false;
    }
    std::string resolved;
    if (!ResolveRedirect(url, redirect, &resolved)) {
      *diagnostic = "HTTPS redirect URL is invalid";
      return false;
    }
    if (response->status == 303 ||
        ((response->status == 301 || response->status == 302) &&
         current.method == "POST")) {
      current.method = "GET";
      current.body.clear();
    }
    url = std::move(resolved);
  }
  *diagnostic = "HTTPS redirect policy failure";
  return false;
}

}  // namespace ppm
