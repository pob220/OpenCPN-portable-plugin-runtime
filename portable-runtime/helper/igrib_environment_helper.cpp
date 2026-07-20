/*
 * Isolated environmental-data decoder for the iGRIB reference component.
 *
 * This process intentionally has no OpenCPN or wxWidgets dependency.  It
 * accepts a file capability as an explicit command-line path and emits only
 * bounded JSON value data.  The host supervisor is responsible for sandboxing,
 * timeouts, process limits and path grants.
 */

#include <eccodes.h>
#include <json/json.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Handle = std::unique_ptr<codes_handle, decltype(&codes_handle_delete)>;

class Error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct FileCloser {
  void operator()(FILE* file) const {
    if (file) std::fclose(file);
  }
};

using File = std::unique_ptr<FILE, FileCloser>;

std::string GetString(codes_handle* handle, const char* key) {
  std::array<char, 512> value{};
  size_t length = value.size();
  return codes_get_string(handle, key, value.data(), &length) == 0
             ? std::string(value.data())
             : std::string();
}

long GetLong(codes_handle* handle, const char* key, long fallback = -1) {
  long value = fallback;
  return codes_get_long(handle, key, &value) == 0 ? value : fallback;
}

std::string ValidTime(codes_handle* handle) {
  const long date = GetLong(handle, "validityDate");
  const long time = GetLong(handle, "validityTime");
  if (date < 0 || date > 99999999 || time < 0 || time > 2359) return {};
  char result[32]{};
  std::snprintf(result, sizeof(result), "%08dT%04dZ", static_cast<int>(date),
                static_cast<int>(time));
  return result;
}

long long DaysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<long long>(era) * 146097 + day_of_era - 719468;
}

bool TimeMinutes(const std::string& value, long long* result) {
  if (value.size() != 14 || value[8] != 'T' || value[13] != 'Z') return false;
  for (size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13) continue;
    if (value[i] < '0' || value[i] > '9') return false;
  }
  const int year = std::stoi(value.substr(0, 4));
  const unsigned month = static_cast<unsigned>(std::stoi(value.substr(4, 2)));
  const unsigned day = static_cast<unsigned>(std::stoi(value.substr(6, 2)));
  const int hour = std::stoi(value.substr(9, 2));
  const int minute = std::stoi(value.substr(11, 2));
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59)
    return false;
  *result = DaysFromCivil(year, month, day) * 24 * 60 + hour * 60 + minute;
  return true;
}

std::string FieldKind(codes_handle* handle) {
  const std::string short_name = GetString(handle, "shortName");
  const long parameter = GetLong(handle, "indicatorOfParameter");
  if (parameter == 49) return "current-u";
  if (parameter == 50) return "current-v";
  if (short_name == "10u" || short_name == "u10") return "wind-u";
  if (short_name == "10v" || short_name == "v10") return "wind-v";
  if (short_name == "msl" || short_name == "prmsl") return "pressure";
  if (short_name == "2t" || short_name == "t2m") return "air-temperature";
  if (short_name == "swh" || short_name == "htsgw") return "wave-height";
  if (short_name == "perpw") return "wave-period";
  if (short_name == "dirpw") return "wave-direction";
  return {};
}

bool PlausibleFieldValue(const std::string& kind, double value) {
  if (!std::isfinite(value)) return false;
  if (kind == "current-u" || kind == "current-v") return std::abs(value) < 12.0;
  if (kind == "wind-u" || kind == "wind-v") return std::abs(value) <= 200.0;
  if (kind == "wave-height") return value >= 0.0 && value <= 100.0;
  if (kind == "wave-period") return value >= 0.0 && value <= 100.0;
  if (kind == "wave-direction") return value >= 0.0 && value <= 360.0;
  return true;
}

File Open(const std::filesystem::path& path) {
  if (!std::filesystem::is_regular_file(path))
    throw Error("input is not a regular file");
  File file(std::fopen(path.c_str(), "rb"));
  if (!file) throw Error("could not open input file");
  return file;
}

