#include "portable_polar.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>

#include "pugixml.hpp"

namespace {
constexpr std::uintmax_t kMaximumInputBytes = 2 * 1024 * 1024;
constexpr size_t kMaximumAxes = 200;
constexpr size_t kMaximumPolars = 8;
constexpr size_t kMaximumTotalCells = 200000;

std::string Trim(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(),
                                      [](unsigned char c) { return std::isspace(c); });
  const auto last = std::find_if_not(value.rbegin(), value.rend(),
                                     [](unsigned char c) { return std::isspace(c); })
                        .base();
  if (first >= last) return {};
  return std::string(first, last);
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool Number(const std::string& text, double* value) {
  if (!value) return false;
  const std::string clean = Trim(text);
  if (clean.empty()) return false;
  size_t consumed = 0;
  try {
    *value = std::stod(clean, &consumed);
  } catch (...) {
    return false;
  }
  return consumed == clean.size() && std::isfinite(*value);
}

std::vector<std::string> Fields(const std::string& line, char delimiter) {
  std::vector<std::string> fields;
  if (delimiter == ' ') {
    std::istringstream input(line);
    std::string field;
    while (input >> field) fields.push_back(field);
    return fields;
  }
  size_t start = 0;
  for (;;) {
    const size_t end = line.find(delimiter, start);
    fields.push_back(Trim(line.substr(start, end - start)));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return fields;
}

bool StrictlyIncreasing(const std::vector<double>& values, double minimum,
                        double maximum) {
  if (values.size() < 2) return false;
  double previous = -std::numeric_limits<double>::infinity();
  for (double value : values) {
    if (!std::isfinite(value) || value < minimum || value > maximum ||
        value <= previous)
      return false;
    previous = value;
  }
  return true;
}

bool InputIsBounded(const std::filesystem::path& path, std::string* error) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > kMaximumInputBytes) {
    if (error)
      *error = ec ? "could not inspect vessel-performance file"
                  : "vessel-performance file exceeds the 2 MiB limit";
    return false;
  }
  return true;
}

