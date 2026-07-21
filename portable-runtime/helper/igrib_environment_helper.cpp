/*
 * Isolated environmental-data decoder for the iGRIB reference component.
 *
 * This process intentionally has no OpenCPN or wxWidgets dependency.  It
 * accepts file capabilities as explicit command-line paths and emits bounded,
 * versioned value data.  Metadata and low-volume operations use JSON; frame
 * data can use a compact binary protocol.  The host supervisor is responsible
 * for sandboxing, timeouts, process limits and path grants.
 */

#include <eccodes.h>
#include <json/json.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

struct FieldDescriptor {
  std::string id;
  std::string group;
  std::string component;
  std::string level_type;
  long level = 0;
  bool marine = false;
};

struct Candidate {
  std::string value;
  long long minutes = 0;
  uint64_t offset = 0;
};

using CandidateIndex =
    std::map<std::string, std::map<long long, Candidate>>;

uint64_t FileOffset(FILE* file) {
#ifdef _WIN32
  const auto offset = ::_ftelli64(file);
#else
  const auto offset = ::ftello(file);
#endif
  if (offset < 0) throw Error("could not determine GRIB message offset");
  return static_cast<uint64_t>(offset);
}

void SeekFile(FILE* file, uint64_t offset) {
  if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    throw Error("GRIB message offset is out of range");
#ifdef _WIN32
  const int result = ::_fseeki64(file, static_cast<int64_t>(offset), SEEK_SET);
#else
  const int result = ::fseeko(file, static_cast<off_t>(offset), SEEK_SET);
#endif
  if (result != 0) throw Error("could not seek to indexed GRIB message");
}

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

std::string NormalizedLevelType(codes_handle* handle) {
  const auto value = GetString(handle, "typeOfLevel");
  if (value == "isobaricInhPa" || value == "isobaricInPa") return "isobaric";
  if (value == "heightAboveGround") return "height-above-ground";
  if (value == "meanSea") return "mean-sea-level";
  if (value == "depthBelowSea" || value == "oceanModelLayer") return "ocean";
  return value.empty() ? "surface" : value;
}

long NormalizedLevel(codes_handle* handle, const std::string& level_type) {
  long level = GetLong(handle, "level", 0);
  if (level_type == "isobaric" &&
      GetString(handle, "typeOfLevel") == "isobaricInPa")
    level /= 100;
  return level;
}

std::string FieldId(const std::string& group, const std::string& component,
                    const std::string& level_type, long level) {
  std::string result = group;
  if (!component.empty()) result += "-" + component;
  if (level_type == "isobaric") result += "@" + std::to_string(level) + "hpa";
  return result;
}

