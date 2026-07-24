#include "ocpn_portable_runtime.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <set>
#include <string>

namespace {

struct State {
  std::set<std::string> operations;
};

int32_t AuthorCall(void* user_data, const char* operation,
                   std::size_t operation_length, const char*,
                   std::size_t, char* response, std::size_t capacity,
                   std::size_t* response_length) {
  auto* state = static_cast<State*>(user_data);
  if (!state || !operation || !response_length) return -1;
  const std::string name(operation, operation_length);
  state->operations.insert(name);
  std::string value = "{}";
  if (name == "actions.register") value = R"({"host_action_id":1})";
  if (name == "timers.cancel") value = R"({"cancelled":true})";
  if (name == "storage.delete") value = R"({"deleted":true})";
  if (name == "storage.list") value = "[]";
  if (name == "storage.read") value = R"({"base64":""})";
  if (name == "events.subscribe") value = R"({"subscription_id":1})";
  *response_length = value.size();
  if (value.size() > capacity || (!response && !value.empty())) return -2;
  std::memcpy(response, value.data(), value.size());
  return 0;
}

bool CallOkay(int status, const std::array<char, 4096>& error,
              const char* operation) {
  if (status == 0) return true;
  std::cerr << operation << " failed: "
            << (error[0] ? error.data() : "unknown runtime error") << '\n';
  return false;
}

int32_t SettingSet(void* user_data, const char*, std::size_t,
                   const char*, std::size_t) {
  auto* state = static_cast<State*>(user_data);
  if (!state) return -1;
  state->operations.insert("settings.set");
  return 0;
}

int32_t OpenSurface(void* user_data, const char*, std::size_t) {
  auto* state = static_cast<State*>(user_data);
  if (!state) return -1;
  state->operations.insert("surfaces.open");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: portable-fake-host COMPONENT.wasm\n";
    return 2;
  }
  State state;
  ocpn_portable_host_callbacks callbacks{};
  callbacks.abi_version = OCPN_PORTABLE_HOST_ABI_VERSION;
  callbacks.user_data = &state;
  callbacks.setting_set = SettingSet;
  callbacks.open_surface = OpenSurface;
  callbacks.author_service_call = AuthorCall;
  std::array<char, 4096> error{};
  ocpn_portable_runtime* runtime = ocpn_portable_runtime_create(
      argv[1], &callbacks, OCPN_PORTABLE_API_V03,
      OCPN_PORTABLE_WORLD_PLUGIN, error.data(), error.size());
  if (!runtime) {
    std::cerr << "create failed: " << error.data() << '\n';
    return 1;
  }
  const auto destroy = [&]() { ocpn_portable_runtime_destroy(runtime); };
  const std::string id = "org.opencpn.portable-template";
  const std::string name = "Portable Plugin Template";
  const std::string version = "0.1.0";
  if (!CallOkay(ocpn_portable_runtime_initialize(
                     runtime, id.data(), id.size(), name.data(), name.size(),
                     version.data(), version.size(), error.data(),
                     error.size()),
                 error, "initialize") ||
      !CallOkay(ocpn_portable_runtime_enable(runtime, error.data(),
                                             error.size()),
                 error, "enable")) {
    destroy();
    return 1;
  }
  const std::string action = "template.hello";
  std::array<char, 4096> surface_state{};
  std::size_t surface_state_length = 0;
  if (!CallOkay(ocpn_portable_runtime_on_action(
                     runtime, action.data(), action.size(), error.data(),
                     error.size()),
                 error, "on-action") ||
      !CallOkay(ocpn_portable_runtime_on_surface_event(
                     runtime, "template.main", 13, "hello", 5, "{}", 2,
                     surface_state.data(), surface_state.size(),
                     &surface_state_length, error.data(), error.size()),
                 error, "on-surface-event") ||
      !CallOkay(ocpn_portable_runtime_on_timer(
                     runtime, "template.tick", 13, 1000, 1000, error.data(),
                     error.size()),
                 error, "on-timer") ||
      !CallOkay(ocpn_portable_runtime_disable(runtime, error.data(),
                                              error.size()),
                 error, "disable")) {
    destroy();
    return 1;
  }
  destroy();
  const std::set<std::string> required = {
      "actions.register", "scenes.submit", "timers.schedule",
      "rpc.register",     "storage.write-atomic", "scenes.clear",
      "timers.cancel",    "rpc.unregister", "settings.set",
      "surfaces.open"};
  for (const auto& operation : required) {
    if (state.operations.count(operation) == 0) {
      std::cerr << "missing expected host operation: " << operation << '\n';
      return 1;
    }
  }
  if (surface_state_length == 0) {
    std::cerr << "surface callback returned empty state\n";
    return 1;
  }
  std::cout << "API 0.3 fake-host conformance passed\n";
  return 0;
}
