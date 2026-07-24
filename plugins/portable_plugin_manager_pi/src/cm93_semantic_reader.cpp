#include "cm93_semantic_reader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>

namespace ppm {
namespace {

namespace fs = std::filesystem;

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegree = kPi / 180.0;
constexpr double kCm93SemiMajorAxisMetres = 6378388.0;
constexpr double kNmPerDegreeLatitude = 60.0;
constexpr double kSemanticSampleNm = 0.025;
constexpr std::size_t kMaxRecords = 2000000;
constexpr std::size_t kMaxPoints = 20000000;
constexpr auto kCellModificationCheckInterval = std::chrono::seconds(30);

const std::array<unsigned char, 256> kTable0 = {
    0xCD, 0xEA, 0xDC, 0x48, 0x3E, 0x6D, 0xCA, 0x7B, 0x52, 0xE1, 0xA4, 0x8E,
    0xAB, 0x05, 0xA7, 0x97, 0xB9, 0x60, 0x39, 0x85, 0x7C, 0x56, 0x7A, 0xBA,
    0x68, 0x6E, 0xF5, 0x5D, 0x02, 0x4E, 0x0F, 0xA1, 0x27, 0x24, 0x41, 0x34,
    0x00, 0x5A, 0xFE, 0xCB, 0xD0, 0xFA, 0xF8, 0x6C, 0x74, 0x96, 0x9E, 0x0E,
    0xC2, 0x49, 0xE3, 0xE5, 0xC0, 0x3B, 0x59, 0x18, 0xA9, 0x86, 0x8F, 0x30,
    0xC3, 0xA8, 0x22, 0x0A, 0x14, 0x1A, 0xB2, 0xC9, 0xC7, 0xED, 0xAA, 0x29,
    0x94, 0x75, 0x0D, 0xAC, 0x0C, 0xF4, 0xBB, 0xC5, 0x3F, 0xFD, 0xD9, 0x9C,
    0x4F, 0xD5, 0x84, 0x1E, 0xB1, 0x81, 0x69, 0xB4, 0x09, 0xB8, 0x3C, 0xAF,
    0xA3, 0x08, 0xBF, 0xE0, 0x9A, 0xD7, 0xF7, 0x8C, 0x67, 0x66, 0xAE, 0xD4,
    0x4C, 0xA5, 0xEC, 0xF9, 0xB6, 0x64, 0x78, 0x06, 0x5B, 0x9B, 0xF2, 0x99,
    0xCE, 0xDB, 0x53, 0x55, 0x65, 0x8D, 0x07, 0x33, 0x04, 0x37, 0x92, 0x26,
    0x23, 0xB5, 0x58, 0xDA, 0x2F, 0xB3, 0x40, 0x5E, 0x7F, 0x4B, 0x62, 0x80,
    0xE4, 0x6F, 0x73, 0x1D, 0xDF, 0x17, 0xCC, 0x28, 0x25, 0x2D, 0xEE, 0x3A,
    0x98, 0xE2, 0x01, 0xEB, 0xDD, 0xBC, 0x90, 0xB0, 0xFC, 0x95, 0x76, 0x93,
    0x46, 0x57, 0x2C, 0x2B, 0x50, 0x11, 0x0B, 0xC1, 0xF0, 0xE7, 0xD6, 0x21,
    0x31, 0xDE, 0xFF, 0xD8, 0x12, 0xA6, 0x4D, 0x8A, 0x13, 0x43, 0x45, 0x38,
    0xD2, 0x87, 0xA0, 0xEF, 0x82, 0xF1, 0x47, 0x89, 0x6A, 0xC8, 0x54, 0x1B,
    0x16, 0x7E, 0x79, 0xBD, 0x6B, 0x91, 0xA2, 0x71, 0x36, 0xB7, 0x03, 0x3D,
    0x72, 0xC6, 0x44, 0x8B, 0xCF, 0x15, 0x9F, 0x32, 0xC4, 0x77, 0x83, 0x63,
    0x20, 0x88, 0xF6, 0xAD, 0xF3, 0xE8, 0x4A, 0xE9, 0x35, 0x1C, 0x5F, 0x19,
    0x1F, 0x7D, 0x70, 0xFB, 0xD1, 0x51, 0x10, 0xD3, 0x2E, 0x61, 0x9D, 0x5C,
    0x2A, 0x42, 0xBE, 0xE6};

std::array<unsigned char, 256> MakeDecodeTable() {
  std::array<unsigned char, 256> table{};
  for (std::size_t i = 0; i < kTable0.size(); ++i)
    table[static_cast<unsigned char>(kTable0[i] ^ 8U)] =
        static_cast<unsigned char>(i);
  return table;
}

const std::array<unsigned char, 256> kDecodeTable = MakeDecodeTable();

class DecodedReader {
public:
  explicit DecodedReader(const fs::path& path)
      : stream_(path, std::ios::binary), path_(path) {}

  bool Good() const { return stream_.good(); }
  std::uint64_t Position() {
    const auto position = stream_.tellg();
    return position < 0 ? 0U : static_cast<std::uint64_t>(position);
  }

  bool Bytes(void* output, std::size_t count) {
    if (count == 0) return true;
    stream_.read(static_cast<char*>(output),
                 static_cast<std::streamsize>(count));
    if (!stream_) return false;
    auto* bytes = static_cast<unsigned char*>(output);
    for (std::size_t i = 0; i < count; ++i) bytes[i] = kDecodeTable[bytes[i]];
    return true;
  }

  template <typename T>
  bool Scalar(T* output) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "CM93 scalar must be trivially copyable");
    return Bytes(output, sizeof(T));
  }

  const fs::path& Path() const { return path_; }

private:
  std::ifstream stream_;
  fs::path path_;
};

struct RawPoint {
  std::uint16_t x = 0;
  std::uint16_t y = 0;
};

struct Edge {
  std::vector<RawPoint> points;
};

struct EdgeReference {
  std::uint16_t index = 0;
  std::uint8_t usage = 0;
};

struct RawObject {
  std::uint8_t object_type = 0;
  std::uint8_t geometry_type = 0;
  std::uint8_t attribute_count = 0;
  std::vector<EdgeReference> edges;
  std::vector<unsigned char> attributes;
};