FieldDescriptor DescribeField(codes_handle* handle) {
  const std::string short_name = GetString(handle, "shortName");
  const long parameter = GetLong(handle, "indicatorOfParameter");
  const long discipline = GetLong(handle, "discipline");
  const long category = GetLong(handle, "parameterCategory");
  const long number = GetLong(handle, "parameterNumber");
  FieldDescriptor field;
  field.level_type = NormalizedLevelType(handle);
  field.level = NormalizedLevel(handle, field.level_type);

  if (parameter == 49 || short_name == "uo" || short_name == "uogrd" ||
      (discipline == 10 && category == 1 && number == 2)) {
    field.group = "current";
    field.component = "u";
    field.marine = true;
  } else if (parameter == 50 || short_name == "vo" || short_name == "vogrd" ||
             (discipline == 10 && category == 1 && number == 3)) {
    field.group = "current";
    field.component = "v";
    field.marine = true;
  } else if (short_name == "10u" || short_name == "u10" || short_name == "u" ||
             short_name == "ugrd" ||
             (discipline == 0 && category == 2 && number == 2)) {
    field.group = "wind";
    field.component = "u";
  } else if (short_name == "10v" || short_name == "v10" || short_name == "v" ||
             short_name == "vgrd" ||
             (discipline == 0 && category == 2 && number == 3)) {
    field.group = "wind";
    field.component = "v";
  } else if (short_name == "gust" || short_name == "10fg" ||
             short_name == "gustsfc" ||
             (discipline == 0 && category == 2 && number == 22)) {
    field.group = "wind-gust";
  } else if (short_name == "msl" || short_name == "prmsl" ||
             short_name == "pres" ||
             (discipline == 0 && category == 3 &&
              (number == 0 || number == 1 || number == 192))) {
    field.group = "pressure";
  } else if (short_name == "2t" || short_name == "t2m" || short_name == "t" ||
             (discipline == 0 && category == 0 && number == 0)) {
    field.group = "air-temperature";
  } else if (short_name == "sst" || short_name == "wtmp" ||
             short_name == "sot" ||
             (discipline == 10 && category == 3 && number == 0) ||
             (discipline == 0 && category == 0 && number == 17)) {
    field.group = "sea-temperature";
    field.marine = true;
  } else if (short_name == "swh" || short_name == "htsgw" ||
             (discipline == 10 && category == 0 &&
              (number == 3 || number == 5))) {
    field.group = "wave";
    field.component = "height";
    field.marine = true;
  } else if (short_name == "perpw" || short_name == "mwp" ||
             short_name == "wvper" ||
             (discipline == 10 && category == 0 &&
              (number == 6 || number == 11 || number == 15))) {
    field.group = "wave";
    field.component = "period";
    field.marine = true;
  } else if (short_name == "dirpw" || short_name == "mwd" ||
             short_name == "wvdir" ||
             (discipline == 10 && category == 0 &&
              (number == 4 || number == 10 || number == 14))) {
    field.group = "wave";
    field.component = "direction";
    field.marine = true;
  } else if (short_name == "tp" || short_name == "apcp" ||
             short_name == "prate" ||
             (discipline == 0 && category == 1 &&
              (number == 7 || number == 8 || number == 49 || number == 52))) {
    field.group = "precipitation";
  } else if (short_name == "tcc" || short_name == "tcdc" ||
             (discipline == 0 && category == 6 && number == 1)) {
    field.group = "cloud";
  } else if (short_name == "cape" ||
             (discipline == 0 && category == 7 && number == 6)) {
    field.group = "cape";
  } else if (short_name == "refc" || short_name == "cref" ||
             short_name == "refd" ||
             (discipline == 0 && category == 16 && number == 196)) {
    field.group = "composite-reflectivity";
  } else if (short_name == "r" || short_name == "rh" ||
             (discipline == 0 && category == 1 && number == 1)) {
    field.group = "relative-humidity";
  } else if (short_name == "gh" || short_name == "z" || short_name == "hgt" ||
             (discipline == 0 && category == 3 && number == 5)) {
    field.group = "geopotential-height";
  }
  if (!field.group.empty())
    field.id =
        FieldId(field.group, field.component, field.level_type, field.level);
  return field;
}

bool PlausibleFieldValue(const FieldDescriptor& field, double value) {
  if (!std::isfinite(value)) return false;
  if (field.group == "current") return std::abs(value) < 12.0;
  if (field.group == "wind") return std::abs(value) <= 200.0;
  if (field.group == "wind-gust") return value >= 0.0 && value <= 200.0;
  if (field.group == "wave" && field.component == "height")
    return value >= 0.0 && value <= 100.0;
  if (field.group == "wave" && field.component == "period")
    return value >= 0.0 && value <= 100.0;
  if (field.group == "wave" && field.component == "direction")
    return value >= 0.0 && value <= 360.0;
  if (field.group == "cloud" || field.group == "relative-humidity")
    return value >= 0.0 && value <= 100.0;
  if (field.group == "precipitation") return value >= 0.0;
  return true;
}

