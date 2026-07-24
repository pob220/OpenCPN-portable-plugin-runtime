#include "environment_service.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#define CHECK(expression)                                            \
  do {                                                               \
    if (!(expression)) {                                             \
      std::cerr << "check failed at " << __FILE__ << ':' << __LINE__ \
                << ": " #expression "\n";                            \
      return 1;                                                      \
    }                                                                \
  } while (false)

namespace {

namespace fs = std::filesystem;

void U32(std::ostream& output, std::uint32_t value) {
  for (unsigned index = 0; index < 4; ++index)
    output.put(static_cast<char>((value >> (index * 8)) & 0xff));
}

void U64(std::ostream& output, std::uint64_t value) {
  for (unsigned index = 0; index < 8; ++index)
    output.put(static_cast<char>((value >> (index * 8)) & 0xff));
}

void Number(std::ostream& output, double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  U64(output, bits);
}

void Text(std::ostream& output, const std::string& value) {
  U32(output, static_cast<std::uint32_t>(value.size()));
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

struct Field {
  std::string name;
  std::string unit;
  std::vector<ppm::EnvironmentGridSample> samples;
};

bool WriteFrame(const fs::path& path, const std::vector<Field>& fields,
                bool trailing = false) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write("OCPNFRM1", 8);
  U32(output, 1);
  Text(output, "20260723T1000Z");
  U32(output, static_cast<std::uint32_t>(fields.size()));
  std::uint64_t total = 0;
  for (const auto& field : fields) total += field.samples.size();
  U64(output, total);
  for (const auto& field : fields) {
    Text(output, field.name);
    Text(output, field.unit);
    Text(output, "20260723T1000Z");
    U32(output, static_cast<std::uint32_t>(field.samples.size()));
    for (const auto& sample : field.samples) {
      Number(output, sample.latitude);
      Number(output, sample.longitude);
      Number(output, sample.value);
    }
  }
  if (trailing) output.put('x');
  return static_cast<bool>(output);
}

}  // namespace

