#include "service_version.h"

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

int main() {
  ppm::ServiceVersion version;
  CHECK(ppm::ParseServiceVersion("0.1.0", &version));
  CHECK(version.major == 0 && version.minor == 1 && version.patch == 0);
  CHECK(!ppm::ParseServiceVersion("01.1.0", &version));
  CHECK(!ppm::ParseServiceVersion("0.1", &version));
  CHECK(!ppm::ParseServiceVersion("0.1.0-beta", &version));
  CHECK(ppm::IsServiceVersionRange(">=0.1.0 <0.2.0"));
  CHECK(ppm::IsServiceVersionRange("0.1.0"));
  CHECK(!ppm::IsServiceVersionRange(">=0.1.0 nonsense"));
  CHECK(ppm::ServiceVersionSatisfies("0.1.0", ">=0.1.0 <0.2.0"));
  CHECK(ppm::ServiceVersionSatisfies("0.1.9", ">=0.1.0 <0.2.0"));
  CHECK(!ppm::ServiceVersionSatisfies("0.2.0", ">=0.1.0 <0.2.0"));
  CHECK(!ppm::ServiceVersionSatisfies("0.0.9", ">=0.1.0 <0.2.0"));
  CHECK(ppm::ServiceVersionSatisfies("1.2.3", "1.2.3"));
  CHECK(!ppm::ServiceVersionSatisfies("1.2.3", ">=1.2.0 nonsense"));
  return 0;
}
