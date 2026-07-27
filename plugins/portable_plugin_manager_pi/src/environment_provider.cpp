#include "environment_provider.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <wx/datetime.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/sstream.h>

#include "bounded_parallel.h"

namespace ppm {
namespace {

namespace fs = std::filesystem;

constexpr std::uintmax_t kMaximumDatasetBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uintmax_t kMaximumJsonBytes = 32ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumRequests = 100'000;
// Each requested instant is decoded as a complete spatial frame. Eight keeps
// the helper comfortably below its 512 MiB address-space sandbox even for the
// dense UKV/wave/current datasets used during 15-minute route validation.
constexpr std::size_t kMaximumDecodeTimes = 8;
constexpr std::size_t kFrameCacheBudget = 256U * 1024U * 1024U;

bool IsFlatpak() {
  const char* value = std::getenv("FLATPAK_ID");
  return value && *value;
}

std::string HelperTarget() {
#if defined(_WIN32)
  return "windows-x86_64";
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
  return "macos-aarch64";
#else
  return "macos-x86_64";
#endif
#elif defined(__aarch64__)
  return IsFlatpak() ? "flatpak-aarch64" : "linux-gnu-aarch64";
#else
  return IsFlatpak() ? "flatpak-x86_64" : "linux-gnu-x86_64";
#endif
}

std::string TimeKey(std::int64_t unix_time) {
  wxDateTime value(static_cast<time_t>(unix_time));
  return value.ToUTC().Format("%Y%m%dT%H%MZ").ToStdString();
}

std::int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned adjusted_month =
      static_cast<unsigned>(static_cast<int>(month) + (month > 2 ? -3 : 9));
  const unsigned day_of_year = (153 * adjusted_month + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<std::int64_t>(era) * 146097 +
         static_cast<std::int64_t>(day_of_era) - 719468;
}

bool TimeKeyMinutes(const std::string& key, std::int64_t* minutes) {
  if (!minutes || key.size() != 14 || key[8] != 'T' || key[13] != 'Z')
    return false;
  auto number = [&key](std::size_t offset, std::size_t length, int* value) {
    int parsed = 0;
    for (std::size_t index = offset; index < offset + length; ++index) {
      if (key[index] < '0' || key[index] > '9') return false;
      parsed = parsed * 10 + key[index] - '0';
    }
    *value = parsed;
    return true;
  };
  int year = 0, month = 0, day = 0, hour = 0, minute = 0;
  if (!number(0, 4, &year) || !number(4, 2, &month) || !number(6, 2, &day) ||
      !number(9, 2, &hour) || !number(11, 2, &minute) || month < 1 ||
      month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59)
    return false;
  *minutes = DaysFromCivil(year, static_cast<unsigned>(month),
                           static_cast<unsigned>(day)) *
                 1440 +
             hour * 60 + minute;
  return true;
}

bool ReadJson(const fs::path& path, wxJSONValue* value,
              std::string* diagnostic) {
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error || size == 0 || size > kMaximumJsonBytes) {
    if (diagnostic) *diagnostic = "decoder JSON result is missing or oversized";
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  std::string contents(static_cast<std::size_t>(size), '\0');
  input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  wxStringInputStream stream(wxString::FromUTF8(contents));
  wxJSONReader reader;
  if (!input || reader.Parse(stream, value) != 0 || !value->IsObject()) {
    if (diagnostic) *diagnostic = "decoder returned invalid JSON";
    return false;
  }
  const wxJSONValue failure = value->ItemAt("error");
  if (failure.IsObject()) {
    const wxJSONValue message = failure.ItemAt("message");
    if (diagnostic) {
      *diagnostic = message.IsString() ? message.AsString().ToStdString()
                                       : "environment decoder failed";
    }
    return false;
  }
  return true;
}

bool RunProcess(const std::vector<std::string>& arguments,
                const EnvironmentProvider::Cancelled& cancelled,
                std::string* diagnostic) {
#if defined(__linux__) || defined(__APPLE__)
  if (arguments.empty()) {
    if (diagnostic) *diagnostic = "invalid decoder command";
    return false;
  }
  std::vector<std::string> encoded = arguments;
  std::vector<char*> argv;
  argv.reserve(encoded.size() + 1);
  for (auto& argument : encoded) argv.push_back(argument.data());
  argv.push_back(nullptr);
  int stderr_pipe[2] = {-1, -1};
  posix_spawn_file_actions_t file_actions;
  bool file_actions_initialized = false;
  bool capture_stderr = pipe(stderr_pipe) == 0;
  if (capture_stderr) {
    const int flags = fcntl(stderr_pipe[0], F_GETFL, 0);
    capture_stderr = flags >= 0 &&
                     fcntl(stderr_pipe[0], F_SETFL, flags | O_NONBLOCK) == 0 &&
                     posix_spawn_file_actions_init(&file_actions) == 0;
    file_actions_initialized = capture_stderr;
  }
  if (capture_stderr) {
    capture_stderr =
        posix_spawn_file_actions_adddup2(&file_actions, stderr_pipe[1],
                                         STDERR_FILENO) == 0 &&
        posix_spawn_file_actions_addclose(&file_actions, stderr_pipe[0]) == 0 &&
        posix_spawn_file_actions_addclose(&file_actions, stderr_pipe[1]) == 0;
  }
  if (!capture_stderr) {
    if (file_actions_initialized) {
      posix_spawn_file_actions_destroy(&file_actions);
      file_actions_initialized = false;
    }
    if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
    if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
    stderr_pipe[0] = stderr_pipe[1] = -1;
  }
  pid_t child = -1;
  const int spawn_error = posix_spawn(&child, argv.front(),
                                      capture_stderr ? &file_actions : nullptr,
                                      nullptr, argv.data(), environ);
  if (file_actions_initialized) {
    posix_spawn_file_actions_destroy(&file_actions);
  }
  if (capture_stderr) {
    close(stderr_pipe[1]);
    stderr_pipe[1] = -1;
  }
  if (spawn_error != 0) {
    if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
    if (diagnostic) {
      *diagnostic = "could not start supervised decoder: " +
                    std::string(std::strerror(spawn_error));
    }
    return false;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::minutes(2);
  int status = 0;
  pid_t waited = -1;
  bool stopping = false;
  std::string child_error;
  auto drain_stderr = [&]() {
    if (stderr_pipe[0] < 0) return;
    std::array<char, 2048> buffer{};
    for (;;) {
      const ssize_t count = read(stderr_pipe[0], buffer.data(), buffer.size());
      if (count > 0) {
        constexpr std::size_t kMaximumCapturedError = 8192;
        const std::size_t available =
            child_error.size() < kMaximumCapturedError
                ? kMaximumCapturedError - child_error.size()
                : 0;
        child_error.append(buffer.data(),
                           std::min<std::size_t>(available, count));
      } else if (count == 0 ||
                 (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        break;
      }
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) break;
    }
  };
  for (;;) {
    waited = waitpid(child, &status, WNOHANG);
    drain_stderr();
    if (waited == child) break;
    if (waited < 0 && errno != EINTR) break;
    const bool timed_out = std::chrono::steady_clock::now() >= deadline;
    if ((cancelled && cancelled()) || timed_out) {
      stopping = true;
      kill(child, SIGTERM);
      const auto grace =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
      do {
        waited = waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      } while (std::chrono::steady_clock::now() < grace);
      if (waited != child) {
        kill(child, SIGKILL);
        do {
          waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
      }
      if (diagnostic) {
        *diagnostic = timed_out
                          ? "contained decoder exceeded its two-minute deadline"
                          : "contained decoder was cancelled";
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  drain_stderr();
  if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
  child_error.erase(
      std::remove_if(child_error.begin(), child_error.end(),
                     [](unsigned char character) {
                       return character != '\n' && character != '\t' &&
                              (character < 0x20 || character == 0x7f);
                     }),
      child_error.end());
  while (!child_error.empty() &&
         std::isspace(static_cast<unsigned char>(child_error.back()))) {
    child_error.pop_back();
  }
  if (waited != child) {
    if (diagnostic) {
      *diagnostic = "could not wait for supervised decoder: " +
                    std::string(std::strerror(errno));
    }
    return false;
  }
  if (stopping) return false;
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (diagnostic) {
      *diagnostic = WIFSIGNALED(status)
                        ? "contained decoder terminated by signal " +
                              std::to_string(WTERMSIG(status))
                        : "contained decoder exited with status " +
                              std::to_string(WEXITSTATUS(status));
      if (!child_error.empty()) *diagnostic += ": " + child_error;
    }
    return false;
  }
  return true;
#else
  (void)arguments;
  (void)cancelled;
  if (diagnostic) {
    *diagnostic =
        "contained environmental helper execution is not implemented on this "
        "platform yet";
  }
  return false;
#endif
}

std::size_t EstimatedBytes(const EnvironmentFrame& frame) {
  std::size_t bytes = sizeof(frame);
  for (const auto& [name, samples] : frame.fields) {
    bytes += name.size() + samples.capacity() * sizeof(EnvironmentGridSample);
  }
  for (const auto& [name, value] : frame.units) {
    bytes += name.size() + value.size();
  }
  for (const auto& [name, value] : frame.source_times) {
    bytes += name.size() + value.size();
  }
  for (const auto& [name, index] : frame.spatial_indices) {
    bytes += name.size() + index.EstimatedBytes();
  }
  return bytes;
}

std::string HexEncode(const std::string& value) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (const unsigned char byte : value) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 0x0f]);
  }
  return result;
}

bool HexDecode(const std::string& value, std::string* output) {
  if (!output || value.size() % 2 != 0) return false;
  auto digit = [](unsigned char character) -> int {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
  };
  output->clear();
  output->reserve(value.size() / 2);
  for (std::size_t index = 0; index < value.size(); index += 2) {
    const int high = digit(static_cast<unsigned char>(value[index]));
    const int low = digit(static_cast<unsigned char>(value[index + 1]));
    if (high < 0 || low < 0) return false;
    output->push_back(static_cast<char>((high << 4) | low));
  }
  return true;
}

}  // namespace

class EnvironmentProvider::Impl {
public:
  struct Dataset {
    std::uint64_t revision = 0;
    std::uintmax_t byte_size = 0;
    fs::path source;
    fs::path index;
    fs::path state;
    std::string display_name;
    std::vector<std::string> times;
    mutable std::atomic_bool preserve{true};
    mutable std::mutex decode_mutex;
    mutable std::map<std::string, std::shared_ptr<const EnvironmentFrame>>
        frames;
    mutable std::map<std::string, std::size_t> frame_bytes;
    mutable std::list<std::string> lru;
    mutable std::size_t cache_bytes = 0;