struct RawCell {
  double transform_x_rate = 0.0;
  double transform_y_rate = 0.0;
  double transform_x_origin = 0.0;
  double transform_y_origin = 0.0;
  std::vector<Edge> edges;
  std::vector<RawObject> objects;
};

struct Dictionary {
  struct Attribute {
    std::string name;
    char type = '?';
  };

  std::map<unsigned int, std::string> classes;
  std::map<unsigned int, Attribute> attributes;
};

struct BBox {
  double min_lat = std::numeric_limits<double>::infinity();
  double max_lat = -std::numeric_limits<double>::infinity();
  double min_lon = std::numeric_limits<double>::infinity();
  double max_lon = -std::numeric_limits<double>::infinity();

  void Add(const SemanticGeoPoint& point) {
    min_lat = std::min(min_lat, point.latitude);
    max_lat = std::max(max_lat, point.latitude);
    min_lon = std::min(min_lon, point.longitude);
    max_lon = std::max(max_lon, point.longitude);
  }

  bool Contains(const SemanticGeoPoint& point,
                double margin_degrees = 0.0) const {
    return point.latitude >= min_lat - margin_degrees &&
           point.latitude <= max_lat + margin_degrees &&
           point.longitude >= min_lon - margin_degrees &&
           point.longitude <= max_lon + margin_degrees;
  }

  bool Intersects(const BBox& other) const {
    return !(max_lat < other.min_lat || min_lat > other.max_lat ||
             max_lon < other.min_lon || min_lon > other.max_lon);
  }
};

enum class AreaKind { kLand, kDrying, kDepth, kCoverage };

struct SemanticArea {
  AreaKind kind = AreaKind::kLand;
  bool has_minimum_depth = false;
  double minimum_depth_metres = 0.0;
  std::vector<std::vector<SemanticGeoPoint>> rings;
  BBox bounds;
};

class SemanticAreaIndex {
public:
  void Build(const std::vector<SemanticArea>& areas,
             const std::vector<std::uint32_t>& indices) {
    bins_.clear();
    global_.clear();
    for (const std::uint32_t index : indices) {
      const auto& bounds = areas[index].bounds;
      const int minimum_latitude = Bin(bounds.min_lat);
      const int maximum_latitude = Bin(bounds.max_lat);
      const int minimum_longitude = Bin(bounds.min_lon);
      const int maximum_longitude = Bin(bounds.max_lon);
      const std::uint64_t bin_count =
          static_cast<std::uint64_t>(maximum_latitude - minimum_latitude + 1) *
          static_cast<std::uint64_t>(maximum_longitude - minimum_longitude + 1);
      if (bin_count > kMaximumBinsPerArea) {
        global_.push_back(index);
        continue;
      }
      for (int latitude = minimum_latitude; latitude <= maximum_latitude;
           ++latitude) {
        for (int longitude = minimum_longitude; longitude <= maximum_longitude;
             ++longitude) {
          bins_[{latitude, longitude}].push_back(index);
        }
      }
    }
  }

