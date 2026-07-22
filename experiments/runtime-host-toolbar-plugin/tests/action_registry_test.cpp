#include "action_registry.h"

#include <cassert>

int main() {
  ActionRegistry registry;
  const LogicalActionKey manager{"org.opencpn.runtime", "manager"};
  const LogicalActionKey igrib{"org.opencpn.igrib", "open"};
  const LogicalActionKey routing{"org.opencpn.routing", "open"};

  assert(registry.Add(manager, 100));
  assert(registry.Add(igrib, 101));
  assert(registry.Add(routing, 102));
  assert(registry.Size() == 3);
  assert(!registry.Add(igrib, 103));
  assert(!registry.Add({"org.opencpn.other", "open"}, 102));

  auto mapped = registry.FindByToolId(101);
  assert(mapped && mapped->key == igrib);
  assert(!registry.FindByToolId(999));

  assert(registry.Remove(igrib));
  assert(!registry.FindByToolId(101));
  assert(registry.Add(igrib, 220));
  mapped = registry.FindByToolId(220);
  assert(mapped && mapped->key == igrib);
  assert(registry.Size() == 3);
  return 0;
}