    ~Dataset() {
      if (preserve.load()) return;
      std::error_code ignored;
      fs::remove(source, ignored);
      fs::remove(index, ignored);
      fs::remove(state, ignored);
    }
  };

  Impl(std::string package_root, std::string private_root)
      : package_root(std::move(package_root)),
        private_root(std::move(private_root)) {
    RestoreLatest();
  }

  void RestoreLatest() {
    std::error_code error;
    fs::create_directories(private_root, error);
    if (error) return;
    std::vector<std::pair<std::uint64_t, fs::path>> states;
    for (const auto& item : fs::directory_iterator(private_root, error)) {
      if (error || !item.is_regular_file()) continue;
      const std::string name = item.path().filename().string();
      constexpr const char* prefix = "dataset-";
      constexpr const char* suffix = ".state";
      if (name.rfind(prefix, 0) != 0 ||
          name.size() <= std::strlen(prefix) + std::strlen(suffix) ||
          name.substr(name.size() - std::strlen(suffix)) != suffix) {
        continue;
      }
      const std::string revision_text =
          name.substr(std::strlen(prefix),
                      name.size() - std::strlen(prefix) - std::strlen(suffix));
      char* end = nullptr;
      const auto revision = std::strtoull(revision_text.c_str(), &end, 10);
      if (end && *end == '\0') states.emplace_back(revision, item.path());
    }
    std::sort(states.begin(), states.end(),
              [](const auto& left, const auto& right) {
                return left.first > right.first;
              });
    for (const auto& [revision, state] : states) {
      std::ifstream input(state);
      std::string line;
      std::uintmax_t declared_size = 0;
      std::string display_name;
      std::vector<std::string> times;
      bool valid = static_cast<bool>(input);
      while (valid && std::getline(input, line)) {
        if (line.rfind("size=", 0) == 0) {
          char* end = nullptr;
          declared_size = std::strtoull(line.c_str() + 5, &end, 10);
          valid = end && *end == '\0';
        } else if (line.rfind("display=", 0) == 0) {
          valid = HexDecode(line.substr(8), &display_name);
        } else if (line.rfind("time=", 0) == 0) {
          const std::string time = line.substr(5);
          valid = time.size() == 14;
          if (valid) times.push_back(time);
        } else if (line != "format=1") {
          valid = false;
        }
      }
      const fs::path source =
          private_root / ("dataset-" + std::to_string(revision) + ".grb");
      const fs::path index =
          private_root / ("index-" + std::to_string(revision) + ".json");
      error.clear();
      if (!valid || declared_size == 0 || times.empty() ||
          !fs::is_regular_file(source, error) ||
          fs::file_size(source, error) != declared_size || error ||
          !fs::is_regular_file(index, error) || error) {
        continue;
      }
      auto restored = std::make_shared<Dataset>();
      restored->revision = revision;
      restored->byte_size = declared_size;
      restored->source = source;
      restored->index = index;
      restored->state = state;
      restored->display_name =
          display_name.empty() ? source.filename().string() : display_name;
      std::sort(times.begin(), times.end());
      times.erase(std::unique(times.begin(), times.end()), times.end());
      restored->times = std::move(times);
      dataset = std::move(restored);
      return;
    }
  }