  std::vector<std::uint32_t> Query(const BBox& bounds) const {
    std::vector<std::uint32_t> candidates = global_;
    const int minimum_latitude = Bin(bounds.min_lat);
    const int maximum_latitude = Bin(bounds.max_lat);
    const int minimum_longitude = Bin(bounds.min_lon);
    const int maximum_longitude = Bin(bounds.max_lon);
    for (int latitude = minimum_latitude; latitude <= maximum_latitude;
         ++latitude) {
      for (int longitude = minimum_longitude; longitude <= maximum_longitude;
           ++longitude) {
        const auto found = bins_.find({latitude, longitude});
        if (found == bins_.end()) continue;
        candidates.insert(candidates.end(), found->second.begin(),
                          found->second.end());
      }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    return candidates;
  }

private:
  static constexpr double kBinDegrees = 0.05;
  static constexpr std::uint64_t kMaximumBinsPerArea = 4096;

  static int Bin(double coordinate) {
    return static_cast<int>(std::floor(coordinate / kBinDegrees));
  }

  std::map<std::pair<int, int>, std::vector<std::uint32_t>> bins_;
  std::vector<std::uint32_t> global_;
};

struct SemanticCell {
  fs::path path;
  int detail = 0;
  std::vector<SemanticArea> areas;
  std::vector<std::uint32_t> hazard_areas;
  std::vector<std::uint32_t> classified_areas;
  SemanticAreaIndex hazard_index;
  SemanticAreaIndex classified_index;
};

std::vector<std::string> Split(const std::string& line, char separator) {
  std::vector<std::string> fields;
  std::stringstream stream(line);
  std::string field;
  while (std::getline(stream, field, separator)) fields.push_back(field);
  return fields;
}

std::string Lower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool IsCm93Root(const fs::path& root) {
  std::error_code error;
  return fs::is_regular_file(root / "CM93OBJ.DIC", error) ||
         fs::is_regular_file(root / "cm93obj.dic", error);
}

fs::path FindCaseFile(const fs::path& root, const char* upper,
                      const char* lower) {
  std::error_code error;
  fs::path candidate = root / upper;
  if (fs::is_regular_file(candidate, error)) return candidate;
  candidate = root / lower;
  if (fs::is_regular_file(candidate, error)) return candidate;
  return {};
}

bool LoadDictionary(const fs::path& root, Dictionary* dictionary,
                    std::string* diagnostic) {
  if (!dictionary) return false;
  const fs::path object_path = FindCaseFile(root, "CM93OBJ.DIC", "cm93obj.dic");
  fs::path attribute_path = FindCaseFile(root, "CM93ATTR.DIC", "cm93attr.dic");
  if (attribute_path.empty())
    attribute_path = FindCaseFile(root, "ATTRLUT.DIC", "attrlut.dic");
  if (object_path.empty() || attribute_path.empty()) {
    if (diagnostic) *diagnostic = "CM93 dictionary files are incomplete";
    return false;
  }

  std::ifstream objects(object_path);
  std::string line;
  while (std::getline(objects, line)) {
    const auto fields = Split(line, '|');
    if (fields.size() < 2) continue;
    try {
      dictionary->classes.emplace(
          static_cast<unsigned int>(std::stoul(fields[1])), fields[0]);
    } catch (...) {
    }
  }

  std::ifstream attributes(attribute_path);
  while (std::getline(attributes, line)) {
    if (line.empty() || line[0] == ';') continue;
    const auto fields = Split(line, '|');
    if (fields.size() < 3) continue;
    try {
      const unsigned int index =
          static_cast<unsigned int>(std::stoul(fields[1]));
      const std::string type_name = fields.size() >= 7 ? fields[6] : fields[2];
      char type = '?';
      if (type_name.find("aFLOAT") != std::string::npos)
        type = 'R';
      else if (type_name.find("aBYTE") != std::string::npos)
        type = 'B';
      else if (type_name.find("aSTRING") != std::string::npos)
        type = 'S';
      else if (type_name.find("aCMPLX") != std::string::npos)
        type = 'C';
      else if (type_name.find("aLIST") != std::string::npos)
        type = 'L';
      else if (type_name.find("aWORD10") != std::string::npos)
        type = 'W';
      else if (type_name.find("aLONG") != std::string::npos)
        type = 'G';
      dictionary->attributes.emplace(index,
                                     Dictionary::Attribute{fields[0], type});
    } catch (...) {
    }
  }

  const bool useful =
      std::any_of(dictionary->classes.begin(), dictionary->classes.end(),
                  [](const auto& item) { return item.second == "LNDARE"; });
  if (!useful && diagnostic)
    *diagnostic = "CM93 object dictionary has no LNDARE class";
  return useful;
}

bool CountSane(std::int64_t count, std::size_t maximum = kMaxRecords) {
  return count >= 0 && static_cast<std::uint64_t>(count) <= maximum;
}

bool ReadRawCell(const fs::path& path, RawCell* cell, std::string* diagnostic) {
  if (!cell) return false;
  std::error_code error;
  const std::uintmax_t file_size = fs::file_size(path, error);
  if (error || file_size < 138) {
    if (diagnostic) *diagnostic = "CM93 cell is absent or too short";
    return false;
  }

  DecodedReader reader(path);
  std::uint16_t prolog_and_header = 0;
  std::int32_t table1 = 0;
  std::int32_t table2 = 0;
  if (!reader.Scalar(&prolog_and_header) || !reader.Scalar(&table1) ||
      !reader.Scalar(&table2) ||
      static_cast<std::int64_t>(prolog_and_header) + table1 + table2 !=
          static_cast<std::int64_t>(file_size)) {
    if (diagnostic) *diagnostic = "CM93 cell prolog failed integrity check";
    return false;
  }

  double lon_min = 0.0, lat_min = 0.0, lon_max = 0.0, lat_max = 0.0;
  double easting_min = 0.0, northing_min = 0.0, easting_max = 0.0,
         northing_max = 0.0;
  std::uint16_t vector_records = 0, point3d_records = 0, point2d_records = 0,
                feature_records = 0;
  std::int32_t vector_points = 0, descriptor_count_a = 0,
               descriptor_count_b = 0, point3d_values = 0, attribute_bytes = 0,
               related_pointers = 0;
  std::int32_t unused_i = 0;
  std::uint16_t unused_s = 0;

  if (!reader.Scalar(&lon_min) || !reader.Scalar(&lat_min) ||
      !reader.Scalar(&lon_max) || !reader.Scalar(&lat_max) ||
      !reader.Scalar(&easting_min) || !reader.Scalar(&northing_min) ||
      !reader.Scalar(&easting_max) || !reader.Scalar(&northing_max) ||
      !reader.Scalar(&vector_records) || !reader.Scalar(&vector_points) ||
      !reader.Scalar(&descriptor_count_a) ||
      !reader.Scalar(&descriptor_count_b) || !reader.Scalar(&point3d_records) ||
      !reader.Scalar(&point3d_values) || !reader.Scalar(&unused_i) ||
      !reader.Scalar(&point2d_records) || !reader.Scalar(&unused_s) ||
      !reader.Scalar(&unused_s) || !reader.Scalar(&feature_records) ||
      !reader.Scalar(&unused_i) || !reader.Scalar(&unused_i) ||
      !reader.Scalar(&unused_s) || !reader.Scalar(&unused_s) ||
      !reader.Scalar(&unused_s) || !reader.Scalar(&related_pointers) ||
      !reader.Scalar(&unused_i) || !reader.Scalar(&unused_s) ||
      !reader.Scalar(&attribute_bytes) || !reader.Scalar(&unused_i)) {
    if (diagnostic) *diagnostic = "CM93 cell header is truncated";
    return false;
  }

  if (!CountSane(vector_records) || !CountSane(vector_points, kMaxPoints) ||
      !CountSane(point3d_records) || !CountSane(point3d_values, kMaxPoints) ||
      !CountSane(point2d_records, kMaxPoints) || !CountSane(feature_records) ||
      !CountSane(attribute_bytes, kMaxPoints) ||
      !CountSane(related_pointers, kMaxPoints) ||
      !CountSane(
          static_cast<std::int64_t>(descriptor_count_a) + descriptor_count_b,
          kMaxPoints)) {
    if (diagnostic) *diagnostic = "CM93 cell header contains unsafe counts";
    return false;
  }

  double delta_x = easting_max - easting_min;
  if (delta_x < 0.0) delta_x += kCm93SemiMajorAxisMetres * 2.0 * kPi;
  cell->transform_x_rate = delta_x / 65535.0;
  cell->transform_y_rate = (northing_max - northing_min) / 65535.0;
  cell->transform_x_origin = easting_min;
  cell->transform_y_origin = northing_min;

  cell->edges.resize(vector_records);
  std::size_t total_points = 0;
  for (auto& edge : cell->edges) {
    std::uint16_t count = 0;
    if (!reader.Scalar(&count) || total_points + count > kMaxPoints) {
      if (diagnostic) *diagnostic = "CM93 vector table is invalid";
      return false;
    }
    edge.points.resize(count);
    for (auto& point : edge.points) {
      if (!reader.Scalar(&point.x) || !reader.Scalar(&point.y)) {
        if (diagnostic) *diagnostic = "CM93 vector table is truncated";
        return false;
      }
    }
    total_points += count;
  }

  for (std::uint16_t i = 0; i < point3d_records; ++i) {
    std::uint16_t count = 0;
    if (!reader.Scalar(&count) || count > kMaxPoints) return false;
    std::array<std::uint16_t, 3> point{};
    for (std::uint16_t j = 0; j < count; ++j)
      if (!reader.Scalar(&point[0]) || !reader.Scalar(&point[1]) ||
          !reader.Scalar(&point[2]))
        return false;
  }
  std::array<std::uint16_t, 2> point2d{};
  for (std::uint16_t i = 0; i < point2d_records; ++i)
    if (!reader.Scalar(&point2d[0]) || !reader.Scalar(&point2d[1]))
      return false;

  cell->objects.reserve(feature_records);
  for (std::uint16_t object_index = 0; object_index < feature_records;
       ++object_index) {
    RawObject object;
    std::uint16_t descriptor_bytes = 0;
    if (!reader.Scalar(&object.object_type) ||
        !reader.Scalar(&object.geometry_type) ||
        !reader.Scalar(&descriptor_bytes)) {
      if (diagnostic) *diagnostic = "CM93 feature table is truncated";
      return false;
    }
    std::int64_t remaining = descriptor_bytes;
    const unsigned int primitive = object.geometry_type & 0x0FU;
    if (primitive == 4U || primitive == 2U) {
      std::uint16_t count = 0;
      if (!reader.Scalar(&count)) return false;
      remaining -= static_cast<std::int64_t>(count) * 2 + 2;
      object.edges.reserve(count);
      for (std::uint16_t i = 0; i < count; ++i) {
        std::uint16_t encoded_index = 0;
        if (!reader.Scalar(&encoded_index)) return false;
        const std::uint16_t index = encoded_index & 0x1FFFU;
        if (index >= cell->edges.size()) {
          if (diagnostic)
            *diagnostic = "CM93 feature references an invalid edge";
          return false;
        }
        object.edges.push_back(
            {index, static_cast<std::uint8_t>(encoded_index >> 13U)});
      }
    } else if (primitive == 1U || primitive == 8U) {
      std::uint16_t ignored_index = 0;
      if (!reader.Scalar(&ignored_index)) return false;
      remaining -= 2;
    }

    if ((object.geometry_type & 0x10U) != 0U) {
      std::uint8_t count = 0;
      if (!reader.Scalar(&count)) return false;
      remaining -= static_cast<std::int64_t>(count) * 2 + 1;
      std::uint16_t ignored = 0;
      for (std::uint8_t i = 0; i < count; ++i)
        if (!reader.Scalar(&ignored)) return false;
    }
    if ((object.geometry_type & 0x20U) != 0U) {
      std::uint16_t ignored = 0;
      if (!reader.Scalar(&ignored)) return false;
      remaining -= 2;
    }
    if ((object.geometry_type & 0x80U) != 0U) {
      if (!reader.Scalar(&object.attribute_count)) return false;
      remaining -= 5;
      if (remaining < 0 || static_cast<std::uint64_t>(remaining) > kMaxPoints) {
        if (diagnostic) *diagnostic = "CM93 feature attribute size is invalid";
        return false;
      }
      object.attributes.resize(static_cast<std::size_t>(remaining));
      if (!reader.Bytes(object.attributes.data(), object.attributes.size()))
        return false;
    }
    cell->objects.push_back(std::move(object));
  }

  return true;
}

SemanticGeoPoint Transform(const RawCell& cell, const RawPoint& point) {
  const double x = point.x * cell.transform_x_rate + cell.transform_x_origin;
  const double y = point.y * cell.transform_y_rate + cell.transform_y_origin;
  SemanticGeoPoint output;
  output.latitude =
      (2.0 * std::atan(std::exp(y / kCm93SemiMajorAxisMetres)) - kPi / 2.0) /
      kDegree;
  output.longitude = x / (kDegree * kCm93SemiMajorAxisMetres);
  while (output.longitude > 180.0) output.longitude -= 360.0;
  while (output.longitude < -180.0) output.longitude += 360.0;
  return output;
}

bool SamePoint(const RawPoint& a, const RawPoint& b) {
  return a.x == b.x && a.y == b.y;
}

std::vector<std::vector<SemanticGeoPoint>> BuildRings(const RawCell& cell,
                                                      const RawObject& object) {
  std::vector<std::vector<SemanticGeoPoint>> rings;
  std::vector<SemanticGeoPoint> ring;
  RawPoint ring_start{};
  bool have_start = false;
  for (const auto& reference : object.edges) {
    if (reference.index >= cell.edges.size()) return {};
    const auto& points = cell.edges[reference.index].points;
    if (points.empty()) continue;
    const bool reversed = (reference.usage & 4U) != 0U;
    const RawPoint& segment_start = reversed ? points.back() : points.front();
    const RawPoint& segment_end = reversed ? points.front() : points.back();
    if (!have_start) {
      ring_start = segment_start;
      have_start = true;
    }
    if (!reversed) {
      for (std::size_t i = 0; i < points.size(); ++i) {
        if (!ring.empty() && i == 0) continue;
        ring.push_back(Transform(cell, points[i]));
      }
    } else {
      for (std::size_t i = points.size(); i > 0; --i) {
        if (!ring.empty() && i == points.size()) continue;
        ring.push_back(Transform(cell, points[i - 1]));
      }
    }
    if (SamePoint(segment_end, ring_start)) {
      if (ring.size() >= 3) {
        if (ring.front().latitude != ring.back().latitude ||
            ring.front().longitude != ring.back().longitude)
          ring.push_back(ring.front());
        rings.push_back(std::move(ring));
      }
      ring.clear();
      have_start = false;
    }
  }
  return rings;
}

bool ReadFloatAttribute(const RawObject& object, const Dictionary& dictionary,
                        const std::string& wanted, double* value) {
  std::size_t cursor = 0;
  for (unsigned int count = 0; count < object.attribute_count; ++count) {
    if (cursor >= object.attributes.size()) return false;
    const unsigned int index = object.attributes[cursor++];
    const auto found = dictionary.attributes.find(index);
    if (found == dictionary.attributes.end()) return false;
    const auto& attribute = found->second;
    const std::size_t value_start = cursor;
    switch (attribute.type) {
      case 'B':
        cursor += 1;
        break;
      case 'R':
        cursor += 4;
        break;
      case 'W':
        cursor += 2;
        break;
      case 'G':
        cursor += 4;
        break;
      case 'S':
        while (cursor < object.attributes.size() &&
               object.attributes[cursor++] != 0U) {
        }
        break;
      case 'C':
        cursor += 3;
        while (cursor < object.attributes.size() &&
               object.attributes[cursor++] != 0U) {
        }
        break;
      case 'L':
        if (cursor < object.attributes.size())
          cursor += 1 + object.attributes[cursor];
        break;
      default:
        return false;
    }
    if (cursor > object.attributes.size()) return false;
    if (attribute.name == wanted && attribute.type == 'R' &&
        value_start + sizeof(float) <= object.attributes.size()) {
      float decoded = 0.0F;
      std::memcpy(&decoded, object.attributes.data() + value_start,
                  sizeof(decoded));
      if (std::isfinite(decoded)) {
        if (value) *value = decoded;
        return true;
      }
    }
  }
  return false;
}

std::shared_ptr<SemanticCell> DecodeCell(const fs::path& path, int detail,
                                         const Dictionary& dictionary,
                                         std::string* diagnostic) {
  RawCell raw;
  if (!ReadRawCell(path, &raw, diagnostic)) return {};
  auto cell = std::make_shared<SemanticCell>();
  cell->path = path;
  cell->detail = detail;
  for (const auto& object : raw.objects) {
    if ((object.geometry_type & 0x0FU) != 4U) continue;
    const auto class_it = dictionary.classes.find(object.object_type);
    if (class_it == dictionary.classes.end()) continue;
    const std::string& class_name = class_it->second;
    SemanticArea area;
    if (class_name == "LNDARE")
      area.kind = AreaKind::kLand;
    else if (class_name == "DRGARE" || class_name == "ITDARE")
      area.kind = AreaKind::kDrying;
    else if (class_name == "DEPARE")
      area.kind = AreaKind::kDepth;
    else if (class_name == "_m_sor")
      area.kind = AreaKind::kCoverage;
    else
      continue;
    area.rings = BuildRings(raw, object);
    if (area.rings.empty()) continue;
    for (const auto& ring : area.rings)
      for (const auto& point : ring) area.bounds.Add(point);
    if (area.kind == AreaKind::kDepth)
      area.has_minimum_depth = ReadFloatAttribute(object, dictionary, "DRVAL1",
                                                  &area.minimum_depth_metres);
    cell->areas.push_back(std::move(area));
  }
  if (cell->areas.empty()) {
    if (diagnostic) *diagnostic = "CM93 cell contains no safety area semantics";
    return {};
  }
  for (std::size_t index = 0; index < cell->areas.size(); ++index) {
    const auto kind = cell->areas[index].kind;
    if (kind == AreaKind::kLand || kind == AreaKind::kDrying)
      cell->hazard_areas.push_back(static_cast<std::uint32_t>(index));
    if (kind == AreaKind::kCoverage || kind == AreaKind::kDepth)
      cell->classified_areas.push_back(static_cast<std::uint32_t>(index));
  }
  cell->hazard_index.Build(cell->areas, cell->hazard_areas);
  cell->classified_index.Build(cell->areas, cell->classified_areas);
  return cell;
}

struct Scale {
  int detail;
  int denominator;
  int divisor;
  char letter;
};

const std::array<Scale, 8> kScales = {{{7, 7500, 1, 'G'},
                                       {6, 20000, 1, 'F'},
                                       {5, 50000, 1, 'E'},
                                       {4, 100000, 3, 'D'},
                                       {3, 200000, 12, 'C'},
                                       {2, 1000000, 30, 'B'},
                                       {1, 3000000, 60, 'A'},
                                       {0, 20000000, 120, 'Z'}}};

int CellIndex(double latitude, double longitude, int divisor) {
  double longitude_value = (longitude + 360.0) * 3.0;
  while (longitude_value >= 1080.0) longitude_value -= 1080.0;
  const int longitude_index =
      static_cast<int>(std::floor(longitude_value / divisor)) * divisor;
  const double latitude_value = latitude * 3.0 + 240.0;
  const int latitude_index =
      static_cast<int>(std::floor(latitude_value / divisor)) * divisor;
  return longitude_index + (latitude_index + 30) * 10000;
}

fs::path FindCell(const fs::path& root, const SemanticGeoPoint& point,
                  const Scale& scale) {
  const int cell_index =
      CellIndex(point.latitude, point.longitude, scale.divisor);
  const int latitude_index = cell_index / 10000;
  const int longitude_index = cell_index % 10000;
  const int latitude_cell =
      ((latitude_index - 30) / scale.divisor) * scale.divisor + 30;
  const int longitude_cell = (longitude_index / scale.divisor) * scale.divisor;
  const int latitude_root = ((latitude_index - 30) / 60) * 60 + 30;
  const int longitude_root = (longitude_index / 60) * 60;

  char root_name[16]{};
  std::snprintf(root_name, sizeof(root_name), "%04d%04d", latitude_root,
                longitude_root);
  fs::path directory = root / root_name / std::string(1, scale.letter);
  std::error_code error;
  if (!fs::is_directory(directory, error)) {
    directory = root / root_name / Lower(std::string(1, scale.letter));
  }
  if (!fs::is_directory(directory, error)) return {};

  char suffix[24]{};
  std::snprintf(suffix, sizeof(suffix), "%03d%04d.%c", latitude_cell,
                longitude_cell, scale.letter);
  const std::string expected_suffix = Lower(suffix);
  for (fs::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error)) continue;
    const std::string name = Lower(iterator->path().filename().string());
    if (name.size() == expected_suffix.size() + 1 &&
        name.compare(1, expected_suffix.size(), expected_suffix) == 0)
      return iterator->path();
  }
  return {};
}