int main() {
  const auto stamp =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() /
                        ("ppm environment service " + std::to_string(stamp));
  std::error_code error;
  fs::create_directories(root, error);
  CHECK(!error);
  const std::vector<ppm::EnvironmentGridSample> grid = {{53.0, -5.0, 1.0},
                                                        {54.0, -5.0, 2.0}};
  CHECK(
      WriteFrame(root / "valid.bin",
                 {{"wind-u", "m s-1", grid},
                  {"wind-v", "m s-1", {{53.0, -5.0, -2.0}, {54.0, -5.0, -3.0}}},
                  {"current-u", "m s-1", {{53.0, -5.0, 0.5}}},
                  {"current-v", "m s-1", {{53.0, -5.0, -0.25}}},
                  {"wave-height", "m", {{53.0, -5.0, 1.75}}}}));
  std::vector<ppm::EnvironmentFrame> frames;
  std::string diagnostic;
  CHECK(ppm::ReadEnvironmentFrames((root / "valid.bin").string(), &frames,
                                   &diagnostic));
  CHECK(diagnostic.empty());
  CHECK(frames.size() == 1);
  CHECK(frames[0].sample_count == 7);
  const auto sample = ppm::SampleEnvironmentFrame(frames[0], 53.1, -5.1);
  CHECK(sample.available == 7);
  CHECK(std::abs(sample.wind_u_knots - 1.94384449) < 1e-8);
  CHECK(std::abs(sample.wind_v_knots + 3.88768898) < 1e-8);
  CHECK(std::abs(sample.current_u_knots - 0.971922245) < 1e-8);
  CHECK(std::abs(sample.current_v_knots + 0.4859611225) < 1e-8);
  CHECK(std::abs(sample.wave_height_metres - 1.75) < 1e-8);
  CHECK(ppm::SampleEnvironmentFrame(frames[0], 20.0, 20.0).available == 0);
  auto unindexed = frames[0];
  unindexed.spatial_indices.clear();
  for (double latitude = 51.75; latitude <= 55.25; latitude += 0.125) {
    for (double longitude = -6.25; longitude <= -3.75; longitude += 0.125) {
      const auto indexed =
          ppm::SampleEnvironmentFrame(frames[0], latitude, longitude);
      const auto scanned =
          ppm::SampleEnvironmentFrame(unindexed, latitude, longitude);
      CHECK(indexed.available == scanned.available);
      CHECK(indexed.wind_u_knots == scanned.wind_u_knots);
      CHECK(indexed.wind_v_knots == scanned.wind_v_knots);
      CHECK(indexed.current_u_knots == scanned.current_u_knots);
      CHECK(indexed.current_v_knots == scanned.current_v_knots);
      CHECK(indexed.wave_height_metres == scanned.wave_height_metres);
    }
  }
  std::atomic_bool concurrent_plans_match{true};
  std::vector<std::thread> sampling_workers;
  for (int worker = 0; worker < 4; ++worker) {
    sampling_workers.emplace_back([&] {
      for (int iteration = 0; iteration < 200; ++iteration) {
        const auto cached =
            ppm::SampleEnvironmentFrame(frames[0], 53.1, -5.1);
        if (cached.available != sample.available ||
            cached.wind_u_knots != sample.wind_u_knots ||
            cached.wind_v_knots != sample.wind_v_knots ||
            cached.current_u_knots != sample.current_u_knots ||
            cached.current_v_knots != sample.current_v_knots ||
            cached.wave_height_metres != sample.wave_height_metres) {
          concurrent_plans_match = false;
        }
      }
    });
  }
  for (auto& worker : sampling_workers) worker.join();
  CHECK(concurrent_plans_match);
  auto first = std::make_shared<const ppm::EnvironmentFrame>(frames[0]);
  auto second_value = frames[0];
  second_value.time = "20260723T1100Z";
  second_value.fields["wind-u"][0].value = 5.0;
  second_value.fields["wind-v"][0].value = 2.0;
  second_value.fields["wave-direction"] = {{53.0, -5.0, 10.0}};
  second_value.units["wave-direction"] = "degree";
  auto first_value = frames[0];
  first_value.fields["wave-direction"] = {{53.0, -5.0, 350.0}};
  first_value.units["wave-direction"] = "degree";
  first = std::make_shared<const ppm::EnvironmentFrame>(std::move(first_value));
  auto second =
      std::make_shared<const ppm::EnvironmentFrame>(std::move(second_value));
  const auto interpolated =
      ppm::InterpolateEnvironmentFrames(first, second, "20260723T1015Z", 0.25);
  CHECK(interpolated);
  CHECK(interpolated->time == "20260723T1015Z");
  CHECK(interpolated->spatial_indices.count("wind-u") == 1);
  CHECK(std::abs(interpolated->fields.at("wind-u")[0].value - 2.0) < 1e-12);
  CHECK(std::abs(interpolated->fields.at("wind-v")[0].value + 1.0) < 1e-12);
  CHECK(std::abs(interpolated->fields.at("wave-direction")[0].value - 355.0) <
        1e-12);

  CHECK(WriteFrame(root / "trailing.bin", {{"wind-u", "m s-1", grid}}, true));
  CHECK(!ppm::ReadEnvironmentFrames((root / "trailing.bin").string(), &frames,
                                    &diagnostic));
  CHECK(diagnostic.find("trailing") != std::string::npos);
  CHECK(WriteFrame(root / "implausible.bin",
                   {{"current-u", "m s-1", {{53.0, -5.0, 9999.0}}}}));
  CHECK(!ppm::ReadEnvironmentFrames((root / "implausible.bin").string(),
                                    &frames, &diagnostic));
  CHECK(diagnostic.find("invalid environmental sample") != std::string::npos);
  CHECK(WriteFrame(root / "mismatch.bin",
                   {{"wind-u", "m s-1", {{53.0, -5.0, 1.0}}},
                    {"wind-v", "m s-1", {{54.0, -5.0, 1.0}}}}));
  CHECK(ppm::ReadEnvironmentFrames((root / "mismatch.bin").string(), &frames,
                                   &diagnostic));
  CHECK(ppm::SampleEnvironmentFrame(frames[0], 53.0, -5.0).available == 0);

  fs::remove_all(root, error);
  return 0;
}