void AppendDescriptor(Json::Value* value, const FieldDescriptor& descriptor) {
  (*value)["kind"] = descriptor.id;  // schema-1 compatibility alias
  (*value)["fieldId"] = descriptor.id;
  (*value)["group"] = descriptor.group;
  if (!descriptor.component.empty())
    (*value)["component"] = descriptor.component;
  (*value)["levelType"] = descriptor.level_type;
  (*value)["level"] = Json::Int64(descriptor.level);
  (*value)["marine"] = descriptor.marine;
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

Json::Value Inspect(const std::filesystem::path& path,
                    Json::Value* message_index = nullptr) {
  auto file = Open(path);
  std::set<std::string> times;
  std::map<std::string, std::pair<FieldDescriptor, size_t>> fields;
  size_t messages = 0;
  while (true) {
    const uint64_t offset = FileOffset(file.get());
    auto handle = Next(file.get());
    if (!handle) break;
    ++messages;
    if (messages > 100000) throw Error("GRIB message limit exceeded");
    const auto time = ValidTime(handle.get());
    if (!time.empty()) times.insert(time);
    const auto field = DescribeField(handle.get());
    if (!field.id.empty()) {
      auto& entry = fields[field.id];
      entry.first = field;
      ++entry.second;
      if (message_index && !time.empty()) {
        Json::Value indexed(Json::objectValue);
        indexed["fieldId"] = field.id;
        indexed["time"] = time;
        indexed["offset"] = Json::UInt64(offset);
        (*message_index)["messages"].append(std::move(indexed));
      }
    }
  }
  if (messages == 0) throw Error("file contains no GRIB messages");

  Json::Value result(Json::objectValue);
  result["schemaVersion"] = 2;
  result["messageCount"] = Json::UInt64(messages);
  result["byteCount"] = Json::UInt64(std::filesystem::file_size(path));
  result["displayName"] = path.filename().string();
  for (const auto& time : times) result["times"].append(time);
  for (const auto& [id, entry] : fields) {
    Json::Value field(Json::objectValue);
    AppendDescriptor(&field, entry.first);
    field["messageCount"] = Json::UInt64(entry.second);
    result["fields"].append(field);
  }
  if (message_index) {
    (*message_index)["schemaVersion"] = 1;
    (*message_index)["byteCount"] = result["byteCount"];
    (*message_index)["messageCount"] = result["messageCount"];
  }
  return result;
}

Json::Value ReadJsonFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw Error("could not read GRIB message index");
  Json::CharReaderBuilder builder;
  Json::Value value;
  std::string errors;
  if (!Json::parseFromStream(builder, input, &value, &errors) ||
      !value.isObject())
    throw Error("GRIB message index is malformed");
  return value;
}

CandidateIndex LoadCandidateIndex(const std::filesystem::path& path,
                                  const std::filesystem::path& index_path) {
  const Json::Value index = ReadJsonFile(index_path);
  const uint64_t byte_count = std::filesystem::file_size(path);
  if (index["schemaVersion"].asInt() != 1 ||
      !index["byteCount"].isUInt64() ||
      index["byteCount"].asUInt64() != byte_count ||
      !index["messages"].isArray() || index["messages"].size() > 100000)
    throw Error("GRIB message index does not match the immutable dataset");
  CandidateIndex candidates;
  for (const auto& message : index["messages"]) {
    if (!message.isObject() || !message["fieldId"].isString() ||
        !message["time"].isString() || !message["offset"].isUInt64())
      throw Error("GRIB message index has an incompatible entry");
    const std::string id = message["fieldId"].asString();
    const std::string time = message["time"].asString();
    const uint64_t offset = message["offset"].asUInt64();
    long long minutes = 0;
    if (id.empty() || id.size() > 256 || !TimeMinutes(time, &minutes) ||
        offset >= byte_count)
      throw Error("GRIB message index contains an invalid value");
    // Identical field/time entries preserve concatenated-file last-wins
    // semantics because inspection records messages in file order.
    candidates[id][minutes] = {time, minutes, offset};
  }
  if (candidates.empty()) throw Error("GRIB message index contains no fields");
  return candidates;
}

