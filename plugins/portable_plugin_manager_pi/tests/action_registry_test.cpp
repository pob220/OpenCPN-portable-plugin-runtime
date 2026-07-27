#include "action_registry.h"

#include <iostream>

#define CHECK(expression)                                            \
  do {                                                               \
    if (!(expression)) {                                             \
      std::cerr << "check failed at " << __FILE__ << ':' << __LINE__ \
                << ": " #expression "\n";                            \
      return 1;                                                      \
    }                                                                \
  } while (false)

int main() {
  ppm::ActionRegistry registry;
  const ppm::ActionKey manager{"org.opencpn.runtime", "manager"};
  const ppm::ActionKey igrib{"org.opencpn.igrib", "open"};
  const ppm::ActionKey routing{"org.opencpn.routing", "open"};

  CHECK(registry.Add(manager, 100));
  CHECK(registry.Add(igrib, 101));
  CHECK(registry.Add(routing, 102));
  CHECK(registry.Size() == 3);
  CHECK(!registry.Add(igrib, 103));
  CHECK(!registry.Add({"org.opencpn.other", "open"}, 102));
  CHECK(!registry.Add({"org.opencpn.invalid", "open"}, -1));
  CHECK(!registry.Add({"org.opencpn.invalid", "duplicate-context"}, -1,
                      std::vector<int>{200, 200}));

  auto mapped = registry.FindByToolId(101);
  CHECK(mapped && mapped->key == igrib);
  CHECK(!registry.FindByToolId(999));

  auto* mutable_action = registry.Find(igrib);
  CHECK(mutable_action);
  mutable_action->checked = true;
  mutable_action->dispatchable = false;
  mapped = registry.FindByToolId(101);
  CHECK(mapped && mapped->checked && !mapped->dispatchable);

  CHECK(registry.Remove(igrib));
  CHECK(!registry.FindByToolId(101));
  CHECK(!registry.Remove(igrib));
  CHECK(registry.Add(igrib, 220));
  CHECK(registry.Size() == 3);

  const ppm::ActionKey routing_settings{"org.opencpn.routing", "settings"};
  CHECK(registry.Add(routing_settings, -1, std::vector<int>{221, 222, 223}));
  CHECK(registry.FindByContextId(221)->key == routing_settings);
  CHECK(registry.FindByContextId(222)->key == routing_settings);
  CHECK(registry.FindByContextId(223)->key == routing_settings);
  CHECK(!registry.Add({"org.opencpn.other", "context"}, -1,
                      std::vector<int>{223}));
  const auto package_actions = registry.RemovePackage("org.opencpn.routing");
  CHECK(package_actions.size() == 2);
  CHECK(!registry.FindByToolId(102));
  CHECK(!registry.FindByContextId(221));
  CHECK(!registry.FindByContextId(222));
  CHECK(!registry.FindByContextId(223));
  CHECK(registry.Size() == 2);
  CHECK(registry.RemovePackage("org.opencpn.missing").empty());

  const auto removed = registry.Clear();
  CHECK(removed.size() == 2);
  CHECK(registry.Size() == 0);
  CHECK(!registry.FindByToolId(100));
  return 0;
}