Handle Next(FILE* file) {
  int error = 0;
  Handle handle(codes_handle_new_from_file(nullptr, file, PRODUCT_GRIB, &error),
                &codes_handle_delete);
  if (!handle && error != CODES_SUCCESS && !std::feof(file))
    throw Error(std::string("ecCodes decode failed: ") +
                codes_get_error_message(error));
  return handle;
}

Json::Value Inspect(const std::filesystem::path& path) {
  auto file = Open(path);
  std::set<std::string> times;
  std::map<std::string, size_t> fields;
  size_t messages = 0;
  while (auto handle = Next(file.get())) {
    ++messages;
    if (messages > 100000) throw Error("GRIB message limit exceeded");
    const auto time = ValidTime(handle.get());
    if (!time.empty()) times.insert(time);
    const auto kind = FieldKind(handle.get());
    if (!kind.empty()) ++fields[kind];
  }
  if (messages == 0) throw Error("file contains no GRIB messages");

  Json::Value result(Json::objectValue);
  result["schemaVersion"] = 1;
  result["messageCount"] = Json::UInt64(messages);
  result["byteCount"] = Json::UInt64(std::filesystem::file_size(path));
  result["displayName"] = path.filename().string();
  for (const auto& time : times) result["times"].append(time);
  for (const auto& [kind, count] : fields) {
    Json::Value field(Json::objectValue);
    field["kind"] = kind;
    field["messageCount"] = Json::UInt64(count);
    result["fields"].append(field);
  }
  return result;
}

Json::Value DecodeFrame(const std::filesystem::path& path,
                        const std::string& requested_time,
                        size_t maximum_points) {
  if (maximum_points < 16 || maximum_points > 100000)
    throw Error("max-points must be between 16 and 100000");
  long long requested_minutes = 0;
  if (!TimeMinutes(requested_time, &requested_minutes))
    throw Error("requested time is invalid");
  struct NearestTime {
    std::string value;
    long long minutes = 0;
    long long distance = std::numeric_limits<long long>::max();
  };
  std::map<std::string, NearestTime> nearest_times;
  {
    auto metadata_file = Open(path);
    size_t metadata_messages = 0;
    while (auto handle = Next(metadata_file.get())) {
      if (++metadata_messages > 100000)
        throw Error("GRIB message limit exceeded");
      const auto kind = FieldKind(handle.get());
      if (kind.empty()) continue;
      const auto time = ValidTime(handle.get());
      long long minutes = 0;
      if (!TimeMinutes(time, &minutes)) continue;
      const long long distance = std::llabs(minutes - requested_minutes);
      auto& nearest = nearest_times[kind];
      if (distance < nearest.distance ||
          (distance == nearest.distance && minutes < nearest.minutes))
        nearest = {time, minutes, distance};
    }
  }
  // Forecast products commonly publish waves at three-hour intervals while
  // weather and currents are hourly. Select the closest field-specific frame
  // within three hours and report its source time rather than silently making
  // sparse fields disappear from the combined timeline.
  constexpr long long kMaximumNearestMinutes = 180;
  auto file = Open(path);
  Json::Value fields(Json::arrayValue);
  size_t total_points = 0;
  size_t messages = 0;
  while (auto handle = Next(file.get())) {
    if (++messages > 100000) throw Error("GRIB message limit exceeded");
    const auto kind = FieldKind(handle.get());
    if (kind.empty()) continue;
    const auto nearest = nearest_times.find(kind);
    if (nearest == nearest_times.end() ||
        nearest->second.distance > kMaximumNearestMinutes ||
        ValidTime(handle.get()) != nearest->second.value)
      continue;

    size_t value_count = 0;
    if (codes_get_size(handle.get(), "values", &value_count) != 0 ||
        value_count == 0)
      continue;
    const size_t stride = std::max<size_t>(
        1, (value_count + maximum_points - 1) / maximum_points);
    int error = 0;
    std::unique_ptr<codes_iterator, decltype(&codes_grib_iterator_delete)>
        iterator(codes_grib_iterator_new(handle.get(), 0, &error),
                 &codes_grib_iterator_delete);
    if (!iterator)
      throw Error(std::string("could not iterate GRIB field: ") +
                  codes_get_error_message(error));

    Json::Value field(Json::objectValue);
    field["kind"] = kind;
    field["unit"] = GetString(handle.get(), "units");
    field["shortName"] = GetString(handle.get(), "shortName");
    field["sourceTime"] = nearest->second.value;
    field["timeOffsetMinutes"] =
        Json::Int64(nearest->second.minutes - requested_minutes);
    Json::Value samples(Json::arrayValue);
    double latitude = 0.0, longitude = 0.0, value = 0.0;
    double missing_value = 0.0;
    const bool has_missing_value =
        codes_get_double(handle.get(), "missingValue", &missing_value) == 0 &&
        std::isfinite(missing_value);
    size_t index = 0;
    while (codes_grib_iterator_next(iterator.get(), &latitude, &longitude,
                                    &value)) {
      if (index++ % stride != 0) continue;
      if (!std::isfinite(latitude) || !std::isfinite(longitude) ||
          !PlausibleFieldValue(kind, value) || latitude < -90.0 ||
          latitude > 90.0 || longitude < -360.0 || longitude > 360.0 ||
          (has_missing_value && value == missing_value))
        continue;
      Json::Value sample(Json::arrayValue);
      sample.append(latitude);
      sample.append(longitude > 180.0 ? longitude - 360.0 : longitude);
      sample.append(value);
      samples.append(std::move(sample));
      if (++total_points > maximum_points * 8)
        throw Error("decoded frame point limit exceeded");
    }
    field["samples"] = std::move(samples);
    fields.append(std::move(field));
  }
  if (fields.empty()) throw Error("requested time has no supported fields");
  Json::Value result(Json::objectValue);
  result["schemaVersion"] = 1;
  result["time"] = requested_time;
  result["fields"] = std::move(fields);
  result["sampleCount"] = Json::UInt64(total_points);
  return result;
}