bool ParsePolar(const std::filesystem::path& path, PortablePolarGrid* result,
                std::string* error) {
  if (!result || !InputIsBounded(path, error)) return false;
  std::ifstream input(path);
  if (!input) {
    if (error) *error = "could not open polar file: " + path.string();
    return false;
  }
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (lines.empty() && line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xef &&
        static_cast<unsigned char>(line[1]) == 0xbb &&
        static_cast<unsigned char>(line[2]) == 0xbf)
      line.erase(0, 3);
    if (!Trim(line).empty()) lines.push_back(line);
  }
  size_t header_index = lines.size();
  char delimiter = ' ';
  std::vector<std::string> header;
  for (size_t index = 0; index < std::min<size_t>(2, lines.size()); ++index) {
    delimiter = lines[index].find('\t') != std::string::npos
                    ? '\t'
                    : lines[index].find(';') != std::string::npos
                          ? ';'
                          : lines[index].find(',') != std::string::npos ? ',' : ' ';
    header = Fields(lines[index], delimiter);
    if (!header.empty()) {
      const std::string tag = Lower(Trim(header.front()));
      if (tag == "twa/tws" || tag == "twa\\tws" || tag == "twa") {
        header_index = index;
        break;
      }
    }
  }
  if (header_index == lines.size() || header.size() < 3 ||
      header.size() - 1 > kMaximumAxes) {
    if (error) *error = "polar header must be twa/tws followed by 2-200 wind speeds";
    return false;
  }
  PortablePolarGrid parsed;
  parsed.identity = path.stem().string();
  for (size_t column = 1; column < header.size(); ++column) {
    double value = 0.0;
    if (!Number(header[column], &value)) {
      if (error) *error = "polar header contains an invalid true-wind speed";
      return false;
    }
    parsed.true_wind_speeds_knots.push_back(value);
  }
  if (!StrictlyIncreasing(parsed.true_wind_speeds_knots, 0.0, 200.0)) {
    if (error) *error = "polar true-wind speeds must be strictly increasing in [0, 200] kt";
    return false;
  }
  std::vector<std::vector<double>> angle_major;
  for (size_t index = header_index + 1; index < lines.size(); ++index) {
    auto fields = Fields(lines[index], delimiter);
    if (fields.empty()) continue;
    if (fields.size() > header.size()) {
      if (error) *error = "polar row contains too many boat-speed values";
      return false;
    }
    fields.resize(header.size());
    double angle = 0.0;
    if (!Number(fields.front(), &angle)) {
      if (error) *error = "polar row contains an invalid true-wind angle";
      return false;
    }
    parsed.true_wind_angles_degrees.push_back(angle);
    std::vector<double> row;
    row.reserve(parsed.true_wind_speeds_knots.size());
    for (size_t column = 1; column < header.size(); ++column) {
      double speed = std::numeric_limits<double>::quiet_NaN();
      if (!Trim(fields[column]).empty() &&
          (!Number(fields[column], &speed) || speed < 0.0 || speed > 100.0)) {
        if (error) *error = "polar contains an invalid boat speed";
        return false;
      }
      row.push_back(speed);
    }
    angle_major.push_back(std::move(row));
  }
  if (parsed.true_wind_angles_degrees.size() > kMaximumAxes ||
      !StrictlyIncreasing(parsed.true_wind_angles_degrees, 0.0, 180.0)) {
    if (error) *error = "polar angles must be 2-200 strictly increasing values in [0, 180]";
    return false;
  }
  const size_t wind_count = parsed.true_wind_speeds_knots.size();
  const size_t angle_count = parsed.true_wind_angles_degrees.size();
  parsed.boat_speeds_knots.assign(wind_count * angle_count,
                                  std::numeric_limits<double>::quiet_NaN());
  for (size_t angle = 0; angle < angle_count; ++angle)
    for (size_t wind = 0; wind < wind_count; ++wind)
      parsed.boat_speeds_knots[wind * angle_count + angle] =
          angle_major[angle][wind];

  // Blank cells in established OpenCPN .pol files mean "interpolate". Fill
  // them deterministically from the nearest known cells in normalized
  // TWS/TWA space while retaining explicit numeric zeroes as real no-go data.
  for (size_t missing = 0; missing < parsed.boat_speeds_knots.size(); ++missing) {
    if (std::isfinite(parsed.boat_speeds_knots[missing])) continue;
    const size_t missing_wind = missing / angle_count;
    const size_t missing_angle = missing % angle_count;
    std::vector<std::pair<double, double>> nearest;
    for (size_t candidate = 0; candidate < parsed.boat_speeds_knots.size(); ++candidate) {
      const double speed = parsed.boat_speeds_knots[candidate];
      if (!std::isfinite(speed)) continue;
      const size_t wind = candidate / angle_count;
      const size_t angle = candidate % angle_count;
      const double wind_span = std::max(1.0, parsed.true_wind_speeds_knots.back() -
                                                parsed.true_wind_speeds_knots.front());
      const double angle_span = std::max(1.0, parsed.true_wind_angles_degrees.back() -
                                                 parsed.true_wind_angles_degrees.front());
      const double distance = std::hypot(
          (parsed.true_wind_speeds_knots[wind] -
           parsed.true_wind_speeds_knots[missing_wind]) /
              wind_span,
          (parsed.true_wind_angles_degrees[angle] -
           parsed.true_wind_angles_degrees[missing_angle]) /
              angle_span);
      nearest.emplace_back(distance, speed);
    }
    if (nearest.empty()) {
      if (error) *error = "polar has no numeric boat-speed cells";
      return false;
    }
    std::stable_sort(nearest.begin(), nearest.end());
    double weighted = 0.0, weights = 0.0;
    for (size_t i = 0; i < std::min<size_t>(4, nearest.size()); ++i) {
      const double weight = 1.0 / std::max(1e-12, nearest[i].first * nearest[i].first);
      weighted += nearest[i].second * weight;
      weights += weight;
    }
    parsed.boat_speeds_knots[missing] = weighted / weights;
  }
  *result = std::move(parsed);
  return true;
}

bool IsWithin(const std::filesystem::path& child,
              const std::filesystem::path& root) {
  auto child_part = child.begin();
  for (auto root_part = root.begin(); root_part != root.end();
       ++root_part, ++child_part)
    if (child_part == child.end() || *child_part != *root_part) return false;
  return true;
}

