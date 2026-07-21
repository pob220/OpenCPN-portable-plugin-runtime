/***************************************************************************
 * Version negotiation for typed portable-plugin services.
 *
 * This implements the deterministic SemVer subset used by portable service
 * manifests. Interface versions are release triples and requirements are
 * whitespace-separated comparison clauses such as ">=0.1.0 <0.2.0".
 ***************************************************************************/

#ifndef OCPN_PORTABLE_SERVICE_VERSION_H
#define OCPN_PORTABLE_SERVICE_VERSION_H

#include <cctype>
#include <limits>
#include <sstream>
#include <string>

namespace ocpn::portable {

struct ServiceVersion {
  unsigned major = 0;
  unsigned minor = 0;
  unsigned patch = 0;

  friend bool operator==(const ServiceVersion& left,
                         const ServiceVersion& right) {
    return left.major == right.major && left.minor == right.minor &&
           left.patch == right.patch;
  }
  friend bool operator<(const ServiceVersion& left,
                        const ServiceVersion& right) {
    if (left.major != right.major) return left.major < right.major;
    if (left.minor != right.minor) return left.minor < right.minor;
    return left.patch < right.patch;
  }
};

inline bool ParseServiceVersion(const std::string& text,
                                ServiceVersion* output) {
  if (!output || text.empty()) return false;
  ServiceVersion parsed;
  unsigned* parts[] = {&parsed.major, &parsed.minor, &parsed.patch};
  size_t position = 0;
  for (size_t part = 0; part < 3; ++part) {
    if (position >= text.size() ||
        !std::isdigit(static_cast<unsigned char>(text[position])))
      return false;
    if (text[position] == '0' && position + 1 < text.size() &&
        std::isdigit(static_cast<unsigned char>(text[position + 1])))
      return false;
    unsigned value = 0;
    while (position < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[position]))) {
      const unsigned digit = static_cast<unsigned>(text[position] - '0');
      if (value > (std::numeric_limits<unsigned>::max() - digit) / 10)
        return false;
      value = value * 10 + digit;
      ++position;
    }
    *parts[part] = value;
    if (part < 2) {
      if (position >= text.size() || text[position] != '.') return false;
      ++position;
    }
  }
  if (position != text.size()) return false;
  *output = parsed;
  return true;
}

inline bool IsServiceVersionRange(const std::string& range_text) {
  if (range_text.empty()) return false;
  std::istringstream input(range_text);
  std::string clause;
  bool saw_clause = false;
  while (input >> clause) {
    saw_clause = true;
    size_t prefix = 0;
    if (clause.rfind(">=", 0) == 0 || clause.rfind("<=", 0) == 0)
      prefix = 2;
    else if (!clause.empty() &&
             (clause[0] == '>' || clause[0] == '<' || clause[0] == '='))
      prefix = 1;
    ServiceVersion boundary;
    if (!ParseServiceVersion(clause.substr(prefix), &boundary)) return false;
  }
  return saw_clause;
}

inline bool ServiceVersionSatisfies(const std::string& version_text,
                                    const std::string& range_text) {
  ServiceVersion version;
  if (!ParseServiceVersion(version_text, &version) || range_text.empty())
    return false;
  std::istringstream input(range_text);
  std::string clause;
  bool saw_clause = false;
  while (input >> clause) {
    saw_clause = true;
    std::string operation;
    size_t prefix = 0;
    if (clause.rfind(">=", 0) == 0 || clause.rfind("<=", 0) == 0) {
      operation = clause.substr(0, 2);
      prefix = 2;
    } else if (!clause.empty() &&
               (clause[0] == '>' || clause[0] == '<' || clause[0] == '=')) {
      operation = clause.substr(0, 1);
      prefix = 1;
    } else {
      operation = "=";
    }
    ServiceVersion boundary;
    if (!ParseServiceVersion(clause.substr(prefix), &boundary)) return false;
    const bool equal = version == boundary;
    const bool less = version < boundary;
    const bool matches = operation == "="    ? equal
                         : operation == ">"  ? (!less && !equal)
                         : operation == ">=" ? !less
                         : operation == "<"  ? less
                         : operation == "<=" ? (less || equal)
                                             : false;
    if (!matches) return false;
  }
  return saw_clause;
}

}  // namespace ocpn::portable

#endif  // OCPN_PORTABLE_SERVICE_VERSION_H
