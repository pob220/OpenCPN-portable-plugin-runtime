#include "https_service.h"

#include <arpa/inet.h>

#include <iostream>
#include <set>
#include <string>

#define CHECK(expression)                                            \
  do {                                                               \
    if (!(expression)) {                                             \
      std::cerr << "check failed at " << __FILE__ << ':' << __LINE__ \
                << ": " #expression "\n";                            \
      return 1;                                                      \
    }                                                                \
  } while (false)

int main() {
  CHECK(ppm::IsValidHttpsDomain("api.example.org"));
  CHECK(!ppm::IsValidHttpsDomain("*.example.org"));
  CHECK(!ppm::IsValidHttpsDomain("-api.example.org"));
  CHECK(!ppm::IsValidHttpsDomain("127.0.0.1"));

  std::string host;
  std::string diagnostic;
  CHECK(ppm::IsHttpsUrlAllowed("https://api.example.org/v1",
                               std::set<std::string>{"api.example.org"}, &host,
                               &diagnostic));
  CHECK(host == "api.example.org");
  CHECK(!ppm::IsHttpsUrlAllowed("http://api.example.org/v1",
                                std::set<std::string>{"api.example.org"}, &host,
                                &diagnostic));
  CHECK(!ppm::IsHttpsUrlAllowed("https://other.example.org/v1",
                                std::set<std::string>{"api.example.org"}, &host,
                                &diagnostic));

  sockaddr_in ipv4{};
  ipv4.sin_family = AF_INET;
  CHECK(inet_pton(AF_INET, "8.8.8.8", &ipv4.sin_addr) == 1);
  CHECK(ppm::IsPublicNetworkAddress(&ipv4, AF_INET));
  CHECK(inet_pton(AF_INET, "127.0.0.1", &ipv4.sin_addr) == 1);
  CHECK(!ppm::IsPublicNetworkAddress(&ipv4, AF_INET));
  CHECK(inet_pton(AF_INET, "192.168.1.3", &ipv4.sin_addr) == 1);
  CHECK(!ppm::IsPublicNetworkAddress(&ipv4, AF_INET));
  CHECK(inet_pton(AF_INET, "100.64.0.1", &ipv4.sin_addr) == 1);
  CHECK(!ppm::IsPublicNetworkAddress(&ipv4, AF_INET));
  CHECK(inet_pton(AF_INET, "198.51.100.10", &ipv4.sin_addr) == 1);
  CHECK(!ppm::IsPublicNetworkAddress(&ipv4, AF_INET));

  sockaddr_in6 ipv6{};
  ipv6.sin6_family = AF_INET6;
  CHECK(inet_pton(AF_INET6, "2606:4700:4700::1111", &ipv6.sin6_addr) == 1);
  CHECK(ppm::IsPublicNetworkAddress(&ipv6, AF_INET6));
  CHECK(inet_pton(AF_INET6, "::1", &ipv6.sin6_addr) == 1);
  CHECK(!ppm::IsPublicNetworkAddress(&ipv6, AF_INET6));
  CHECK(inet_pton(AF_INET6, "::ffff:127.0.0.1", &ipv6.sin6_addr) == 1);
  CHECK(!ppm::IsPublicNetworkAddress(&ipv6, AF_INET6));
  CHECK(inet_pton(AF_INET6, "::ffff:8.8.8.8", &ipv6.sin6_addr) == 1);
  CHECK(ppm::IsPublicNetworkAddress(&ipv6, AF_INET6));
  CHECK(inet_pton(AF_INET6, "2001:db8::1", &ipv6.sin6_addr) == 1);
  CHECK(!ppm::IsPublicNetworkAddress(&ipv6, AF_INET6));
  return 0;
}