double NormalizeLongitudeDelta(double value) {
  while (value > 180.0) value -= 360.0;
  while (value < -180.0) value += 360.0;
  return value;
}

std::vector<SemanticGeoPoint> SegmentCellRepresentatives(
    const SemanticGeoPoint& start, const SemanticGeoPoint& end,
    const Scale& scale, double safety_margin_nautical_miles) {
  const double end_longitude =
      start.longitude +
      NormalizeLongitudeDelta(end.longitude - start.longitude);
  const double x0 = (start.longitude + 360.0) * 3.0 / scale.divisor;
  const double y0 = (start.latitude * 3.0 + 240.0) / scale.divisor;
  const double x1 = (end_longitude + 360.0) * 3.0 / scale.divisor;
  const double y1 = (end.latitude * 3.0 + 240.0) / scale.divisor;
  const double dx = x1 - x0;
  const double dy = y1 - y0;
  const int step_x = dx > 0.0 ? 1 : (dx < 0.0 ? -1 : 0);
  const int step_y = dy > 0.0 ? 1 : (dy < 0.0 ? -1 : 0);
  int cell_x = static_cast<int>(std::floor(x0));
  int cell_y = static_cast<int>(std::floor(y0));
  const int end_x = static_cast<int>(std::floor(x1));
  const int end_y = static_cast<int>(std::floor(y1));
  const double infinity = std::numeric_limits<double>::infinity();
  const double delta_x = step_x == 0 ? infinity : 1.0 / std::abs(dx);
  const double delta_y = step_y == 0 ? infinity : 1.0 / std::abs(dy);
  double maximum_x =
      step_x > 0 ? (std::floor(x0) + 1.0 - x0) * delta_x
                 : (step_x < 0 ? (x0 - std::floor(x0)) * delta_x : infinity);
  double maximum_y =
      step_y > 0 ? (std::floor(y0) + 1.0 - y0) * delta_y
                 : (step_y < 0 ? (y0 - std::floor(y0)) * delta_y : infinity);

  const double maximum_absolute_latitude = std::min(
      89.9, std::max(std::abs(start.latitude), std::abs(end.latitude)));
  const double latitude_margin =
      safety_margin_nautical_miles / kNmPerDegreeLatitude;
  const double longitude_margin =
      latitude_margin /
      std::max(0.01, std::cos(maximum_absolute_latitude * kDegree));
  const int margin_x = static_cast<int>(
      std::ceil(longitude_margin * 3.0 / static_cast<double>(scale.divisor)));
  const int margin_y = static_cast<int>(
      std::ceil(latitude_margin * 3.0 / static_cast<double>(scale.divisor)));

  std::set<std::pair<int, int>> cells;
  auto add = [&](int x, int y) {
    for (int offset_y = -margin_y; offset_y <= margin_y; ++offset_y) {
      for (int offset_x = -margin_x; offset_x <= margin_x; ++offset_x) {
        cells.emplace(x + offset_x, y + offset_y);
      }
    }
  };
  add(cell_x, cell_y);
  while (cell_x != end_x || cell_y != end_y) {
    if (maximum_x < maximum_y) {
      cell_x += step_x;
      maximum_x += delta_x;
    } else if (maximum_y < maximum_x) {
      cell_y += step_y;
      maximum_y += delta_y;
    } else {
      // A segment through a grid corner touches both adjacent cells as well
      // as the diagonal destination cell. Include all four conservatively.
      add(cell_x + step_x, cell_y);
      add(cell_x, cell_y + step_y);
      cell_x += step_x;
      cell_y += step_y;
      maximum_x += delta_x;
      maximum_y += delta_y;
    }
    add(cell_x, cell_y);
  }

  std::vector<SemanticGeoPoint> representatives;
  representatives.reserve(cells.size());
  for (const auto& cell : cells) {
    SemanticGeoPoint point;
    point.longitude =
        (static_cast<double>(cell.first) + 0.5) * scale.divisor / 3.0 - 360.0;
    while (point.longitude > 180.0) point.longitude -= 360.0;
    while (point.longitude < -180.0) point.longitude += 360.0;
    point.latitude =
        ((static_cast<double>(cell.second) + 0.5) * scale.divisor - 240.0) /
        3.0;
    if (point.latitude >= -90.0 && point.latitude <= 90.0)
      representatives.push_back(point);
  }
  return representatives;
}