  bool WriteState(const Dataset& value, std::string* diagnostic) const {
    const fs::path temporary = value.state.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    output << "format=1\n"
           << "size=" << value.byte_size << '\n'
           << "display=" << HexEncode(value.display_name) << '\n';
    for (const auto& time : value.times) output << "time=" << time << '\n';
    output.close();
    if (!output) {
      if (diagnostic) *diagnostic = "could not persist dataset state";
      std::error_code ignored;
      fs::remove(temporary, ignored);
      return false;
    }
    std::error_code error;
    fs::rename(temporary, value.state, error);
    if (error) {
      if (diagnostic) {
        *diagnostic = "could not publish dataset state: " + error.message();
      }
      fs::remove(temporary, error);
      return false;
    }
    return true;
  }

  std::vector<std::string> DecoderCommand(const fs::path& input,
                                          const fs::path& index,
                                          const fs::path& output,
                                          const std::vector<std::string>& times,
                                          bool inspect,
                                          std::string* diagnostic) const {
    fs::path helper =
        package_root / "helpers" / HelperTarget() / "igrib-environment-helper";
#if defined(_WIN32)
    helper += ".exe";
#endif
    if (!fs::is_regular_file(helper)) {
      if (diagnostic) {
        *diagnostic = "signed environmental decoder is absent for target " +
                      HelperTarget();
      }
      return {};
    }
#if defined(__linux__)
    if (!fs::is_regular_file("/usr/bin/prlimit")) {
      if (diagnostic) *diagnostic = "Linux decoder requires prlimit";
      return {};
    }
    std::vector<std::string> command = {"/usr/bin/prlimit", "--as=536870912",
                                        "--cpu=30", "--"};
    if (IsFlatpak()) {
      command.insert(
          command.end(),
          {helper.string(), inspect ? "inspect-indexed" : "frames-indexed-bin",
           input.string(), index.string()});
      if (!inspect) {
        command.push_back("12000");
        command.push_back(output.string());
        command.insert(command.end(), times.begin(), times.end());
      } else {
        command.push_back(output.string());
      }
      return command;
    }
    if (!fs::is_regular_file("/usr/bin/bwrap")) {
      if (diagnostic) *diagnostic = "Linux decoder requires bubblewrap";
      return {};
    }
    command.insert(command.end(),
                   {"/usr/bin/bwrap",
                    "--die-with-parent",
                    "--new-session",
                    "--unshare-user",
                    "--unshare-pid",
                    "--unshare-ipc",
                    "--unshare-uts",
                    "--ro-bind",
                    "/usr",
                    "/usr",
                    "--ro-bind",
                    "/lib",
                    "/lib",
                    "--ro-bind",
                    "/lib64",
                    "/lib64",
                    "--ro-bind",
                    helper.parent_path().string(),
                    "/helper",
                    "--ro-bind",
                    input.string(),
                    "/input.grb",
                    "--bind",
                    private_root.string(),
                    "/output",
                    "/helper/" + helper.filename().string(),
                    inspect ? "inspect-indexed" : "frames-indexed-bin",
                    "/input.grb",
                    "/output/" + index.filename().string()});
    if (!inspect) {
      command.push_back("12000");
      command.push_back("/output/" + output.filename().string());
      command.insert(command.end(), times.begin(), times.end());
    } else {
      command.push_back("/output/" + output.filename().string());
    }
    return command;
#else
    (void)times;
    (void)inspect;
    if (diagnostic) {
      *diagnostic =
          "environment decoder command is not implemented for this platform";
    }
    return {};
#endif
  }