std::optional<std::filesystem::path> ResolvePolarReference(
    const std::filesystem::path& boat, const std::string& reference) {
  const auto boat_directory = std::filesystem::weakly_canonical(boat.parent_path());
  const auto polar_directory =
      std::filesystem::weakly_canonical(boat.parent_path().parent_path() / "polars");
  std::vector<std::filesystem::path> candidates;
  const std::filesystem::path referenced(reference);
  if (referenced.is_absolute())
    candidates.push_back(referenced);
  else {
    candidates.push_back(boat_directory / referenced);
    candidates.push_back(polar_directory / referenced);
  }
  for (const auto& candidate : candidates) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(candidate, ec);
    if (!ec && std::filesystem::is_regular_file(canonical, ec) &&
        (IsWithin(canonical, boat_directory) || IsWithin(canonical, polar_directory)))
      return canonical;
  }
  return std::nullopt;
}

bool ParseBoat(const std::filesystem::path& path, PortablePolarSet* result,
               std::string* error) {
  if (!InputIsBounded(path, error)) return false;
  pugi::xml_document document;
  const auto parsed = document.load_file(path.string().c_str(),
                                         pugi::parse_default & ~pugi::parse_doctype);
  if (!parsed) {
    if (error) *error = "could not parse OpenCPN boat XML: " + std::string(parsed.description());
    return false;
  }
  const auto root = document.child("OpenCPNWeatherRoutingBoat");
  if (!root) {
    if (error) *error = "boat XML has no OpenCPNWeatherRoutingBoat root";
    return false;
  }
  PortablePolarSet loaded;
  loaded.source_path = std::filesystem::weakly_canonical(path);
  size_t total_cells = 0;
  for (const auto polar : root.children("Polar")) {
    if (loaded.grids.size() >= kMaximumPolars) {
      if (error) *error = "boat XML exceeds the eight-polar limit";
      return false;
    }
    const std::string reference = polar.attribute("FileName").as_string();
    if (reference.empty()) {
      if (error) *error = "boat XML Polar element has no FileName";
      return false;
    }
    const auto resolved = ResolvePolarReference(path, reference);
    if (!resolved) {
      if (error) *error = "boat XML polar is missing or outside its granted directories: " + reference;
      return false;
    }
    const double crossover =
        polar.attribute("CrossOverPercentage").as_double(0.0);
    if (!std::isfinite(crossover) || crossover < 0.0 || crossover > 100.0) {
      if (error) *error = "boat XML CrossOverPercentage must be in [0, 100]";
      return false;
    }
    PortablePolarGrid grid;
    // CrossOverPercentage is an allowance used while selecting between sail
    // configurations; it is not a multiplier for the polar's boat speeds.
    // This first portable model selects the fastest applicable table and does
    // not import native crossover-contour state.
    if (!ParsePolar(*resolved, &grid, error)) return false;
    total_cells += grid.boat_speeds_knots.size();
    if (total_cells > kMaximumTotalCells) {
      if (error) *error = "boat XML polar tables exceed the cell limit";
      return false;
    }
    loaded.grids.push_back(std::move(grid));
  }
  if (loaded.grids.empty()) {
    if (error) *error = "boat XML contains no Polar entries";
    return false;
  }
  *result = std::move(loaded);
  return true;
}
}  // namespace

bool LoadPortablePolarSet(const std::filesystem::path& selected_path,
                          PortablePolarSet* result, std::string* error) {
  if (!result) {
    if (error) *error = "polar output is required";
    return false;
  }
  std::error_code ec;
  const auto path = std::filesystem::weakly_canonical(selected_path, ec);
  if (ec || !std::filesystem::is_regular_file(path, ec)) {
    if (error) *error = "vessel-performance file does not exist";
    return false;
  }
  const std::string extension = Lower(path.extension().string());
  if (extension == ".xml") return ParseBoat(path, result, error);
  if (extension != ".pol") {
    if (error) *error = "select an OpenCPN boat .xml or polar .pol file";
    return false;
  }
  PortablePolarGrid grid;
  if (!ParsePolar(path, &grid, error)) return false;
  result->source_path = path;
  result->grids = {std::move(grid)};
  return true;
}