double DistanceNm(const SemanticGeoPoint& a, const SemanticGeoPoint& b) {
  const double mean_latitude = (a.latitude + b.latitude) * 0.5 * kDegree;
  const double dx = NormalizeLongitudeDelta(b.longitude - a.longitude) *
                    std::cos(mean_latitude) * kNmPerDegreeLatitude;
  const double dy = (b.latitude - a.latitude) * kNmPerDegreeLatitude;
  return std::hypot(dx, dy);
}

SemanticGeoPoint Interpolate(const SemanticGeoPoint& a,
                             const SemanticGeoPoint& b, double fraction) {
  SemanticGeoPoint point;
  point.latitude = a.latitude + (b.latitude - a.latitude) * fraction;
  point.longitude =
      a.longitude +
      NormalizeLongitudeDelta(b.longitude - a.longitude) * fraction;
  while (point.longitude > 180.0) point.longitude -= 360.0;
  while (point.longitude < -180.0) point.longitude += 360.0;
  return point;
}

bool PointInRing(const SemanticGeoPoint& point,
                 const std::vector<SemanticGeoPoint>& ring) {
  bool inside = false;
  if (ring.size() < 3) return false;
  for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
    const auto& a = ring[i];
    const auto& b = ring[j];
    const bool crosses =
        ((a.latitude > point.latitude) != (b.latitude > point.latitude)) &&
        (point.longitude < (b.longitude - a.longitude) *
                                   (point.latitude - a.latitude) /
                                   ((b.latitude - a.latitude) == 0.0
                                        ? std::numeric_limits<double>::epsilon()
                                        : (b.latitude - a.latitude)) +
                               a.longitude);
    if (crosses) inside = !inside;
  }
  return inside;
}