  bool OpenDataset(const std::vector<std::string>& selected_paths,
                   const Cancelled& cancelled, std::string* diagnostic) {
    if (selected_paths.size() != 1) {
      if (diagnostic) {
        *diagnostic =
            "select one combined GRIB dataset for routing; multiple-file "
            "composition is not available yet";
      }
      return false;
    }
    const fs::path selected = fs::absolute(selected_paths.front());
    std::error_code error;
    const auto size = fs::file_size(selected, error);
    if (error || size == 0 || size > kMaximumDatasetBytes) {
      if (diagnostic) {
        *diagnostic =
            "selected GRIB is missing, empty, or exceeds the 1 GiB limit";
      }
      return false;
    }
    fs::create_directories(private_root, error);
    if (error) {
      if (diagnostic) *diagnostic = error.message();
      return false;
    }
    const auto revision = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path snapshot =
        private_root / ("dataset-" + std::to_string(revision) + ".grb");
    const fs::path temporary = snapshot.string() + ".tmp";
    fs::copy_file(selected, temporary, fs::copy_options::overwrite_existing,
                  error);
    if (error) {
      if (diagnostic)
        *diagnostic = "could not stage immutable GRIB: " + error.message();
      fs::remove(temporary, error);
      return false;
    }
    fs::rename(temporary, snapshot, error);
    if (error) {
      if (diagnostic)
        *diagnostic = "could not publish immutable GRIB: " + error.message();
      fs::remove(temporary, error);
      return false;
    }
    const fs::path index =
        private_root / ("index-" + std::to_string(revision) + ".json");
    const fs::path inspection =
        private_root / ("inspect-" + std::to_string(revision) + ".json");
    const auto command =
        DecoderCommand(snapshot, index, inspection, {}, true, diagnostic);
    if (command.empty() || !RunProcess(command, cancelled, diagnostic)) {
      fs::remove(snapshot, error);
      fs::remove(index, error);
      fs::remove(inspection, error);
      return false;
    }
    wxJSONValue document;
    if (!ReadJson(inspection, &document, diagnostic)) {
      fs::remove(snapshot, error);
      fs::remove(index, error);
      fs::remove(inspection, error);
      return false;
    }
    const wxJSONValue times = document.ItemAt("times");
    if (!times.IsArray() || times.Size() == 0 || times.Size() > 10000) {
      if (diagnostic)
        *diagnostic = "GRIB contains no bounded forecast timeline";
      fs::remove(snapshot, error);
      fs::remove(index, error);
      fs::remove(inspection, error);
      return false;
    }
    auto next = std::make_shared<Dataset>();
    next->revision = revision;
    next->byte_size = size;
    next->source = snapshot;
    next->index = index;
    next->state =
        private_root / ("dataset-" + std::to_string(revision) + ".state");
    next->display_name = selected.filename().string();
    next->times.reserve(times.Size());
    std::set<std::string> unique_times;
    for (int item = 0; item < times.Size(); ++item) {
      const wxJSONValue value = times.ItemAt(item);
      if (!value.IsString()) continue;
      const std::string key = value.AsString().ToStdString();
      if (key.size() == 14 && unique_times.insert(key).second) {
        next->times.push_back(key);
      }
    }
    if (next->times.empty()) {
      if (diagnostic) *diagnostic = "GRIB forecast timeline is malformed";
      fs::remove(snapshot, error);
      fs::remove(index, error);
      fs::remove(inspection, error);
      return false;
    }
    std::sort(next->times.begin(), next->times.end());
    fs::remove(inspection, error);
    if (!WriteState(*next, diagnostic)) {
      fs::remove(snapshot, error);
      fs::remove(index, error);
      return false;
    }
    std::shared_ptr<const Dataset> old;
    {
      std::lock_guard<std::mutex> lock(dataset_mutex);
      old = std::move(dataset);
      dataset = std::move(next);
    }
    if (old) old->preserve.store(false);
    if (diagnostic) diagnostic->clear();
    return true;
  }

