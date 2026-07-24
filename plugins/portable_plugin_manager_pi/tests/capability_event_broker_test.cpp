#include "capability_event_broker.h"

#include <iostream>
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
  using ppm::CapabilityEvent;
  using ppm::CapabilityEventBroker;
  using ppm::CapabilityEventKind;
  using ppm::CapabilityEventSubscription;

  CapabilityEventBroker broker;
  std::string diagnostic;
  CHECK(broker.Subscribe(
      {"org.opencpn.events", CapabilityEventKind::kNmea0183, "$GPGGA", 2},
      &diagnostic));
  CHECK(broker.Subscribe({"org.opencpn.events",
                          CapabilityEventKind::kPluginMessage, "OCPN_DRAW_", 2},
                         &diagnostic));
  CHECK(
      !broker.Subscribe({"org.opencpn.events",
                         CapabilityEventKind::kPluginMessage, "OCPN_DRAW_", 2},
                        &diagnostic));

  CHECK(broker.Publish(
      {CapabilityEventKind::kNmea0183, "$GPRMC", "$GPRMC,ignored"}));
  CHECK(broker.Pending("org.opencpn.events") == 0);
  CHECK(broker.Publish(
      {CapabilityEventKind::kNmea0183, "$GPGGA", "$GPGGA,first"}));
  CHECK(broker.Publish(
      {CapabilityEventKind::kPluginMessage, "OCPN_DRAW_PI", "{\"value\":1}"}));
  CHECK(broker.Publish(
      {CapabilityEventKind::kNmea0183, "$GPGGA", "$GPGGA,last"}));
  const auto bounded = broker.Drain("org.opencpn.events", 10);
  CHECK(bounded.size() == 2);
  CHECK(bounded[0].topic == "OCPN_DRAW_PI");
  CHECK(bounded[1].payload == "$GPGGA,last");
  CHECK(bounded[0].sequence < bounded[1].sequence);
  CHECK(broker.Stats("org.opencpn.events").dropped == 1);

  CHECK(broker.Subscribe(
      {"org.opencpn.coalesce", CapabilityEventKind::kNavigationPosition, "", 1},
      &diagnostic));
  CHECK(broker.Publish(
      {CapabilityEventKind::kNavigationPosition, "ownship", "{\"lat\":1}"}));
  CHECK(broker.Publish(
      {CapabilityEventKind::kNavigationPosition, "ownship", "{\"lat\":2}"}));
  const auto latest = broker.Drain("org.opencpn.coalesce", 1);
  CHECK(latest.size() == 1);
  CHECK(latest[0].payload == "{\"lat\":2}");
  CHECK(broker.Stats("org.opencpn.coalesce").coalesced == 1);

  CHECK(!broker.Publish(
      {CapabilityEventKind::kPluginMessage, "oversize",
       std::string(CapabilityEventBroker::kMaximumPayloadBytes + 1, 'x')},
      &diagnostic));
  CHECK(diagnostic == "event payload exceeds policy");

  CapabilityEventKind parsed;
  CHECK(ppm::ParseCapabilityEventKind("navigation.nmea2000", &parsed));
  CHECK(parsed == CapabilityEventKind::kNmea2000);
  CHECK(std::string(ppm::CapabilityEventPermission(parsed)) ==
        "navigation.nmea2000.read");
  CHECK(!ppm::ParseCapabilityEventKind("native.pointer", &parsed));

  broker.RemovePackage("org.opencpn.events");
  CHECK(broker.Pending("org.opencpn.events") == 0);
  std::cout << "capability event broker test passed\n";
  return 0;
}