bool PointInArea(const SemanticGeoPoint& point, const SemanticArea& area) {
  if (!area.bounds.Contains(point)) return false;
  bool inside = false;
  for (const auto& ring : area.rings)
    if (PointInRing(point, ring)) inside = !inside;
  return inside;
}

double Orientation(const SemanticGeoPoint& a, const SemanticGeoPoint& b,
                   const SemanticGeoPoint& c) {
  return (b.longitude - a.longitude) * (c.latitude - a.latitude) -
         (b.latitude - a.latitude) * (c.longitude - a.longitude);
}

bool SegmentsIntersect(const SemanticGeoPoint& a, const SemanticGeoPoint& b,
                       const SemanticGeoPoint& c, const SemanticGeoPoint& d) {
  const double o1 = Orientation(a, b, c);
  const double o2 = Orientation(a, b, d);
  const double o3 = Orientation(c, d, a);
  const double o4 = Orientation(c, d, b);
  constexpr double epsilon = 1e-12;
  return ((o1 > epsilon && o2 < -epsilon) || (o1 < -epsilon && o2 > epsilon)) &&
         ((o3 > epsilon && o4 < -epsilon) || (o3 < -epsilon && o4 > epsilon));
}

struct LocalPointNm {
  double x = 0.0;
  double y = 0.0;
};

