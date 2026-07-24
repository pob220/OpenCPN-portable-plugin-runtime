#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "environmental_grib/environment.h"
#include "environmental_grib/geo.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/model.h"
#include "environmental_grib/parallel.h"
#include "environmental_grib/sources.h"

namespace eg = environmental_grib;

namespace {

int failures = 0;

void Check(bool condition, const std::string& description) {
  if (!condition) {
    std::cerr << "FAIL: " << description << '\n';
    ++failures;
  }
}

std::vector<unsigned char> ReadBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("cannot open concurrency fixture");
  const auto size = input.tellg();
  std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!input) throw std::runtime_error("cannot read concurrency fixture");
  return bytes;
}

void UpdatePeak(std::atomic<int>& peak, int active) {
  int value = peak.load();
  while (active > value && !peak.compare_exchange_weak(value, active)) {
  }
}

}  // namespace

int main() {
  try {
    std::vector<int> inputs(16);
    for (std::size_t i = 0; i < inputs.size(); ++i)
      inputs[i] = static_cast<int>(i);
    std::atomic<int> map_active{0}, map_peak{0};
    const auto mapped =
        eg::ParallelMapOrdered(inputs, 4, [&](const int& input) {
          const int active = ++map_active;
          UpdatePeak(map_peak, active);
          std::this_thread::sleep_for(
              std::chrono::milliseconds(2 * (4 - input % 4)));
          --map_active;
          return input * input;
        });
    Check(map_peak.load() == 4, "ordered map uses the requested bound");
    for (std::size_t i = 0; i < mapped.size(); ++i)
      Check(mapped[i] == inputs[i] * inputs[i],
            "ordered map preserves input order");

    const auto root = std::filesystem::temp_directory_path();
    const auto token = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto weather_path =
        root / ("xgrib-concurrency-weather-" + token + ".grb2");
    const auto current_path =
        root / ("xgrib-concurrency-current-" + token + ".grb");
    const auto wave_path = root / ("xgrib-concurrency-wave-" + token + ".grb2");
    const auto serial_path =
        root / ("xgrib-concurrency-serial-" + token + ".grb");
    const auto parallel_path =
        root / ("xgrib-concurrency-parallel-" + token + ".grb");
    const auto constrained_path =
        root / ("xgrib-concurrency-constrained-" + token + ".grb");
    const auto low_resource_path =
        root / ("xgrib-concurrency-low-resource-" + token + ".grb");
    const auto wave_current_path =
        root / ("xgrib-concurrency-wave-current-" + token + ".grb");
    const auto start = eg::ParseUtcDateTime("2026-07-01T00:00:00Z");
    const auto grid = eg::BuildRegularGrid({-8.5, 50.5, -7.5, 51.5}, 0.5);
    eg::WriteRegularLatLonGrib2(
        grid, start,
        {{0, "10u", std::vector<double>(grid.size(), 4.0), {}},
         {0, "10v", std::vector<double>(grid.size(), -2.0), {}}},
        weather_path);
    eg::WriteRegularLatLonGrib2(
        grid, start,
        {{0, "swh", std::vector<double>(grid.size(), 1.5), {}},
         {0, "perpw", std::vector<double>(grid.size(), 7.0), {}},
         {0, "dirpw", std::vector<double>(grid.size(), 240.0), {}}},
        wave_path);
    const auto current =
        eg::MakeSyntheticRotaryCurrent({-8.5, 50.5, -7.5, 51.5}, start, grid);
    eg::WriteGrib1Currents({current}, current_path);
    const auto weather_bytes = ReadBytes(weather_path);
    const auto wave_bytes = ReadBytes(wave_path);
    const auto current_bytes = ReadBytes(current_path);

    eg::EnvironmentRequest request;
    request.bbox = {-8.5, 50.5, -7.5, 51.5};
    request.start = start;
    request.hours = 6;
    request.step_hours = 1;
    request.cycle = "00";
    request.date = "20260701";
    request.weather_provider = "gfs";
    request.current_source = "marine_ie_irish_sea";
    request.overwrite = true;

    auto run = [&](bool parallel, const std::filesystem::path& output,
                   std::atomic<int>& peak) {
      std::atomic<int> active{0};
      request.parallel_components = parallel;
      request.output = output;
      return eg::GenerateEnvironment(
          request,
          [&](const std::string& url, double) {
            const int count = ++active;
            UpdatePeak(peak, count);
            std::this_thread::sleep_for(url.rfind("ftp://", 0) == 0
                                            ? std::chrono::milliseconds(120)
                                            : std::chrono::milliseconds(50));
            --active;
            return url.rfind("ftp://", 0) == 0 ? current_bytes : weather_bytes;
          },
          start);
    };

    std::atomic<int> serial_peak{0}, parallel_peak{0};
    const auto serial = run(false, serial_path, serial_peak);
    const auto concurrent = run(true, parallel_path, parallel_peak);
    Check(serial_peak.load() == 4,
          "serial component mode still parallelizes forecast hours");
    Check(parallel_peak.load() == 4,
          "component overlap honours the four-download job-wide bound");
    Check(serial.message_count == concurrent.message_count &&
              serial.byte_count == concurrent.byte_count,
          "component concurrency preserves output counts");
    Check(ReadBytes(serial_path) == ReadBytes(parallel_path),
          "component concurrency preserves GRIB bytes exactly");

    request.max_concurrent_downloads = 2;
    std::atomic<int> constrained_peak{0};
    const auto constrained = run(true, constrained_path, constrained_peak);
    Check(constrained_peak.load() == 2,
          "configured job-wide download bound is enforced");
    Check(constrained.message_count == serial.message_count &&
              ReadBytes(constrained_path) == ReadBytes(serial_path),
          "a lower download bound preserves deterministic output");

    request.max_concurrent_downloads = 1;
    std::atomic<int> low_resource_peak{0};
    const auto low_resource = run(true, low_resource_path, low_resource_peak);
    Check(low_resource_peak.load() == 1,
          "low-resource mode permits only one active download");
    Check(low_resource.message_count == serial.message_count &&
              ReadBytes(low_resource_path) == ReadBytes(serial_path),
          "low-resource mode preserves deterministic output");

    request.weather_provider = "none";
    request.include_waves = true;
    request.wave_provider = "gfs_wave";
    request.wave_step_hours = 3;
    request.current_source = "marine_ie_irish_sea";
    request.parallel_components = true;
    request.max_concurrent_downloads = 2;
    request.output = wave_current_path;
    std::atomic<int> wave_current_active{0}, wave_current_peak{0};
    const auto wave_current = eg::GenerateEnvironment(
        request,
        [&](const std::string& url, double) {
          const int count = ++wave_current_active;
          UpdatePeak(wave_current_peak, count);
          const bool current_request = url.rfind("ftp://", 0) == 0;
          std::this_thread::sleep_for(current_request
                                          ? std::chrono::milliseconds(160)
                                          : std::chrono::milliseconds(15));
          --wave_current_active;
          return current_request ? current_bytes : wave_bytes;
        },
        start);
    Check(wave_current_peak.load() == 2,
          "independent wave and current child jobs overlap safely");
    Check(wave_current.message_count > 0 &&
              std::filesystem::file_size(wave_current_path) > 0,
          "parallel wave/current child workspaces survive unequal completion");

    for (const auto& path :
         {weather_path, wave_path, current_path, serial_path, parallel_path,
          constrained_path, low_resource_path, wave_current_path}) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } catch (const std::exception& error) {
    std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
    ++failures;
  }
  std::cout << "environmental_grib_concurrency_tests failures=" << failures
            << '\n';
  return failures == 0 ? 0 : 1;
}