Json::Value DecodeFrame(const std::filesystem::path& path,
                        const std::string& requested_time,
                        size_t maximum_points,
                        const std::filesystem::path& index_path = {}) {
  if (maximum_points < 16 || maximum_points > 100000)
    throw Error("max-points must be between 16 and 100000");
  long long requested_minutes = 0;
  if (!TimeMinutes(requested_time, &requested_minutes))
    throw Error("requested time is invalid");
  CandidateIndex candidates;
  if (!index_path.empty()) {
    candidates = LoadCandidateIndex(path, index_path);
  } else {
    auto metadata_file = Open(path);
    size_t metadata_messages = 0;
    while (true) {
      const uint64_t offset = FileOffset(metadata_file.get());
      auto handle = Next(metadata_file.get());
      if (!handle) break;
      if (++metadata_messages > 100000)
        throw Error("GRIB message limit exceeded");
      const auto field = DescribeField(handle.get());
      if (field.id.empty()) continue;
      const auto time = ValidTime(handle.get());
      long long minutes = 0;
      if (!TimeMinutes(time, &minutes)) continue;
      // Multiple selected files are concatenated in user-selected order.
      // Replacing this map entry makes conflicts deterministic: the last
      // message for an identical field/time wins.
      candidates[field.id][minutes] = {time, minutes, offset};
    }
  }
  struct Selection {
    Candidate before;
    Candidate after;
    bool have_before = false;
    bool have_after = false;
  };
  std::map<std::string, Selection> selections;
  constexpr long long kMaximumNearestMinutes = 180;
  constexpr long long kMaximumInterpolationSpanMinutes = 360;
  std::set<uint64_t> selected_messages;
  for (const auto& [id, available] : candidates) {
    Selection selection;
    const auto after = available.lower_bound(requested_minutes);
    if (after != available.end()) {
      selection.after = after->second;
      selection.have_after = true;
    }
    if (after != available.begin()) {
      selection.before = std::prev(after)->second;
      selection.have_before = true;
    }
    if (after != available.end() && after->first == requested_minutes) {
      selection.before = after->second;
      selection.have_before = true;
    }
    if (selection.have_before && selection.have_after &&
        selection.after.minutes - selection.before.minutes <=
            kMaximumInterpolationSpanMinutes) {
      selected_messages.insert(selection.before.offset);
      selected_messages.insert(selection.after.offset);
    } else {
      const long long before_distance =
          selection.have_before ? requested_minutes - selection.before.minutes
                                : std::numeric_limits<long long>::max();
      const long long after_distance =
          selection.have_after ? selection.after.minutes - requested_minutes
                               : std::numeric_limits<long long>::max();
      if (std::min(before_distance, after_distance) > kMaximumNearestMinutes)
        continue;
      if (before_distance <= after_distance) {
        selection.after = selection.before;
        selection.have_after = selection.have_before;
      } else {
        selection.before = selection.after;
        selection.have_before = selection.have_after;
      }
      selected_messages.insert(selection.before.offset);
    }
    selections[id] = selection;
  }

  struct PointValue {
    double latitude = 0.0;
    double longitude = 0.0;
    double value = 0.0;
  };
  struct DecodedMessage {
    FieldDescriptor descriptor;
    std::string unit;
    std::string short_name;
    std::string source_time;
    long long minutes = 0;
    std::vector<PointValue> samples;
  };
  std::map<uint64_t, DecodedMessage> decoded_messages;
  auto file = Open(path);
  for (const uint64_t offset : selected_messages) {
    SeekFile(file.get(), offset);
    auto handle = Next(file.get());
    if (!handle) throw Error("indexed GRIB message is unavailable");
    const auto descriptor = DescribeField(handle.get());
    if (descriptor.id.empty()) continue;
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

    DecodedMessage decoded;
    decoded.descriptor = descriptor;
    decoded.unit = GetString(handle.get(), "units");
    decoded.short_name = GetString(handle.get(), "shortName");
    decoded.source_time = ValidTime(handle.get());
    TimeMinutes(decoded.source_time, &decoded.minutes);
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
          !PlausibleFieldValue(descriptor, value) || latitude < -90.0 ||
          latitude > 90.0 || longitude < -360.0 || longitude > 360.0 ||
          (has_missing_value && value == missing_value))
        continue;
      decoded.samples.push_back(
          {latitude, longitude > 180.0 ? longitude - 360.0 : longitude, value});
    }
    decoded_messages[offset] = std::move(decoded);
  }

  Json::Value fields(Json::arrayValue);
  size_t total_points = 0;
  for (const auto& [id, selection] : selections) {
    const auto before = decoded_messages.find(selection.before.offset);
    const auto after = decoded_messages.find(selection.after.offset);
    if (before == decoded_messages.end() || after == decoded_messages.end())
      continue;
    const auto& first = before->second;
    const auto& second = after->second;
    const bool can_interpolate =
        selection.before.offset != selection.after.offset &&
        first.samples.size() == second.samples.size() &&
        second.minutes > first.minutes;
    const double factor =
        can_interpolate
            ? static_cast<double>(requested_minutes - first.minutes) /
                  static_cast<double>(second.minutes - first.minutes)
            : 0.0;
    Json::Value field(Json::objectValue);
    AppendDescriptor(&field, first.descriptor);
    field["unit"] = first.unit;
    field["shortName"] = first.short_name;
    field["sourceTime"] = can_interpolate ? requested_time : first.source_time;
    field["timeOffsetMinutes"] =
        Json::Int64(can_interpolate ? 0 : first.minutes - requested_minutes);
    field["interpolated"] = can_interpolate;
    field["sourceTimes"].append(first.source_time);
    if (can_interpolate) field["sourceTimes"].append(second.source_time);
    Json::Value samples(Json::arrayValue);
    for (size_t index = 0; index < first.samples.size(); ++index) {
      const auto& left = first.samples[index];
      double output_value = left.value;
      if (can_interpolate) {
        const auto& right = second.samples[index];
        if (std::abs(left.latitude - right.latitude) > 1e-6 ||
            std::abs(left.longitude - right.longitude) > 1e-6)
          continue;
        if (first.descriptor.group == "wave" &&
            first.descriptor.component == "direction") {
          const double delta =
              std::fmod(right.value - left.value + 540.0, 360.0) - 180.0;
          output_value = std::fmod(left.value + factor * delta + 360.0, 360.0);
        } else {
          output_value = left.value + factor * (right.value - left.value);
        }
      }
      if (!PlausibleFieldValue(first.descriptor, output_value)) continue;
      Json::Value sample(Json::arrayValue);
      sample.append(left.latitude);
      sample.append(left.longitude);
      sample.append(output_value);
      samples.append(std::move(sample));
      if (++total_points > maximum_points * 64)
        throw Error("decoded frame point limit exceeded");
    }
    field["samples"] = std::move(samples);
    fields.append(std::move(field));
  }
  if (fields.empty()) throw Error("requested time has no supported fields");
  Json::Value result(Json::objectValue);
  result["schemaVersion"] = 2;
  result["time"] = requested_time;
  result["fields"] = std::move(fields);
  result["sampleCount"] = Json::UInt64(total_points);
  return result;
}