LocalPointNm ToLocalNm(const SemanticGeoPoint& point, double reference_latitude,
                       double reference_longitude) {
  return {NormalizeLongitudeDelta(point.longitude - reference_longitude) *
              std::cos(reference_latitude * kDegree) * kNmPerDegreeLatitude,
          (point.latitude - reference_latitude) * kNmPerDegreeLatitude};
}

double PointSegmentDistanceNm(const LocalPointNm& point,
                              const LocalPointNm& start,
                              const LocalPointNm& end) {
  const double dx = end.x - start.x;
  const double dy = end.y - start.y;
  const double length_squared = dx * dx + dy * dy;
  if (length_squared <= std::numeric_limits<double>::epsilon())
    return std::hypot(point.x - start.x, point.y - start.y);
  const double fraction = std::clamp(
      ((point.x - start.x) * dx + (point.y - start.y) * dy) / length_squared,
      0.0, 1.0);
  return std::hypot(point.x - (start.x + fraction * dx),
                    point.y - (start.y + fraction * dy));
}

double SegmentDistanceNm(const SemanticGeoPoint& a, const SemanticGeoPoint& b,
                         const SemanticGeoPoint& c, const SemanticGeoPoint& d) {
  if (SegmentsIntersect(a, b, c, d)) return 0.0;
  const double reference_latitude =
      (a.latitude + b.latitude + c.latitude + d.latitude) * 0.25;
  const double reference_longitude = a.longitude;
  const LocalPointNm local_a =
      ToLocalNm(a, reference_latitude, reference_longitude);
  const LocalPointNm local_b =
      ToLocalNm(b, reference_latitude, reference_longitude);
  const LocalPointNm local_c =
      ToLocalNm(c, reference_latitude, reference_longitude);
  const LocalPointNm local_d =
      ToLocalNm(d, reference_latitude, reference_longitude);
  return std::min({PointSegmentDistanceNm(local_a, local_c, local_d),
                   PointSegmentDistanceNm(local_b, local_c, local_d),
                   PointSegmentDistanceNm(local_c, local_a, local_b),
                   PointSegmentDistanceNm(local_d, local_a, local_b)});
}

bool SegmentHitsArea(const SemanticGeoPoint& start, const SemanticGeoPoint& end,
                     const SemanticArea& area,
                     double safety_margin_nautical_miles) {
  BBox segment_bounds;
  segment_bounds.Add(start);
  segment_bounds.Add(end);
  const double margin_degrees =
      safety_margin_nautical_miles / kNmPerDegreeLatitude;
  segment_bounds.min_lat -= margin_degrees;
  segment_bounds.max_lat += margin_degrees;
  segment_bounds.min_lon -= margin_degrees;
  segment_bounds.max_lon += margin_degrees;
  if (!segment_bounds.Intersects(area.bounds)) return false;
  if (PointInArea(start, area) || PointInArea(end, area)) return true;
  for (const auto& ring : area.rings)
    for (std::size_t i = 1; i < ring.size(); ++i)
      if (SegmentDistanceNm(start, end, ring[i - 1], ring[i]) <=
          safety_margin_nautical_miles)
        return true;
  return false;
}

BBox SegmentQueryBounds(const SemanticGeoPoint& start,
                        const SemanticGeoPoint& end,
                        double safety_margin_nautical_miles) {
  BBox bounds;
  bounds.Add(start);
  bounds.Add(end);
  const double margin_degrees =
      safety_margin_nautical_miles / kNmPerDegreeLatitude;
  bounds.min_lat -= margin_degrees;
  bounds.max_lat += margin_degrees;
  bounds.min_lon -= margin_degrees;
  bounds.max_lon += margin_degrees;
  return bounds;
}

}  // namespace

struct Cm93SemanticReader::Impl {
  struct Root {
    fs::path path;
    Dictionary dictionary;
  };
  struct CachedCell {
    fs::file_time_type modified;
    std::chrono::steady_clock::time_point last_checked;
    std::shared_ptr<SemanticCell> cell;
  };

  mutable std::mutex mutex;
  std::vector<std::shared_ptr<const Root>> roots;
  mutable std::unordered_map<std::string, CachedCell> cache;
  mutable std::unordered_map<std::string, fs::path> cell_path_cache;
  std::string status = "no CM93 chart root has been indexed";

  fs::path Find(const Root& root, const SemanticGeoPoint& point,
                const Scale& scale) const {
    const std::string key =
        root.path.string() + ":" + scale.letter + ":" +
        std::to_string(
            CellIndex(point.latitude, point.longitude, scale.divisor));
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto found = cell_path_cache.find(key);
      if (found != cell_path_cache.end()) return found->second;
    }
    const fs::path path = FindCell(root.path, point, scale);
    {
      std::lock_guard<std::mutex> lock(mutex);
      cell_path_cache.emplace(key, path);
    }
    return path;
  }

  std::shared_ptr<SemanticCell> Load(const Root& root, const fs::path& path,
                                     int detail,
                                     std::string* diagnostic) const {
    const std::string key = path.string();
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto found = cache.find(key);
      if (found != cache.end() &&
          now - found->second.last_checked < kCellModificationCheckInterval) {
        return found->second.cell;
      }
    }

    std::error_code error;
    const auto modified = fs::last_write_time(path, error);
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto found = cache.find(key);
      if (found != cache.end() && !error &&
          found->second.modified == modified) {
        found->second.last_checked = now;
        return found->second.cell;
      }
      if (error) {
        cache.erase(key);
        if (diagnostic)
          *diagnostic = "CM93 cell metadata is no longer available";
        return {};
      }
    }
    auto decoded = DecodeCell(path, detail, root.dictionary, diagnostic);
    if (decoded) {
      std::lock_guard<std::mutex> lock(mutex);
      cache[key] = {modified, now, decoded};
    }
    return decoded;
  }
};

Cm93SemanticReader::Cm93SemanticReader() : impl_(new Impl) {}
Cm93SemanticReader::~Cm93SemanticReader() = default;