void Print(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  std::cout << Json::writeString(builder, value) << '\n';
}

void WriteResult(const Json::Value& value, const std::filesystem::path& path) {
  const auto temporary = path.string() + ".tmp";
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw Error("could not create result file");
    output << Json::writeString(builder, value) << '\n';
    output.flush();
    if (!output) throw Error("could not write result file");
  }
  std::error_code error;
  std::filesystem::remove(path, error);
  error.clear();
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary);
    throw Error("could not publish result file");
  }
}

size_t ParseLimit(const std::string& value) {
  size_t parsed = 0;
  try {
    const auto result = std::stoull(value, &parsed);
    if (parsed != value.size() || result > std::numeric_limits<size_t>::max())
      throw Error("invalid max-points");
    return static_cast<size_t>(result);
  } catch (const std::exception&) {
    throw Error("invalid max-points");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const bool inspect_to_file = argc == 4 && std::string(argv[1]) == "inspect";
  const bool frame_to_file = argc == 6 && std::string(argv[1]) == "frame";
  const char* result_path = inspect_to_file ? argv[3]
                            : frame_to_file ? argv[5]
                                            : nullptr;
  try {
    if ((argc == 3 || inspect_to_file) && std::string(argv[1]) == "inspect") {
      const auto result = Inspect(argv[2]);
      if (result_path)
        WriteResult(result, result_path);
      else
        Print(result);
      return 0;
    }
    if ((argc == 5 || frame_to_file) && std::string(argv[1]) == "frame") {
      const auto result = DecodeFrame(argv[2], argv[3], ParseLimit(argv[4]));
      if (result_path)
        WriteResult(result, result_path);
      else
        Print(result);
      return 0;
    }
    std::cerr << "usage: igrib-environment-helper inspect FILE [RESULT_JSON]\n"
                 "       igrib-environment-helper frame FILE TIME MAX_POINTS "
                 "[RESULT_JSON]\n";
    return 2;
  } catch (const std::exception& error) {
    Json::Value failure(Json::objectValue);
    failure["schemaVersion"] = 1;
    failure["error"]["code"] = "environment-decode-failed";
    failure["error"]["message"] = error.what();
    if (result_path) {
      try {
        WriteResult(failure, result_path);
      } catch (const std::exception&) {
        Print(failure);
      }
    } else {
      Print(failure);
    }
    return 1;
  }
}