Json::Value WeatherTable(const std::filesystem::path& path, double latitude,
                         double longitude) {
  if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0 ||
      !std::isfinite(longitude) || longitude < -180.0 || longitude > 180.0)
    throw Error("weather-table position is invalid");
  struct NearestValue {
    FieldDescriptor descriptor;
    double distance = std::numeric_limits<double>::max();
    double value = 0.0;
    std::string unit;
  };
  std::map<std::string, std::map<std::string, NearestValue>> rows;
  auto file = Open(path);
  size_t messages = 0;
  while (auto handle = Next(file.get())) {
    if (++messages > 100000) throw Error("GRIB message limit exceeded");
    const auto descriptor = DescribeField(handle.get());
    const auto time = ValidTime(handle.get());
    if (descriptor.id.empty() || time.empty()) continue;
    int error = 0;
    std::unique_ptr<codes_iterator, decltype(&codes_grib_iterator_delete)>
        iterator(codes_grib_iterator_new(handle.get(), 0, &error),
                 &codes_grib_iterator_delete);
    if (!iterator) continue;
    auto& nearest = rows[time][descriptor.id];
    nearest.descriptor = descriptor;
    nearest.unit = GetString(handle.get(), "units");
    const double longitude_scale =
        std::max(0.1, std::cos(latitude * 3.14159265358979323846 / 180.0));
    double sample_latitude = 0.0;
    double sample_longitude = 0.0;
    double value = 0.0;
    while (codes_grib_iterator_next(iterator.get(), &sample_latitude,
                                    &sample_longitude, &value)) {
      if (sample_longitude > 180.0) sample_longitude -= 360.0;
      if (!PlausibleFieldValue(descriptor, value)) continue;
      const double dy = sample_latitude - latitude;
      const double dx = (sample_longitude - longitude) * longitude_scale;
      const double distance = dx * dx + dy * dy;
      if (distance <= nearest.distance) {
        nearest.distance = distance;
        nearest.value = value;
      }
    }
  }
  if (rows.empty())
    throw Error("GRIB contains no supported weather-table data");
  Json::Value result(Json::objectValue);
  result["schemaVersion"] = 2;
  result["latitude"] = latitude;
  result["longitude"] = longitude;
  for (const auto& [time, values] : rows) {
    Json::Value row(Json::objectValue);
    row["time"] = time;
    for (const auto& [id, nearest] : values) {
      if (!std::isfinite(nearest.distance)) continue;
      Json::Value field(Json::objectValue);
      AppendDescriptor(&field, nearest.descriptor);
      field["value"] = nearest.value;
      field["unit"] = nearest.unit;
      row["fields"][id] = std::move(field);
    }
    result["rows"].append(std::move(row));
  }
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