void Cm93SemanticReader::SetChartRoots(const std::vector<std::string>& roots) {
  std::vector<std::shared_ptr<const Impl::Root>> indexed;
  std::set<std::string> seen;
  std::string last_error;
  for (const auto& root_string : roots) {
    std::error_code error;
    fs::path root = fs::weakly_canonical(root_string, error);
    if (error) root = fs::path(root_string);
    if (!IsCm93Root(root) || !seen.insert(root.string()).second) continue;
    Dictionary dictionary;
    if (!LoadDictionary(root, &dictionary, &last_error)) continue;
    indexed.push_back(std::make_shared<const Impl::Root>(
        Impl::Root{root, std::move(dictionary)}));
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->roots = std::move(indexed);
  impl_->cache.clear();
  impl_->cell_path_cache.clear();
  if (impl_->roots.empty())
    impl_->status = last_error.empty()
                        ? "no configured CM93 semantic chart root found"
                        : last_error;
  else
    impl_->status = "CM93 semantic chart safety indexed from " +
                    std::to_string(impl_->roots.size()) + " chart root(s)";
}

bool Cm93SemanticReader::Available() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return !impl_->roots.empty();
}

std::string Cm93SemanticReader::Summary() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->status;
}

SemanticSegmentAssessment Cm93SemanticReader::QuerySegment(
    const SemanticGeoPoint& start, const SemanticGeoPoint& end,
    double safety_margin_nautical_miles, double minimum_depth_metres) const {
  SemanticSegmentAssessment result;
  if (!std::isfinite(start.latitude) || !std::isfinite(start.longitude) ||
      !std::isfinite(end.latitude) || !std::isfinite(end.longitude) ||
      !std::isfinite(safety_margin_nautical_miles) ||
      !std::isfinite(minimum_depth_metres) ||
      safety_margin_nautical_miles < 0.0 || minimum_depth_metres < 0.0) {
    result.diagnostic = "invalid CM93 semantic query";
    return result;
  }

  std::vector<std::shared_ptr<const Impl::Root>> roots;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    roots = impl_->roots;
  }
  if (roots.empty()) {
    result.state = SemanticSegmentAssessment::State::kMissingCoverage;
    result.diagnostic = "no configured CM93 semantic chart root";
    return result;
  }

  const double distance_nm = DistanceNm(start, end);
  const std::size_t samples = std::max<std::size_t>(
      2,
      static_cast<std::size_t>(std::ceil(distance_nm / kSemanticSampleNm)) + 1);
  std::map<std::string, std::shared_ptr<SemanticCell>> cells;
  std::string decode_error;
  for (const auto& root_handle : roots) {
    const auto& root = *root_handle;
    for (const auto& scale : kScales) {
      for (const auto& point : SegmentCellRepresentatives(
               start, end, scale, safety_margin_nautical_miles)) {
        const fs::path path = impl_->Find(root, point, scale);
        if (path.empty()) continue;
        auto cell = impl_->Load(root, path, scale.detail, &decode_error);
        if (cell) cells[path.string()] = std::move(cell);
      }
    }
  }
  result.charts_considered = static_cast<std::uint32_t>(cells.size());
  if (cells.empty()) {
    result.state = SemanticSegmentAssessment::State::kMissingCoverage;
    result.diagnostic = decode_error.empty()
                            ? "no CM93 cell covers the segment"
                            : "CM93 cell decode failed: " + decode_error;
    return result;
  }

  const BBox segment_bounds =
      SegmentQueryBounds(start, end, safety_margin_nautical_miles);
  for (const auto& item : cells) {
    const auto& cell = *item.second;
    for (const std::uint32_t area_index :
         cell.hazard_index.Query(segment_bounds)) {
      const auto& area = cell.areas[area_index];
      if (SegmentHitsArea(start, end, area, safety_margin_nautical_miles)) {
        result.state = SemanticSegmentAssessment::State::kUnsafe;
        result.diagnostic =
            area.kind == AreaKind::kLand
                ? "CM93 LNDARE intersects the final route corridor"
                : "CM93 DRGARE/ITDARE intersects the final route corridor";
        return result;
      }
    }
  }

  if (minimum_depth_metres <= 0.0) {
    result.state = SemanticSegmentAssessment::State::kSafe;
    result.diagnostic =
        "authoritative CM93 LNDARE/DRGARE search-time validation passed";
    return result;
  }

  // Depth is checked densely. At each point, use the most detailed cell with
  // semantic coverage; this prevents a coarser cell overriding local detail.
  for (std::size_t i = 0; i < samples; ++i) {
    const auto point =
        Interpolate(start, end, static_cast<double>(i) / (samples - 1));
    BBox point_bounds;
    point_bounds.Add(point);
    int best_detail = -1;
    bool classified = false;
    bool has_depth = false;
    double depth = std::numeric_limits<double>::infinity();
    for (const auto& item : cells) {
      const auto& cell = *item.second;
      bool cell_classified = false;
      bool cell_has_depth = false;
      double cell_depth = std::numeric_limits<double>::infinity();
      for (const std::uint32_t area_index :
           cell.classified_index.Query(point_bounds)) {
        const auto& area = cell.areas[area_index];
        if (!PointInArea(point, area)) continue;
        cell_classified = true;
        if (area.kind == AreaKind::kDepth && area.has_minimum_depth) {
          cell_has_depth = true;
          cell_depth = std::min(cell_depth, area.minimum_depth_metres);
        }
      }
      if (cell_classified && cell.detail > best_detail) {
        best_detail = cell.detail;
        classified = true;
        has_depth = cell_has_depth;
        depth = cell_depth;
      } else if (cell_classified && cell.detail == best_detail &&
                 cell_has_depth) {
        has_depth = true;
        depth = std::min(depth, cell_depth);
      }
    }
    if (!classified || !has_depth) {
      result.state = SemanticSegmentAssessment::State::kMissingCoverage;
      std::ostringstream message;
      message << "CM93 chart coverage does not establish a minimum depth at "
              << point.latitude << ", " << point.longitude
              << " in the final route corridor";
      result.diagnostic = message.str();
      return result;
    }
    if (depth < minimum_depth_metres) {
      result.state = SemanticSegmentAssessment::State::kUnsafe;
      std::ostringstream message;
      message << "CM93 DEPARE minimum depth " << depth
              << " m is below required " << minimum_depth_metres << " m";
      result.diagnostic = message.str();
      return result;
    }
  }

  result.state = SemanticSegmentAssessment::State::kSafe;
  result.diagnostic =
      "authoritative CM93 LNDARE/DRGARE/DEPARE validation passed";
  return result;
}

}  // namespace ppm
