#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {
bool Stopped(pid_t process) {
  const std::string path = "/proc/" + std::to_string(process) + "/stat";
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return errno == ENOENT;
  char buffer[512]{};
  const ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
  close(fd);
  if (count <= 0) return false;
  const std::string status(buffer, static_cast<size_t>(count));
  const auto close_name = status.rfind(')');
  return close_name != std::string::npos && close_name + 2 < status.size() &&
         (status[close_name + 2] == 'Z' || status[close_name + 2] == 'X');
}

int Helper() {
  const pid_t grandchild = fork();
  if (grandchild < 0) return 2;
  if (grandchild == 0) {
    for (;;) pause();
  }
  std::cout << getpid() << ' ' << grandchild << std::endl;
  for (;;) pause();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--helper") == 0) return Helper();
  if (argc != 1) return 64;

  int output[2]{};
  if (pipe(output) != 0) return 1;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, output[0]);
  posix_spawn_file_actions_addclose(&actions, output[1]);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attributes, 0);
  char mode[] = "--helper";
  char* child_argv[] = {argv[0], mode, nullptr};
  pid_t child = -1;
  const int result = posix_spawn(&child, argv[0], &actions, &attributes,
                                 child_argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  close(output[1]);
  if (result != 0) return 2;

  FILE* stream = fdopen(output[0], "r");
  long reported_child = 0;
  long grandchild = 0;
  if (!stream || fscanf(stream, "%ld %ld", &reported_child, &grandchild) != 2 ||
      reported_child != child) {
    kill(-child, SIGKILL);
    waitpid(child, nullptr, 0);
    return 3;
  }
  fclose(stream);

  if (kill(-child, SIGTERM) != 0) return 4;
  int status = 0;
  if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status)) return 5;
  for (unsigned attempt = 0; attempt != 50; ++attempt) {
    if (Stopped(static_cast<pid_t>(grandchild))) {
      std::cout << "helper supervisor probe passed: process group cancelled"
                << std::endl;
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  kill(static_cast<pid_t>(grandchild), SIGKILL);
  std::cerr << "grandchild remained active after process-group cancellation"
            << std::endl;
  return 6;
}