void WriteU32(std::ostream& output, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    output.put(static_cast<char>((value >> shift) & 0xff));
}

void WriteU64(std::ostream& output, uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8)
    output.put(static_cast<char>((value >> shift) & 0xff));
}

void WriteDouble(std::ostream& output, double value) {
  static_assert(sizeof(double) == sizeof(uint64_t),
                "portable frame protocol requires binary64 doubles");
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  WriteU64(output, bits);
}

void WriteString(std::ostream& output, const std::string& value) {
  if (value.size() > 4096) throw Error("binary frame string is too large");
  WriteU32(output, static_cast<uint32_t>(value.size()));
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

void WriteFrameBody(std::ostream& output, const Json::Value& frame) {
  if (!frame.isObject() || !frame["time"].isString() ||
      !frame["fields"].isArray() || frame["fields"].size() > 64)
    throw Error("cannot encode incompatible binary frame");
  WriteString(output, frame["time"].asString());
  WriteU32(output, static_cast<uint32_t>(frame["fields"].size()));
  WriteU64(output, frame["sampleCount"].asUInt64());
  uint64_t counted_samples = 0;
  for (const auto& field : frame["fields"]) {
    if (!field.isObject() || !field["kind"].isString() ||
        !field["samples"].isArray() || field["samples"].size() > 100000)
      throw Error("cannot encode incompatible binary field");
    WriteString(output, field["kind"].asString());
    WriteString(output,
                field["unit"].isString() ? field["unit"].asString() : "");
    WriteString(output, field["sourceTime"].isString()
                            ? field["sourceTime"].asString()
                            : "");
    WriteU32(output, static_cast<uint32_t>(field["samples"].size()));
    for (const auto& sample : field["samples"]) {
      if (!sample.isArray() || sample.size() != 3)
        throw Error("cannot encode incompatible binary sample");
      WriteDouble(output, sample[0].asDouble());
      WriteDouble(output, sample[1].asDouble());
      WriteDouble(output, sample[2].asDouble());
      ++counted_samples;
    }
  }
  if (counted_samples != frame["sampleCount"].asUInt64())
    throw Error("binary frame sample count is inconsistent");
}

void WriteBinaryFrames(const std::vector<Json::Value>& frames,
                       const std::filesystem::path& path) {
  if (frames.empty() || frames.size() > 32)
    throw Error("binary frame batch size is invalid");
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw Error("could not create binary frame result");
    constexpr std::array<char, 8> magic = {'O', 'C', 'P', 'N',
                                            'F', 'R', 'M', '1'};
    output.write(magic.data(), static_cast<std::streamsize>(magic.size()));
    WriteU32(output, static_cast<uint32_t>(frames.size()));
    for (const auto& frame : frames) WriteFrameBody(output, frame);
    output.flush();
    if (!output) throw Error("could not write binary frame result");
  }
  std::error_code error;
  std::filesystem::remove(path, error);
  error.clear();
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary);
    throw Error("could not publish binary frame result");
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

