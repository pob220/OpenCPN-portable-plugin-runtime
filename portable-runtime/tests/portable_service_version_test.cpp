#include "service_version.h"

#include <cassert>

using ppm::IsServiceVersionRange;
using ppm::ParseServiceVersion;
using ppm::ServiceVersion;
using ppm::ServiceVersionSatisfies;

int main() {
  ServiceVersion version;
  assert(ParseServiceVersion("0.1.0", &version));
  assert(version.major == 0 && version.minor == 1 && version.patch == 0);
  assert(!ParseServiceVersion("0.1", &version));
  assert(!ParseServiceVersion("00.1.0", &version));
  assert(!ParseServiceVersion("0.1.0-beta", &version));
  assert(IsServiceVersionRange(">=0.1.0 <0.2.0"));
  assert(!IsServiceVersionRange(">=0.1 nonsense"));
  assert(ServiceVersionSatisfies("0.1.0", ">=0.1.0 <0.2.0"));
  assert(ServiceVersionSatisfies("0.1.9", ">=0.1.0 <0.2.0"));
  assert(!ServiceVersionSatisfies("0.2.0", ">=0.1.0 <0.2.0"));
  assert(!ServiceVersionSatisfies("0.0.9", ">=0.1.0 <0.2.0"));
  assert(ServiceVersionSatisfies("1.2.3", "1.2.3"));
  assert(!ServiceVersionSatisfies("1.2.3", ">=1.2.0 nonsense"));
  return 0;
}