  bool SampleBatch(const std::vector<EnvironmentRequest>& requests,
                   std::vector<EnvironmentSample>* samples,
                   const Cancelled& cancelled, std::string* diagnostic) const {
    if (!samples || requests.size() > kMaximumRequests) {
      if (diagnostic) *diagnostic = "invalid environmental sample batch";
      return false;
    }
    std::shared_ptr<const Dataset> snapshot;
    {
      std::lock_guard<std::mutex> lock(dataset_mutex);
      snapshot = dataset;
    }
    if (!snapshot) {
      if (diagnostic) {
        *diagnostic = "iGRIB has no selected decoded GRIB dataset";
      }
      return false;
    }
    std::error_code error;
    if (fs::file_size(snapshot->source, error) != snapshot->byte_size ||
        error) {
      if (diagnostic) {
        *diagnostic = "immutable environmental dataset changed or vanished";
      }
      return false;
    }
    std::map<std::string, std::vector<std::size_t>> by_time;
    for (std::size_t index = 0; index < requests.size(); ++index) {
      by_time[TimeKey(requests[index].unix_time)].push_back(index);
    }
    samples->assign(requests.size(), EnvironmentSample{});
    std::map<std::string, std::shared_ptr<const EnvironmentFrame>>
        resolved_frames;
    {
      std::lock_guard<std::mutex> decode_lock(snapshot->decode_mutex);
      struct FrameBracket {
        std::string lower;
        std::string upper;
        double factor = 0.0;
      };
      std::map<std::string, FrameBracket> brackets;
      std::set<std::string> required_catalogue_times;
      for (const auto& [time, ignored] : by_time) {
        (void)ignored;
        const auto cached = snapshot->frames.find(time);
        if (cached != snapshot->frames.end()) {
          resolved_frames[time] = cached->second;
          snapshot->lru.remove(time);
          snapshot->lru.push_front(time);
          continue;
        }
        std::int64_t requested_minutes = 0;
        std::int64_t first_minutes = 0;
        std::int64_t last_minutes = 0;
        if (!TimeKeyMinutes(time, &requested_minutes) ||
            !TimeKeyMinutes(snapshot->times.front(), &first_minutes) ||
            !TimeKeyMinutes(snapshot->times.back(), &last_minutes)) {
          if (diagnostic)
            *diagnostic = "environmental forecast timeline is malformed";
          return false;
        }
        constexpr std::int64_t kNearestLimitMinutes = 180;
        if (requested_minutes < first_minutes - kNearestLimitMinutes ||
            requested_minutes > last_minutes + kNearestLimitMinutes)
          continue;
        auto upper = std::lower_bound(snapshot->times.begin(),
                                      snapshot->times.end(), time);
        auto lower = upper;
        if (upper == snapshot->times.end()) {
          lower = std::prev(snapshot->times.end());
          upper = lower;
        } else if (upper != snapshot->times.begin() && *upper != time) {
          lower = std::prev(upper);
        }
        FrameBracket bracket{*lower, *upper, 0.0};
        if (lower != upper) {
          std::int64_t lower_minutes = 0;
          std::int64_t upper_minutes = 0;
          if (!TimeKeyMinutes(*lower, &lower_minutes) ||
              !TimeKeyMinutes(*upper, &upper_minutes) ||
              upper_minutes <= lower_minutes) {
            if (diagnostic) {
              *diagnostic = "environmental forecast timeline is malformed";
            }
            return false;
          }
          bracket.factor =
              static_cast<double>(requested_minutes - lower_minutes) /
              static_cast<double>(upper_minutes - lower_minutes);
        }
        brackets[time] = bracket;
        required_catalogue_times.insert(bracket.lower);
        required_catalogue_times.insert(bracket.upper);
      }
      std::map<std::string, std::shared_ptr<const EnvironmentFrame>>
          catalogue_frames;
      std::vector<std::string> missing;
      for (const auto& time : required_catalogue_times) {
        const auto cached = snapshot->frames.find(time);
        if (cached == snapshot->frames.end())
          missing.push_back(time);
        else
          catalogue_frames[time] = cached->second;
      }
      auto cache_frame = [&](const std::string& key,
                             std::shared_ptr<const EnvironmentFrame> value) {
        const std::size_t bytes = EstimatedBytes(*value);
        const auto old = snapshot->frame_bytes.find(key);
        if (old != snapshot->frame_bytes.end())
          snapshot->cache_bytes -= old->second;
        snapshot->frames[key] = value;
        snapshot->frame_bytes[key] = bytes;
        snapshot->cache_bytes += bytes;
        snapshot->lru.remove(key);
        snapshot->lru.push_front(key);
      };
      for (std::size_t offset = 0; offset < missing.size();
           offset += kMaximumDecodeTimes) {
        if (stop.load() || (cancelled && cancelled())) {
          if (diagnostic) *diagnostic = "environment sampling was cancelled";
          return false;
        }
        const auto end = std::min(missing.size(), offset + kMaximumDecodeTimes);
        const std::vector<std::string> times(missing.begin() + offset,
                                             missing.begin() + end);
        const fs::path output =
            private_root / ("frames-" + std::to_string(snapshot->revision) +
                            "-" + std::to_string(offset) + ".bin");
        const auto command = DecoderCommand(snapshot->source, snapshot->index,
                                            output, times, false, diagnostic);
        if (command.empty()) {
          fs::remove(output, error);
          return false;
        }
        if (!RunProcess(command, cancelled, diagnostic)) {
          const std::string process_diagnostic =
              diagnostic ? *diagnostic : std::string();
          wxJSONValue failure;
          std::string decoder_diagnostic;
          ReadJson(output, &failure, &decoder_diagnostic);
          if (diagnostic && !decoder_diagnostic.empty()) {
            *diagnostic = process_diagnostic;
            if (!diagnostic->empty()) *diagnostic += ": ";
            *diagnostic += decoder_diagnostic;
          }
          fs::remove(output, error);
          return false;
        }
        std::vector<EnvironmentFrame> decoded;
        if (!ReadEnvironmentFrames(output.string(), &decoded, diagnostic)) {
          fs::remove(output, error);
          return false;
        }
        fs::remove(output, error);
        for (auto& frame : decoded) {
          const std::string key = frame.time;
          auto value =
              std::make_shared<const EnvironmentFrame>(std::move(frame));
          catalogue_frames[key] = value;
          cache_frame(key, std::move(value));
        }
        while (snapshot->lru.size() > 1 &&
               snapshot->cache_bytes > kFrameCacheBudget) {
          const std::string key = snapshot->lru.back();
          snapshot->lru.pop_back();
          const auto bytes = snapshot->frame_bytes.find(key);
          if (bytes != snapshot->frame_bytes.end()) {
            snapshot->cache_bytes -= bytes->second;
            snapshot->frame_bytes.erase(bytes);
          }
          snapshot->frames.erase(key);
        }
      }
      for (const auto& [time, bracket] : brackets) {
        const auto first = catalogue_frames.find(bracket.lower);
        const auto second = catalogue_frames.find(bracket.upper);
        if (first == catalogue_frames.end() || second == catalogue_frames.end())
          continue;
        auto frame =
            InterpolateEnvironmentFrames(first->second, second->second, time,
                                         std::clamp(bracket.factor, 0.0, 1.0));
        if (!frame) continue;
        resolved_frames[time] = frame;
        cache_frame(time, std::move(frame));
      }
      while (snapshot->lru.size() > 1 &&
             snapshot->cache_bytes > kFrameCacheBudget) {
        const std::string key = snapshot->lru.back();
        snapshot->lru.pop_back();
        const auto bytes = snapshot->frame_bytes.find(key);
        if (bytes != snapshot->frame_bytes.end()) {
          snapshot->cache_bytes -= bytes->second;
          snapshot->frame_bytes.erase(bytes);
        }
        snapshot->frames.erase(key);
      }
    }
    // Immutable resolved frames are sampled after releasing the decoder/cache
    // mutex. Parallel departure searches therefore share catalogue frames
    // without serialising their much larger spatial request batches.
    struct SamplingTask {
      std::shared_ptr<const EnvironmentFrame> frame;
      std::size_t request = 0;
    };
    std::vector<SamplingTask> sampling_tasks;
    sampling_tasks.reserve(requests.size());
    for (const auto& [time, indices] : by_time) {
      const auto found = resolved_frames.find(time);
      if (found == resolved_frames.end()) continue;
      for (const auto index : indices)
        sampling_tasks.push_back({found->second, index});
    }
    BoundedParallelFor(sampling_tasks.size(), 256,
                       [&](const std::size_t task_index) {
                         const auto& task = sampling_tasks[task_index];
                         (*samples)[task.request] = SampleEnvironmentFrame(
                             *task.frame, requests[task.request].latitude,
                             requests[task.request].longitude);
                       });
    if (diagnostic) diagnostic->clear();
    return true;
  }