void ConfigureBundledDefinitions(const char* executable) {
  if (std::getenv("ECCODES_DEFINITION_PATH")) return;
  std::error_code error;
  const auto definitions =
      std::filesystem::weakly_canonical(executable, error).parent_path() /
      "share" / "eccodes" / "definitions";
  if (error || !std::filesystem::is_directory(definitions)) return;
#ifdef _WIN32
  ::_putenv_s("ECCODES_DEFINITION_PATH", definitions.string().c_str());
#else
  ::setenv("ECCODES_DEFINITION_PATH", definitions.c_str(), 0);
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 0) ConfigureBundledDefinitions(argv[0]);
  const bool inspect_to_file = argc == 4 && std::string(argv[1]) == "inspect";
  const bool inspect_indexed =
      argc == 5 && std::string(argv[1]) == "inspect-indexed";
  const bool frame_to_file = argc == 6 && std::string(argv[1]) == "frame";
  const bool frame_indexed_binary =
      argc == 7 && std::string(argv[1]) == "frame-indexed-bin";
  const bool frames_indexed_binary =
      argc >= 7 && std::string(argv[1]) == "frames-indexed-bin";
  const bool table_to_file = argc == 6 && std::string(argv[1]) == "table";
  const char* result_path = inspect_to_file ? argv[3]
                            : inspect_indexed ? argv[4]
                            : frame_to_file ? argv[5]
                            : frame_indexed_binary ? argv[6]
                            : frames_indexed_binary ? argv[5]
                            : table_to_file ? argv[5]
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
    if (inspect_indexed) {
      Json::Value index(Json::objectValue);
      const auto result = Inspect(argv[2], &index);
      WriteResult(index, argv[3]);
      WriteResult(result, result_path);
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
    if (frame_indexed_binary) {
      const auto frame = DecodeFrame(argv[2], argv[4], ParseLimit(argv[5]),
                                     std::filesystem::path(argv[3]));
      WriteBinaryFrames({frame}, result_path);
      return 0;
    }
    if (frames_indexed_binary) {
      std::vector<Json::Value> frames;
      frames.reserve(static_cast<size_t>(argc - 6));
      for (int index = 6; index < argc; ++index)
        frames.push_back(DecodeFrame(argv[2], argv[index], ParseLimit(argv[4]),
                                     std::filesystem::path(argv[3])));
      WriteBinaryFrames(frames, result_path);
      return 0;
    }
    if (table_to_file) {
      const auto result =
          WeatherTable(argv[2], std::stod(argv[3]), std::stod(argv[4]));
      WriteResult(result, result_path);
      return 0;
    }
    std::cerr << "usage: igrib-environment-helper inspect FILE [RESULT_JSON]\n"
                 "       igrib-environment-helper inspect-indexed FILE "
                 "INDEX_JSON RESULT_JSON\n"
                 "       igrib-environment-helper frame FILE TIME MAX_POINTS "
                 "[RESULT_JSON]\n"
                 "       igrib-environment-helper frame-indexed-bin FILE "
                 "INDEX_JSON TIME MAX_POINTS RESULT_BIN\n"
                 "       igrib-environment-helper frames-indexed-bin FILE "
                 "INDEX_JSON MAX_POINTS RESULT_BIN TIME...\n"
                 "       igrib-environment-helper table FILE LAT LON "
                 "RESULT_JSON\n";
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
