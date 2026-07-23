#include "environment_service.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

namespace ppm {
namespace {

namespace fs = std::filesystem;

constexpr std::uintmax_t kMaximumFrameBytes = 64U * 1024U * 1024U;
constexpr std::uint32_t kMaximumFrames = 32;
constexpr std::uint32_t kMaximumFields = 64;
constexpr std::uint32_t kMaximumSamplesPerField = 100'000;
constexpr std::uint64_t kMaximumDeclaredSamples = 6'400'000;
constexpr std::uint32_t kMaximumStringBytes = 4096;
constexpr double kMaximumCurrentMetresPerSecond = 12.0;
constexpr double kMetresPerSecondToKnots = 1.94384449;
constexpr double kPi = 3.14159265358979323846;

bool PlausibleValue(const std::string& kind, double value) {
  if (!std::isfinite(value)) return false;
  if (kind == "current-u" || kind == "current-v") {
    return std::abs(value) < kMaximumCurrentMetresPerSecond;
  }
  if (kind == "wind-u" || kind == "wind-v") {
    return std::abs(value) <= 200.0;
  }
  if (kind == "wave-height" || kind == "wave-period") {
    return value >= 0.0 && value <= 100.0;
  }
  if (kind == "wave-direction") {
    return value >= 0.0 && value <= 360.0;
  }
  return true;
}

bool ReadExact(std::istream& input, char* output, std::size_t size) {
  input.read(output, static_cast<std::streamsize>(size));
  return input.good() ||
         (input.eof() && static_cast<std::size_t>(input.gcount()) == size);
}

bool ReadU32(std::istream& input, std::uint32_t* value) {
  std::array<unsigned char, 4> bytes{};
  if (!value ||
      !ReadExact(input, reinterpret_cast<char*>(bytes.data()), bytes.size())) {
    return false;
  }
  *value = 0;
  for (unsigned index = 0; index < bytes.size(); ++index) {
    *value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
  }
  return true;
}

bool ReadU64(std::istream& input, std::uint64_t* value) {
  std::array<unsigned char, 8> bytes{};
  if (!value ||
      !ReadExact(input, reinterpret_cast<char*>(bytes.data()), bytes.size())) {
    return false;
  }
  *value = 0;
  for (unsigned index = 0; index < bytes.size(); ++index) {
    *value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
  }
  return true;
}

bool ReadDouble(std::istream& input, double* value) {
  std::uint64_t bits = 0;
  if (!value || !ReadU64(input, &bits)) return false;
  static_assert(sizeof(double) == sizeof(bits));
  std::memcpy(value, &bits, sizeof(bits));
  return true;
}

bool ReadString(std::istream& input, std::string* value) {
  std::uint32_t length = 0;
  if (!value || !ReadU32(input, &length) || length > kMaximumStringBytes) {
    return false;
  }
  value->assign(length, '\0');
  return length == 0 || ReadExact(input, value->data(), value->size());
}

bool NearestVector(const EnvironmentFrame& frame, const std::string& u_name,
                   const std::string& v_name, double latitude,
                   double longitude, double* u_output, double* v_output,
                   bool current) {
  const auto u_field = frame.fields.find(u_name);
  const auto v_field = frame.fields.find(v_name);
  if (!u_output || !v_output || u_field == frame.fields.end() ||
      v_field == frame.fields.end()) {
    return false;
  }
  const std::size_t count =
      std::min(u_field->second.size(), v_field->second.size());
  const double longitude_scale =
      std::max(0.1, std::cos(latitude * kPi / 180.0));
  double best_distance = std::numeric_limits<double>::max();
  bool found = false;
  for (std::size_t index = 0; index < count; ++index) {
    const auto& u = u_field->second[index];
    const auto& v = v_field->second[index];
    if (std::abs(u.latitude - v.latitude) > 0.001 ||
        std::abs(u.longitude - v.longitude) > 0.001 ||
        (current && std::hypot(u.value, v.value) >=
                        kMaximumCurrentMetresPerSecond)) {
      continue;
    }
    const double dy = u.latitude - latitude;
    const double dx = (u.longitude - longitude) * longitude_scale;
    const double distance = dx * dx + dy * dy;
    if (distance < best_distance) {
      best_distance = distance;
      *u_output = u.value;
      *v_output = v.value;
      found = true;
    }
  }
  return found && best_distance <= 4.0;
}

bool NearestScalar(const EnvironmentFrame& frame, const std::string& name,
                   double latitude, double longitude, double* output) {
  const auto field = frame.fields.find(name);
  if (!output || field == frame.fields.end() || field->second.empty()) {
    return false;
  }
  const double longitude_scale =
      std::max(0.1, std::cos(latitude * kPi / 180.0));
  double best_distance = std::numeric_limits<double>::max();
  const EnvironmentGridSample* best = nullptr;
  for (const auto& sample : field->second) {
    const double dy = sample.latitude - latitude;
    const double dx = (sample.longitude - longitude) * longitude_scale;
    const double distance = dx * dx + dy * dy;
    if (distance < best_distance) {
      best_distance = distance;
      best = &sample;
    }
  }
  if (!best || best_distance > 4.0) return false;
  *output = best->value;
  return true;
}

}  // namespace

bool ReadEnvironmentFrames(const std::string& path,
                           std::vector<EnvironmentFrame>* frames,
                           std::string* diagnostic) {
  if (!frames) {
    if (diagnostic) *diagnostic = "no frame result destination";
    return false;
  }
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error || size < 12 || size > kMaximumFrameBytes) {
    if (diagnostic) {
      *diagnostic = "decoder frame is missing or exceeds the 64 MiB limit";
    }
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  std::array<char, 8> magic{};
  constexpr std::array<char, 8> expected = {'O', 'C', 'P', 'N',
                                             'F', 'R', 'M', '1'};
  std::uint32_t frame_count = 0;
  if (!input || !ReadExact(input, magic.data(), magic.size()) ||
      magic != expected || !ReadU32(input, &frame_count) ||
      frame_count == 0 || frame_count > kMaximumFrames) {
    if (diagnostic) {
      *diagnostic = "decoder returned an incompatible frame header";
    }
    return false;
  }

  std::vector<EnvironmentFrame> decoded;
  decoded.reserve(frame_count);
  for (std::uint32_t frame_index = 0; frame_index < frame_count;
       ++frame_index) {
    EnvironmentFrame frame;
    std::uint32_t field_count = 0;
    std::uint64_t declared_samples = 0;
    if (!ReadString(input, &frame.time) || frame.time.empty() ||
        !ReadU32(input, &field_count) || field_count == 0 ||
        field_count > kMaximumFields ||
        !ReadU64(input, &declared_samples) ||
        declared_samples > kMaximumDeclaredSamples) {
      if (diagnostic) *diagnostic = "decoder returned malformed frame metadata";
      return false;
    }
    for (std::uint32_t field_index = 0; field_index < field_count;
         ++field_index) {
      std::string kind;
      std::string unit;
      std::string source_time;
      std::uint32_t sample_count = 0;
      if (!ReadString(input, &kind) || kind.empty() ||
          !ReadString(input, &unit) || !ReadString(input, &source_time) ||
          !ReadU32(input, &sample_count) ||
          sample_count > kMaximumSamplesPerField ||
          frame.fields.count(kind) != 0) {
        if (diagnostic) {
          *diagnostic = "decoder returned malformed or duplicate field data";
        }
        return false;
      }
      auto& samples = frame.fields[kind];
      samples.reserve(sample_count);
      for (std::uint32_t sample_index = 0; sample_index < sample_count;
           ++sample_index) {
        EnvironmentGridSample sample;
        if (!ReadDouble(input, &sample.latitude) ||
            !ReadDouble(input, &sample.longitude) ||
            !ReadDouble(input, &sample.value) ||
            !std::isfinite(sample.latitude) || sample.latitude < -90.0 ||
            sample.latitude > 90.0 || !std::isfinite(sample.longitude) ||
            sample.longitude < -180.0 || sample.longitude > 180.0 ||
            !PlausibleValue(kind, sample.value)) {
          if (diagnostic) {
            *diagnostic = "decoder returned an invalid environmental sample";
          }
          return false;
        }
        samples.push_back(sample);
      }
      frame.sample_count += samples.size();
      frame.units.emplace(kind, std::move(unit));
      if (!source_time.empty()) {
        frame.source_times.emplace(kind, std::move(source_time));
      }
    }
    if (frame.sample_count != declared_samples) {
      if (diagnostic) *diagnostic = "decoder frame sample count is inconsistent";
      return false;
    }
    decoded.push_back(std::move(frame));
  }
  if (input.peek() != std::char_traits<char>::eof()) {
    if (diagnostic) *diagnostic = "decoder frame contains trailing data";
    return false;
  }
  *frames = std::move(decoded);
  if (diagnostic) diagnostic->clear();
  return true;
}

EnvironmentSample SampleEnvironmentFrame(const EnvironmentFrame& frame,
                                         double latitude,
                                         double longitude) {
  EnvironmentSample result;
  if (!std::isfinite(latitude) || !std::isfinite(longitude) ||
      latitude < -90.0 || latitude > 90.0 || longitude < -180.0 ||
      longitude > 180.0) {
    return result;
  }
  double u = 0.0;
  double v = 0.0;
  if (NearestVector(frame, "wind-u", "wind-v", latitude, longitude, &u, &v,
                    false)) {
    result.wind_u_knots = u * kMetresPerSecondToKnots;
    result.wind_v_knots = v * kMetresPerSecondToKnots;
    result.available |= 1U;
  }
  if (NearestVector(frame, "current-u", "current-v", latitude, longitude, &u,
                    &v, true)) {
    result.current_u_knots = u * kMetresPerSecondToKnots;
    result.current_v_knots = v * kMetresPerSecondToKnots;
    result.available |= 2U;
  }
  if (NearestScalar(frame, "wave-height", latitude, longitude,
                    &result.wave_height_metres)) {
    result.available |= 4U;
  }
  return result;
}

}  // namespace ppm