  std::string Summary() const {
    std::lock_guard<std::mutex> lock(dataset_mutex);
    if (!dataset) return "No environmental dataset is open";
    return dataset->display_name + " (" +
           std::to_string(dataset->times.size()) + " forecast times)";
  }

  std::string Generation() const {
    std::lock_guard<std::mutex> lock(dataset_mutex);
    return dataset ? std::to_string(dataset->revision) : std::string{};
  }

  fs::path package_root;
  fs::path private_root;
  mutable std::mutex dataset_mutex;
  std::shared_ptr<const Dataset> dataset;
  std::atomic_bool stop{false};
};

EnvironmentProvider::EnvironmentProvider(std::string package_root,
                                         std::string private_root)
    : impl_(std::make_unique<Impl>(std::move(package_root),
                                   std::move(private_root))) {}

EnvironmentProvider::~EnvironmentProvider() = default;

bool EnvironmentProvider::OpenDataset(
    const std::vector<std::string>& selected_paths, const Cancelled& cancelled,
    std::string* diagnostic) {
  impl_->stop.store(false);
  return impl_->OpenDataset(selected_paths, cancelled, diagnostic);
}

bool EnvironmentProvider::SampleBatch(
    const std::vector<EnvironmentRequest>& requests,
    std::vector<EnvironmentSample>* samples, const Cancelled& cancelled,
    std::string* diagnostic) const {
  return impl_->SampleBatch(
      requests, samples,
      [this, &cancelled]() {
        return impl_->stop.load() || (cancelled && cancelled());
      },
      diagnostic);
}

std::string EnvironmentProvider::Summary() const { return impl_->Summary(); }

std::string EnvironmentProvider::Generation() const {
  return impl_->Generation();
}

bool EnvironmentProvider::Available() const {
  std::lock_guard<std::mutex> lock(impl_->dataset_mutex);
  return static_cast<bool>(impl_->dataset);
}

void EnvironmentProvider::Resume() { impl_->stop.store(false); }

void EnvironmentProvider::RequestStop() { impl_->stop.store(true); }

}  // namespace ppm
