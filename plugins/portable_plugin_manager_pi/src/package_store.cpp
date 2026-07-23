#include "package_store.h"

#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <wx/base64.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>

namespace ppm {
namespace {

namespace fs = std::filesystem;

constexpr std::uintmax_t kMaxArchiveBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uintmax_t kMaxExpandedBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxEntries = 32768;
constexpr std::size_t kMaxManifestBytes = 1024ULL * 1024ULL;
constexpr std::uintmax_t kMaxExpansionRatio = 200;

struct EntryInfo {
  std::uintmax_t size = 0;
  bool directory = false;
  bool executable = false;
};

struct ValidatedArchive {
  wxJSONValue manifest;
  std::string manifest_bytes;
  std::string checksum_bytes;
  std::string signature_bytes;
  std::map<std::string, std::string> checksums;
  std::map<std::string, EntryInfo> entries;
  std::string id;
  std::string name;
  std::string version;
  bool development = false;
};

StoreResult Failure(std::string code, std::string message) {
  return {false, std::move(code), std::move(message), {}, {}, {}};
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

bool IsSafeIdentifier(const std::string& value, bool require_dot) {
  if (value.empty() || value.size() > 128 ||
      (require_dot && value.find('.') == std::string::npos)) {
    return false;
  }
  bool label_start = true;
  char previous = '\0';
  for (const unsigned char character : value) {
    if (character == '.') {
      if (label_start || previous == '-') return false;
      label_start = true;
    } else if ((character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9')) {
      label_start = false;
    } else if (character == '-' && !label_start) {
      label_start = false;
    } else {
      return false;
    }
    previous = static_cast<char>(character);
  }
  return !label_start && previous != '-';
}

bool IsSemanticVersion(const std::string& value) {
  if (value.empty() || value.size() > 128) return false;
  std::size_t cursor = 0;
  for (int field = 0; field < 3; ++field) {
    const std::size_t start = cursor;
    while (cursor < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[cursor]))) {
      ++cursor;
    }
    if (cursor == start || (cursor - start > 1 && value[start] == '0'))
      return false;
    if (field < 2) {
      if (cursor >= value.size() || value[cursor++] != '.') return false;
    }
  }
  if (cursor == value.size()) return true;
  bool metadata = false;
  if (value[cursor] == '-') {
    const std::size_t suffix_start = ++cursor;
    bool identifier_start = true;
    for (; cursor < value.size() && value[cursor] != '+'; ++cursor) {
      const unsigned char character = value[cursor];
      if (character == '.') {
        if (identifier_start) return false;
        identifier_start = true;
      } else if (std::isalnum(character) || character == '-') {
        identifier_start = false;
      } else {
        return false;
      }
    }
    if (cursor == suffix_start || identifier_start) return false;
    metadata = cursor < value.size();
  } else if (value[cursor] == '+') {
    metadata = true;
  } else {
    return false;
  }
  if (metadata) {
    if (cursor >= value.size() || value[cursor++] != '+') return false;
    bool identifier_start = true;
    const std::size_t metadata_start = cursor;
    for (; cursor < value.size(); ++cursor) {
      const unsigned char character = value[cursor];
      if (character == '.') {
        if (identifier_start) return false;
        identifier_start = true;
      } else if (std::isalnum(character) || character == '-') {
        identifier_start = false;
      } else {
        return false;
      }
    }
    if (cursor == metadata_start || identifier_start) return false;
  }
  return cursor == value.size();
}

bool IsWindowsDeviceName(const std::string& component) {
  const std::string upper = [&]() {
    std::string result = component.substr(0, component.find('.'));
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                     return static_cast<char>(std::toupper(character));
                   });
    return result;
  }();
  if (upper == "CON" || upper == "PRN" || upper == "AUX" ||
      upper == "NUL") {
    return true;
  }
  if (upper.size() == 4 &&
      (upper.rfind("COM", 0) == 0 || upper.rfind("LPT", 0) == 0) &&
      upper[3] >= '1' && upper[3] <= '9') {
    return true;
  }
  return false;
}

bool CheckedArchivePath(const char* raw, std::string* result,
                        std::string* diagnostic) {
  if (!raw || !*raw) {
    *diagnostic = "empty archive path";
    return false;
  }
  std::string value(raw);
  if (value.front() == '/' || value.find('\\') != std::string::npos ||
      value.find(':') != std::string::npos ||
      value.find('\0') != std::string::npos) {
    *diagnostic = "absolute or platform-ambiguous archive path";
    return false;
  }
  while (!value.empty() && value.back() == '/') value.pop_back();
  if (value.empty()) {
    *diagnostic = "empty archive path";
    return false;
  }
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t separator = value.find('/', start);
    const std::string component =
        value.substr(start, separator == std::string::npos
                                ? std::string::npos
                                : separator - start);
    if (component.empty() || component == "." || component == ".." ||
        component.back() == ' ' || component.back() == '.' ||
        IsWindowsDeviceName(component)) {
      *diagnostic = "unsafe archive path component";
      return false;
    }
    for (const unsigned char character : component) {
      // Format v1 uses portable ASCII entry names. This deliberately rejects
      // Unicode canonicalisation ambiguities while allowing archives to be
      // located in Unicode or whitespace-containing host paths.
      if (character < 0x20 || character > 0x7e) {
        *diagnostic = "non-portable archive path encoding";
        return false;
      }
    }
    if (separator == std::string::npos) break;
    start = separator + 1;
  }
  *result = value;
  return true;
}

class JsonShapeValidator {
 public:
  explicit JsonShapeValidator(const std::string& input) : input_(input) {}

  bool Validate(std::string* diagnostic) {
    SkipSpace();
    if (!Value(diagnostic, 0)) return false;
    SkipSpace();
    if (cursor_ != input_.size()) {
      *diagnostic = "trailing JSON data";
      return false;
    }
    return true;
  }

 private:
  bool Number() {
    const std::size_t start = cursor_;
    if (cursor_ < input_.size() && input_[cursor_] == '-') ++cursor_;
    if (cursor_ >= input_.size()) return false;
    if (input_[cursor_] == '0') {
      ++cursor_;
      if (cursor_ < input_.size() &&
          std::isdigit(static_cast<unsigned char>(input_[cursor_]))) {
        return false;
      }
    } else if (input_[cursor_] >= '1' && input_[cursor_] <= '9') {
      while (cursor_ < input_.size() &&
             std::isdigit(static_cast<unsigned char>(input_[cursor_]))) {
        ++cursor_;
      }
    } else {
      return false;
    }
    if (cursor_ < input_.size() && input_[cursor_] == '.') {
      ++cursor_;
      const std::size_t fraction = cursor_;
      while (cursor_ < input_.size() &&
             std::isdigit(static_cast<unsigned char>(input_[cursor_]))) {
        ++cursor_;
      }
      if (cursor_ == fraction) return false;
    }
    if (cursor_ < input_.size() &&
        (input_[cursor_] == 'e' || input_[cursor_] == 'E')) {
      ++cursor_;
      if (cursor_ < input_.size() &&
          (input_[cursor_] == '+' || input_[cursor_] == '-')) {
        ++cursor_;
      }
      const std::size_t exponent = cursor_;
      while (cursor_ < input_.size() &&
             std::isdigit(static_cast<unsigned char>(input_[cursor_]))) {
        ++cursor_;
      }
      if (cursor_ == exponent) return false;
    }
    return cursor_ > start;
  }

  void SkipSpace() {
    while (cursor_ < input_.size() &&
           (input_[cursor_] == ' ' || input_[cursor_] == '\t' ||
            input_[cursor_] == '\r' || input_[cursor_] == '\n')) {
      ++cursor_;
    }
  }

  bool String(std::string* output, bool object_key,
              std::string* diagnostic) {
    if (cursor_ >= input_.size() || input_[cursor_++] != '"') return false;
    output->clear();
    while (cursor_ < input_.size()) {
      const unsigned char character = input_[cursor_++];
      if (character == '"') return true;
      if (character < 0x20) {
        *diagnostic = "control character in JSON string";
        return false;
      }
      if (character == '\\') {
        if (object_key) {
          *diagnostic = "escaped JSON object key is not permitted";
          return false;
        }
        if (cursor_ >= input_.size()) return false;
        const char escaped = input_[cursor_++];
        if (escaped == 'u') {
          if (cursor_ + 4 > input_.size()) return false;
          for (int count = 0; count < 4; ++count) {
            if (!std::isxdigit(
                    static_cast<unsigned char>(input_[cursor_++]))) {
              return false;
            }
          }
        } else if (std::string("\"\\/bfnrt").find(escaped) ==
                   std::string::npos) {
          return false;
        }
      } else {
        output->push_back(static_cast<char>(character));
      }
    }
    *diagnostic = "unterminated JSON string";
    return false;
  }

  bool Value(std::string* diagnostic, unsigned depth) {
    if (depth > 64) {
      *diagnostic = "JSON nesting limit exceeded";
      return false;
    }
    SkipSpace();
    if (cursor_ >= input_.size()) return false;
    if (input_[cursor_] == '{') return Object(diagnostic, depth + 1);
    if (input_[cursor_] == '[') return Array(diagnostic, depth + 1);
    if (input_[cursor_] == '"') {
      std::string ignored;
      return String(&ignored, false, diagnostic);
    }
    for (const std::string literal : {"true", "false", "null"}) {
      if (input_.compare(cursor_, literal.size(), literal) == 0) {
        cursor_ += literal.size();
        return true;
      }
    }
    return Number();
  }

  bool Object(std::string* diagnostic, unsigned depth) {
    ++cursor_;
    SkipSpace();
    if (cursor_ < input_.size() && input_[cursor_] == '}') {
      ++cursor_;
      return true;
    }
    std::set<std::string> keys;
    while (cursor_ < input_.size()) {
      std::string key;
      if (!String(&key, true, diagnostic)) return false;
      if (!keys.insert(key).second) {
        *diagnostic = "duplicate JSON object key: " + key;
        return false;
      }
      SkipSpace();
      if (cursor_ >= input_.size() || input_[cursor_++] != ':') return false;
      if (!Value(diagnostic, depth)) return false;
      SkipSpace();
      if (cursor_ >= input_.size()) return false;
      if (input_[cursor_] == '}') {
        ++cursor_;
        return true;
      }
      if (input_[cursor_++] != ',') return false;
      SkipSpace();
    }
    return false;
  }

  bool Array(std::string* diagnostic, unsigned depth) {
    ++cursor_;
    SkipSpace();
    if (cursor_ < input_.size() && input_[cursor_] == ']') {
      ++cursor_;
      return true;
    }
    while (cursor_ < input_.size()) {
      if (!Value(diagnostic, depth)) return false;
      SkipSpace();
      if (cursor_ >= input_.size()) return false;
      if (input_[cursor_] == ']') {
        ++cursor_;
        return true;
      }
      if (input_[cursor_++] != ',') return false;
    }
    return false;
  }

  const std::string& input_;
  std::size_t cursor_ = 0;
};

struct ArchiveCloser {
  void operator()(archive* value) const {
    if (value) archive_read_free(value);
  }
};

struct FileCloser {
  void operator()(FILE* value) const {
    if (value) std::fclose(value);
  }
};

using ArchiveHandle = std::unique_ptr<archive, ArchiveCloser>;

ArchiveHandle OpenArchive(const fs::path& path, std::string* diagnostic) {
  ArchiveHandle input(archive_read_new());
  if (!input) {
    *diagnostic = "could not allocate archive reader";
    return {};
  }
  archive_read_support_filter_none(input.get());
  archive_read_support_filter_gzip(input.get());
  archive_read_support_format_zip(input.get());
  if (archive_read_open_filename(input.get(), path.c_str(), 1024 * 1024) !=
      ARCHIVE_OK) {
    *diagnostic = archive_error_string(input.get());
    return {};
  }
  return input;
}

bool ReadArchiveEntry(archive* input, std::size_t limit, std::string* output,
                      std::string* diagnostic) {
  output->clear();
  std::array<char, 64 * 1024> buffer{};
  for (;;) {
    const la_ssize_t count =
        archive_read_data(input, buffer.data(), buffer.size());
    if (count == 0) return true;
    if (count < 0) {
      *diagnostic = archive_error_string(input);
      return false;
    }
    if (output->size() + static_cast<std::size_t>(count) > limit) {
      *diagnostic = "archive metadata entry exceeds its limit";
      return false;
    }
    output->append(buffer.data(), static_cast<std::size_t>(count));
  }
}

bool ParseChecksums(const std::string& document,
                    std::map<std::string, std::string>* checksums,
                    std::string* diagnostic) {
  std::istringstream lines(document);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() < 67 || line[64] != ' ' || line[65] != ' ') {
      *diagnostic = "malformed checksums.sha256";
      return false;
    }
    const std::string digest = line.substr(0, 64);
    if (!std::all_of(digest.begin(), digest.end(), [](unsigned char value) {
          return std::isdigit(value) || (value >= 'a' && value <= 'f');
        })) {
      *diagnostic = "malformed SHA-256 digest";
      return false;
    }
    std::string name;
    if (!CheckedArchivePath(line.c_str() + 66, &name, diagnostic) ||
        !checksums->emplace(name, digest).second) {
      if (diagnostic->empty()) *diagnostic = "duplicate checksum entry";
      return false;
    }
  }
  return !checksums->empty();
}

bool ParseJson(const std::string& bytes, wxJSONValue* document,
               std::string* diagnostic) {
  JsonShapeValidator shape(bytes);
  if (!shape.Validate(diagnostic)) return false;
  wxJSONReader reader;
  if (reader.Parse(wxString::FromUTF8(bytes), document) != 0 ||
      !document->IsObject()) {
    *diagnostic = "malformed UTF-8 JSON object";
    return false;
  }
  return true;
}

bool VerifySignature(const std::string& signature_bytes,
                     const std::string& checksums,
                     const fs::path& trust_root, bool development,
                     bool developer_mode, std::string* diagnostic) {
  if (signature_bytes.empty()) {
    if (development && developer_mode) return true;
    *diagnostic = "package is unsigned";
    return false;
  }
  wxJSONValue signature;
  if (!ParseJson(signature_bytes, &signature, diagnostic)) return false;
  if (!signature["algorithm"].IsString() ||
      signature["algorithm"].AsString() != "Ed25519" ||
      !signature["signed"].IsString() ||
      signature["signed"].AsString() != "checksums.sha256" ||
      !signature["key_id"].IsString() ||
      !signature["signature"].IsString()) {
    *diagnostic = "unsupported signature document";
    return false;
  }
  const std::string key_id = signature["key_id"].AsString().ToStdString();
  if (!IsSafeIdentifier(key_id, true)) {
    *diagnostic = "invalid signing key identifier";
    return false;
  }
  if (key_id.rfind("org.opencpn.development.", 0) == 0 &&
      !developer_mode) {
    *diagnostic = "development signing keys are disabled";
    return false;
  }
  const fs::path public_key_path = trust_root / (key_id + ".pem");
  std::unique_ptr<FILE, FileCloser> key_file(
      std::fopen(public_key_path.c_str(), "rb"));
  if (!key_file) {
    *diagnostic = "untrusted signing key: " + key_id;
    return false;
  }
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> public_key(
      PEM_read_PUBKEY(key_file.get(), nullptr, nullptr, nullptr),
      &EVP_PKEY_free);
  if (!public_key || EVP_PKEY_base_id(public_key.get()) != EVP_PKEY_ED25519) {
    *diagnostic = "trusted key is not Ed25519";
    return false;
  }
  const wxMemoryBuffer decoded =
      wxBase64Decode(signature["signature"].AsString(),
                     wxBase64DecodeMode_Strict);
  if (decoded.GetDataLen() != 64) {
    *diagnostic = "invalid Ed25519 signature encoding";
    return false;
  }
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
      EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!context ||
      EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr,
                           public_key.get()) != 1 ||
      EVP_DigestVerify(
          context.get(),
          static_cast<const unsigned char*>(decoded.GetData()),
          decoded.GetDataLen(),
          reinterpret_cast<const unsigned char*>(checksums.data()),
          checksums.size()) != 1) {
    *diagnostic = "Ed25519 signature verification failed";
    return false;
  }
  return true;
}

bool ValidateManifestFields(ValidatedArchive* validated,
                            std::string* diagnostic) {
  wxJSONValue& manifest = validated->manifest;
  if (!manifest["format_version"].IsInt() ||
      manifest["format_version"].AsInt() != 1 ||
      !manifest["id"].IsString() || !manifest["name"].IsString() ||
      !manifest["version"].IsString() ||
      !manifest["component"].IsString() ||
      !manifest["runtime"].IsString() ||
      !manifest["portable_api"].IsString() ||
      !manifest["permissions"].IsArray() ||
      !manifest["development"].IsBool()) {
    *diagnostic = "manifest has missing or mistyped required fields";
    return false;
  }
  validated->id = manifest["id"].AsString().ToStdString();
  validated->name = manifest["name"].AsString().ToStdString();
  validated->version = manifest["version"].AsString().ToStdString();
  validated->development = manifest["development"].AsBool();
  std::string component;
  if (!IsSafeIdentifier(validated->id, true) ||
      !IsSemanticVersion(validated->version) || validated->name.empty() ||
      validated->name.size() > 256 ||
      manifest["runtime"].AsString() != ">=0.1.0 <0.2.0" ||
      manifest["portable_api"].AsString() != ">=0.1.0 <0.2.0" ||
      !CheckedArchivePath(manifest["component"].AsString().utf8_str(),
                          &component, diagnostic) ||
      validated->entries.count(component) == 0 ||
      validated->entries.at(component).directory) {
    if (diagnostic->empty()) *diagnostic = "invalid manifest identity";
    return false;
  }
  wxJSONValue permissions = manifest["permissions"];
  std::set<std::string> unique_permissions;
  for (int index = 0; index < permissions.Size(); ++index) {
    if (!permissions[index].IsString()) {
      *diagnostic = "manifest permission is not a string";
      return false;
    }
    const std::string permission =
        permissions[index].AsString().ToStdString();
    if (permission.empty() || !unique_permissions.insert(permission).second) {
      *diagnostic = "manifest permission is empty or duplicated";
      return false;
    }
  }
  return true;
}

bool ValidateFileSet(const ValidatedArchive& validated,
                     std::string* diagnostic) {
  std::set<std::string> actual;
  for (const auto& item : validated.entries) {
    if (!item.second.directory && item.first != "checksums.sha256" &&
        item.first != "signature.json") {
      actual.insert(item.first);
    }
  }
  std::set<std::string> expected;
  for (const auto& item : validated.checksums) expected.insert(item.first);
  if (actual != expected) {
    *diagnostic = "checksum file list differs from package contents";
    return false;
  }
  return true;
}

bool ValidateArchive(const fs::path& path, const fs::path& trust_root,
                     bool developer_mode, ValidatedArchive* validated,
                     std::string* diagnostic) {
  std::error_code error;
  const std::uintmax_t archive_size = fs::file_size(path, error);
  if (error || archive_size == 0 || archive_size > kMaxArchiveBytes) {
    *diagnostic = "archive size is outside policy";
    return false;
  }
  auto input = OpenArchive(path, diagnostic);
  if (!input) return false;
  std::set<std::string> folded_names;
  std::uintmax_t expanded = 0;
  archive_entry* raw_entry = nullptr;
  int header_result = ARCHIVE_OK;
  while ((header_result =
              archive_read_next_header(input.get(), &raw_entry)) ==
         ARCHIVE_OK) {
    if (validated->entries.size() >= kMaxEntries) {
      *diagnostic = "too many archive entries";
      return false;
    }
    if (archive_entry_is_encrypted(raw_entry) == 1) {
      *diagnostic = "encrypted archive entries are not permitted";
      return false;
    }
    std::string name;
    if (!CheckedArchivePath(archive_entry_pathname(raw_entry), &name,
                            diagnostic)) {
      return false;
    }
    if (!folded_names.insert(LowerAscii(name)).second) {
      *diagnostic = "duplicate or case-colliding archive path";
      return false;
    }
    const mode_t type = archive_entry_filetype(raw_entry);
    const bool directory = type == AE_IFDIR;
    if (!directory && type != AE_IFREG && type != 0) {
      *diagnostic = "non-regular archive entry";
      return false;
    }
    const la_int64_t signed_size = archive_entry_size(raw_entry);
    if (signed_size < 0) {
      *diagnostic = "archive entry has unknown size";
      return false;
    }
    const auto size = static_cast<std::uintmax_t>(signed_size);
    if (size > kMaxExpandedBytes - std::min(expanded, kMaxExpandedBytes)) {
      *diagnostic = "expanded package exceeds policy";
      return false;
    }
    expanded += size;
    const bool executable = (archive_entry_perm(raw_entry) & 0111) != 0;
    if (executable && name.rfind("helpers/", 0) != 0) {
      *diagnostic = "executable entry outside helpers";
      return false;
    }
    validated->entries.emplace(name,
                               EntryInfo{size, directory, executable});
    if (name == "manifest.json") {
      if (!ReadArchiveEntry(input.get(), kMaxManifestBytes,
                            &validated->manifest_bytes, diagnostic)) {
        return false;
      }
    } else if (name == "checksums.sha256") {
      if (!ReadArchiveEntry(input.get(), kMaxManifestBytes,
                            &validated->checksum_bytes, diagnostic)) {
        return false;
      }
    } else if (name == "signature.json") {
      if (!ReadArchiveEntry(input.get(), kMaxManifestBytes,
                            &validated->signature_bytes, diagnostic)) {
        return false;
      }
    } else {
      archive_read_data_skip(input.get());
    }
  }
  if (header_result != ARCHIVE_EOF) {
    *diagnostic = archive_error_string(input.get());
    return false;
  }
  if (expanded > archive_size * kMaxExpansionRatio) {
    *diagnostic = "archive expansion ratio exceeds policy";
    return false;
  }
  if (validated->manifest_bytes.empty() ||
      validated->checksum_bytes.empty()) {
    *diagnostic = "required package metadata is missing";
    return false;
  }
  if (!ParseJson(validated->manifest_bytes, &validated->manifest,
                 diagnostic) ||
      !ParseChecksums(validated->checksum_bytes, &validated->checksums,
                      diagnostic)) {
    return false;
  }
  if (!ValidateManifestFields(validated, diagnostic)) return false;
  if (validated->development && !developer_mode) {
    *diagnostic = "development packages are disabled";
    return false;
  }
  if (!ValidateFileSet(*validated, diagnostic)) return false;
  return VerifySignature(validated->signature_bytes,
                         validated->checksum_bytes,
                         trust_root, validated->development, developer_mode,
                         diagnostic);
}

std::string HexDigest(const unsigned char* digest, std::size_t length) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < length; ++index)
    output << std::setw(2) << static_cast<unsigned>(digest[index]);
  return output.str();
}

fs::path UniquePath(const fs::path& parent, const std::string& prefix) {
  std::random_device random;
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    const auto ticks =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path candidate =
        parent / (prefix + std::to_string(ticks) + "-" +
                  std::to_string(random()));
    if (!fs::exists(candidate)) return candidate;
  }
  return {};
}

bool ExtractValidated(const fs::path& archive_path,
                      const ValidatedArchive& validated,
                      const fs::path& staging, std::string* diagnostic) {
  auto input = OpenArchive(archive_path, diagnostic);
  if (!input) return false;
  archive_entry* raw_entry = nullptr;
  std::array<unsigned char, 1024 * 1024> buffer{};
  int header_result = ARCHIVE_OK;
  while ((header_result =
              archive_read_next_header(input.get(), &raw_entry)) ==
         ARCHIVE_OK) {
    std::string name;
    if (!CheckedArchivePath(archive_entry_pathname(raw_entry), &name,
                            diagnostic)) {
      return false;
    }
    if (name == "checksums.sha256" || name == "signature.json") {
      archive_read_data_skip(input.get());
      continue;
    }
    const auto metadata = validated.entries.find(name);
    if (metadata == validated.entries.end()) {
      *diagnostic = "archive changed during installation";
      return false;
    }
    const fs::path output_path = staging / fs::path(name);
    std::error_code error;
    if (metadata->second.directory) {
      fs::create_directories(output_path, error);
      if (error) {
        *diagnostic = error.message();
        return false;
      }
      continue;
    }
    fs::create_directories(output_path.parent_path(), error);
    if (error) {
      *diagnostic = error.message();
      return false;
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      *diagnostic = "could not create staged package file";
      return false;
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(
        EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1)
      return false;
    for (;;) {
      const la_ssize_t count =
          archive_read_data(input.get(), buffer.data(), buffer.size());
      if (count == 0) break;
      if (count < 0) {
        *diagnostic = archive_error_string(input.get());
        return false;
      }
      output.write(reinterpret_cast<const char*>(buffer.data()), count);
      if (!output ||
          EVP_DigestUpdate(hash.get(), buffer.data(),
                           static_cast<std::size_t>(count)) != 1) {
        *diagnostic = "failed while writing staged package";
        return false;
      }
    }
    output.close();
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned digest_length = 0;
    if (EVP_DigestFinal_ex(hash.get(), digest.data(), &digest_length) != 1 ||
        HexDigest(digest.data(), digest_length) !=
            validated.checksums.at(name)) {
      *diagnostic = "digest mismatch: " + name;
      return false;
    }
    fs::permissions(
        output_path,
        metadata->second.executable
            ? fs::perms::owner_read | fs::perms::owner_exec |
                  fs::perms::group_read | fs::perms::group_exec |
                  fs::perms::others_read | fs::perms::others_exec
            : fs::perms::owner_read | fs::perms::group_read |
                  fs::perms::others_read,
        fs::perm_options::replace, error);
    if (error) {
      *diagnostic = error.message();
      return false;
    }
  }
  if (header_result != ARCHIVE_EOF) {
    *diagnostic = archive_error_string(input.get());
    return false;
  }
  const auto write_metadata = [&](const fs::path& path,
                                  const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    if (!output) return false;
    output.close();
    std::error_code error;
    fs::permissions(path,
                    fs::perms::owner_read | fs::perms::group_read |
                        fs::perms::others_read,
                    fs::perm_options::replace, error);
    return !error;
  };
  if (!write_metadata(staging / "checksums.sha256",
                      validated.checksum_bytes) ||
      (!validated.signature_bytes.empty() &&
       !write_metadata(staging / "signature.json",
                       validated.signature_bytes))) {
    *diagnostic = "could not retain package verification metadata";
    return false;
  }
  return true;
}

std::string RecoveryStamp(const std::string& version) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::to_string(
             std::chrono::duration_cast<std::chrono::milliseconds>(now)
                 .count()) +
         "-" + version;
}

bool ReadManifestIdentity(const fs::path& root, StoredPackage* package) {
  std::ifstream input(root / "manifest.json", std::ios::binary);
  if (!input) return false;
  const std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  std::string diagnostic;
  wxJSONValue manifest;
  if (bytes.size() > kMaxManifestBytes ||
      !ParseJson(bytes, &manifest, &diagnostic) ||
      !manifest["id"].IsString() || !manifest["name"].IsString() ||
      !manifest["version"].IsString() ||
      !manifest["development"].IsBool() ||
      !manifest["permissions"].IsArray()) {
    return false;
  }
  package->id = manifest["id"].AsString().ToStdString();
  package->name = manifest["name"].AsString().ToStdString();
  package->version = manifest["version"].AsString().ToStdString();
  package->development = manifest["development"].AsBool();
  wxJSONValue permissions = manifest["permissions"];
  std::set<std::string> unique_permissions;
  for (int index = 0; index < permissions.Size(); ++index) {
    if (!permissions[index].IsString()) return false;
    const std::string permission =
        permissions[index].AsString().ToStdString();
    if (permission.empty() || !unique_permissions.insert(permission).second)
      return false;
    package->permissions.push_back(permission);
  }
  std::sort(package->permissions.begin(), package->permissions.end());
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(
      EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned digest_length = 0;
  if (!hash ||
      EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(hash.get(), bytes.data(), bytes.size()) != 1 ||
      EVP_DigestFinal_ex(hash.get(), digest.data(), &digest_length) != 1) {
    return false;
  }
  package->manifest_digest = HexDigest(digest.data(), digest_length);
  package->root = root;
  return IsSafeIdentifier(package->id, true) &&
         IsSemanticVersion(package->version);
}

bool ReadEnabled(const fs::path& root, const std::string& id) {
  std::ifstream input(root / "state" / (id + ".enabled"));
  char value = '\0';
  return input.get(value) && value == '1';
}

bool ReadDiskFile(const fs::path& path, std::size_t limit,
                  std::string* contents, std::string* diagnostic) {
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error || size > limit) {
    *diagnostic = "installed metadata is missing or exceeds policy";
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *diagnostic = "could not read installed metadata";
    return false;
  }
  contents->assign(std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>());
  if (input.bad()) {
    *diagnostic = "could not read installed metadata";
    return false;
  }
  return true;
}

bool HashDiskFile(const fs::path& path, std::uintmax_t size_limit,
                  std::string* digest, std::string* diagnostic) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *diagnostic = "could not read installed package file";
    return false;
  }
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(
      EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1) {
    *diagnostic = "could not initialize installed-file digest";
    return false;
  }
  std::array<unsigned char, 1024 * 1024> buffer{};
  std::uintmax_t total = 0;
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
    const auto count = input.gcount();
    if (count <= 0) break;
    if (static_cast<std::uintmax_t>(count) > size_limit - total ||
        EVP_DigestUpdate(hash.get(), buffer.data(),
                         static_cast<std::size_t>(count)) != 1) {
      *diagnostic = "installed package file exceeds policy";
      return false;
    }
    total += static_cast<std::uintmax_t>(count);
  }
  if (!input.eof()) {
    *diagnostic = "could not read installed package file";
    return false;
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
  unsigned length = 0;
  if (EVP_DigestFinal_ex(hash.get(), bytes.data(), &length) != 1) {
    *diagnostic = "could not finalize installed-file digest";
    return false;
  }
  *digest = HexDigest(bytes.data(), length);
  return true;
}

}  // namespace

PackageStore::PackageStore(fs::path root, fs::path trust_root)
    : root_(std::move(root)),
      trust_root_(trust_root.empty() ? root_ / "trust"
                                     : std::move(trust_root)) {}

StoreResult PackageStore::Inspect(const fs::path& archive_path) const {
  ValidatedArchive validated;
  std::string diagnostic;
  if (!ValidateArchive(archive_path, TrustRoot(), developer_mode_, &validated,
                       &diagnostic)) {
    return Failure("validation-failed", diagnostic);
  }
  return {true,
          "verified",
          "Package signature, manifest and archive structure verified",
          validated.id,
          PackagesRoot() / validated.id,
          {}};
}

StoreResult PackageStore::AuditInstalled(
    const std::string& package_id) const {
  if (!IsSafeIdentifier(package_id, true))
    return Failure("invalid-id", "invalid package identifier");
  const fs::path package_root = PackagesRoot() / package_id;
  std::error_code error;
  if (!fs::is_directory(package_root, error))
    return Failure("not-installed", package_id + " is not installed");

  ValidatedArchive validated;
  std::set<std::string> folded_names;
  std::uintmax_t expanded = 0;
  std::size_t entry_count = 0;
  for (fs::recursive_directory_iterator item(package_root, error), end;
       !error && item != end; item.increment(error)) {
    if (++entry_count > kMaxEntries)
      return Failure("integrity-failed", "installed package has too many entries");
    const fs::file_status status = item->symlink_status(error);
    if (error) break;
    if (!fs::is_directory(status) && !fs::is_regular_file(status))
      return Failure("integrity-failed",
                     "installed package contains a non-regular entry");
    const fs::path relative = fs::relative(item->path(), package_root, error);
    if (error) break;
    std::string name;
    std::string diagnostic;
    const std::string generic = relative.generic_string();
    if (!CheckedArchivePath(generic.c_str(), &name, &diagnostic))
      return Failure("integrity-failed", diagnostic);
    if (!folded_names.insert(LowerAscii(name)).second)
      return Failure("integrity-failed",
                     "installed package contains case-colliding paths");
    const bool directory = fs::is_directory(status);
    const auto permissions = status.permissions();
    const bool executable =
        (permissions & (fs::perms::owner_exec | fs::perms::group_exec |
                        fs::perms::others_exec)) != fs::perms::none;
    if (executable && !directory && name.rfind("helpers/", 0) != 0)
      return Failure("integrity-failed",
                     "installed executable is outside helpers");
    std::uintmax_t size = 0;
    if (!directory) {
      size = item->file_size(error);
      if (error || size > kMaxExpandedBytes - expanded)
        return Failure("integrity-failed",
                       "installed package exceeds size policy");
      expanded += size;
    }
    validated.entries.emplace(name, EntryInfo{size, directory, executable});
  }
  if (error) return Failure("integrity-failed", error.message());

  std::string diagnostic;
  if (!ReadDiskFile(package_root / "manifest.json", kMaxManifestBytes,
                    &validated.manifest_bytes, &diagnostic) ||
      !ReadDiskFile(package_root / "checksums.sha256", kMaxManifestBytes,
                    &validated.checksum_bytes, &diagnostic)) {
    return Failure("integrity-failed", diagnostic);
  }
  if (fs::exists(package_root / "signature.json", error) &&
      !ReadDiskFile(package_root / "signature.json", kMaxManifestBytes,
                    &validated.signature_bytes, &diagnostic)) {
    return Failure("integrity-failed", diagnostic);
  }
  if (error ||
      !ParseJson(validated.manifest_bytes, &validated.manifest, &diagnostic) ||
      !ParseChecksums(validated.checksum_bytes, &validated.checksums,
                      &diagnostic) ||
      !ValidateManifestFields(&validated, &diagnostic) ||
      validated.id != package_id ||
      !ValidateFileSet(validated, &diagnostic)) {
    if (diagnostic.empty()) diagnostic = "installed package identity mismatch";
    return Failure("integrity-failed", diagnostic);
  }
  if (validated.development && !developer_mode_)
    return Failure("integrity-failed",
                   "development packages are disabled");
  if (!VerifySignature(validated.signature_bytes, validated.checksum_bytes,
                       TrustRoot(), validated.development, developer_mode_,
                       &diagnostic)) {
    return Failure("integrity-failed", diagnostic);
  }
  for (const auto& expected : validated.checksums) {
    std::string actual;
    if (!HashDiskFile(package_root / fs::path(expected.first),
                      kMaxExpandedBytes, &actual, &diagnostic) ||
        actual != expected.second) {
      if (diagnostic.empty())
        diagnostic = "installed-file digest mismatch: " + expected.first;
      return Failure("integrity-failed", diagnostic);
    }
  }
  const fs::path component =
      package_root /
      validated.manifest["component"].AsString().ToStdString();
  std::ifstream component_input(component, std::ios::binary);
  std::array<unsigned char, 4> wasm_magic{};
  component_input.read(reinterpret_cast<char*>(wasm_magic.data()),
                       wasm_magic.size());
  if (!component_input ||
      wasm_magic != std::array<unsigned char, 4>{0x00, 0x61, 0x73, 0x6d}) {
    return Failure("integrity-failed",
                   "installed component is not WebAssembly");
  }
  return {true, "integrity-verified",
          "Installed package signature and file digests verified", package_id,
          package_root, {}};
}

StoreResult PackageStore::Install(const fs::path& archive_path, bool replace) {
  ValidatedArchive validated;
  std::string diagnostic;
  if (!ValidateArchive(archive_path, TrustRoot(), developer_mode_, &validated,
                       &diagnostic)) {
    return Failure("validation-failed", diagnostic);
  }
  std::error_code error;
  fs::create_directories(PackagesRoot(), error);
  fs::create_directories(root_ / ".rollback" / validated.id, error);
  fs::create_directories(root_ / "state", error);
  if (error) return Failure("store-unavailable", error.message());

  const fs::path destination = PackagesRoot() / validated.id;
  if (fs::exists(destination) && !replace)
    return Failure("already-installed", validated.id + " is already installed");
  const fs::path staging = UniquePath(root_, ".staging-");
  if (staging.empty() || !fs::create_directory(staging, error))
    return Failure("staging-failed", error.message());

  fs::path rollback;
  auto cleanup = [&]() {
    std::error_code ignored;
    fs::remove_all(staging, ignored);
  };
  if (!ExtractValidated(archive_path, validated, staging, &diagnostic)) {
    cleanup();
    return Failure("digest-mismatch", diagnostic);
  }
  const fs::path component =
      staging /
      validated.manifest["component"].AsString().ToStdString();
  std::ifstream component_input(component, std::ios::binary);
  std::array<unsigned char, 4> wasm_magic{};
  component_input.read(reinterpret_cast<char*>(wasm_magic.data()),
                       wasm_magic.size());
  if (!component_input ||
      wasm_magic != std::array<unsigned char, 4>{0x00, 0x61, 0x73, 0x6d}) {
    cleanup();
    return Failure("component-invalid",
                   "declared component is not a WebAssembly binary");
  }
  const bool destination_exists = fs::exists(destination);
  const bool previous_enabled =
      destination_exists && ReadEnabled(root_, validated.id);
  const StoreResult disabled = SetEnabled(validated.id, false);
  if (!disabled.okay) {
    cleanup();
    return Failure("state-failed", disabled.message);
  }
  if (fs::exists(destination)) {
    StoredPackage previous;
    const std::string old_version =
        ReadManifestIdentity(destination, &previous) ? previous.version
                                                    : "unknown";
    rollback = root_ / ".rollback" / validated.id /
               RecoveryStamp(old_version);
    fs::rename(destination, rollback, error);
    if (error) {
      if (previous_enabled) SetEnabled(validated.id, true);
      cleanup();
      return Failure("rollback-staging-failed", error.message());
    }
  }
  fs::rename(staging, destination, error);
  if (error) {
    if (!rollback.empty()) {
      std::error_code restore_error;
      fs::rename(rollback, destination, restore_error);
    }
    if (previous_enabled) SetEnabled(validated.id, true);
    cleanup();
    return Failure("publish-failed", error.message());
  }
  return {true,
          rollback.empty() ? "installed" : "updated",
          rollback.empty() ? "Package installed" : "Package updated",
          validated.id,
          destination,
          rollback};
}

StoreResult PackageStore::Remove(const std::string& package_id) {
  if (!IsSafeIdentifier(package_id, true))
    return Failure("invalid-id", "invalid package identifier");
  const fs::path destination = PackagesRoot() / package_id;
  if (!fs::exists(destination))
    return Failure("not-installed", package_id + " is not installed");
  std::error_code error;
  const fs::path recovery_root = root_ / ".removed" / package_id;
  fs::create_directories(recovery_root, error);
  StoredPackage current;
  const std::string version =
      ReadManifestIdentity(destination, &current) ? current.version : "unknown";
  const fs::path recovery = recovery_root / RecoveryStamp(version);
  const bool previous_enabled = ReadEnabled(root_, package_id);
  const StoreResult disabled = SetEnabled(package_id, false);
  if (!disabled.okay) return Failure("state-failed", disabled.message);
  fs::rename(destination, recovery, error);
  if (error) {
    if (previous_enabled) SetEnabled(package_id, true);
    return Failure("remove-failed", error.message());
  }
  return {true, "removed", "Package moved to recoverable storage", package_id,
          {}, recovery};
}

StoreResult PackageStore::Rollback(const std::string& package_id) {
  if (!IsSafeIdentifier(package_id, true))
    return Failure("invalid-id", "invalid package identifier");
  const fs::path rollback_root = root_ / ".rollback" / package_id;
  std::vector<fs::path> candidates;
  std::error_code error;
  if (fs::exists(rollback_root)) {
    for (const auto& item : fs::directory_iterator(rollback_root)) {
      if (item.is_directory()) candidates.push_back(item.path());
    }
  }
  if (candidates.empty())
    return Failure("no-rollback", "no rollback version is available");
  std::sort(candidates.begin(), candidates.end());
  const fs::path selected = candidates.back();
  const fs::path destination = PackagesRoot() / package_id;
  fs::path displaced;
  const bool previous_enabled = ReadEnabled(root_, package_id);
  const StoreResult disabled = SetEnabled(package_id, false);
  if (!disabled.okay) return Failure("state-failed", disabled.message);
  if (fs::exists(destination)) {
    StoredPackage current;
    const std::string version =
        ReadManifestIdentity(destination, &current) ? current.version
                                                   : "unknown";
    displaced = rollback_root / RecoveryStamp(version);
    fs::rename(destination, displaced, error);
    if (error) {
      if (previous_enabled) SetEnabled(package_id, true);
      return Failure("rollback-failed", error.message());
    }
  }
  fs::rename(selected, destination, error);
  if (error) {
    if (!displaced.empty()) {
      std::error_code ignored;
      fs::rename(displaced, destination, ignored);
    }
    if (previous_enabled) SetEnabled(package_id, true);
    return Failure("rollback-failed", error.message());
  }
  return {true, "rolled-back", "Previous package version restored",
          package_id, destination, displaced};
}

StoreResult PackageStore::SetEnabled(const std::string& package_id,
                                     bool enabled) {
  if (!IsSafeIdentifier(package_id, true))
    return Failure("invalid-id", "invalid package identifier");
  std::error_code error;
  const fs::path state_root = root_ / "state";
  fs::create_directories(state_root, error);
  if (error) return Failure("state-failed", error.message());
  const fs::path target = state_root / (package_id + ".enabled");
  const fs::path temporary = state_root / (package_id + ".enabled.tmp");
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << (enabled ? "1\n" : "0\n");
    if (!output) return Failure("state-failed", "could not write package state");
  }
  fs::rename(temporary, target, error);
  if (error) {
    fs::remove(target, error);
    error.clear();
    fs::rename(temporary, target, error);
  }
  if (error) return Failure("state-failed", error.message());
  return {true, enabled ? "enabled" : "disabled",
          enabled ? "Package enabled" : "Package disabled", package_id, {},
          {}};
}

std::vector<StoredPackage> PackageStore::Installed(
    std::string* diagnostic) const {
  std::vector<StoredPackage> result;
  std::error_code error;
  if (!fs::exists(PackagesRoot())) return result;
  for (const auto& item : fs::directory_iterator(PackagesRoot(), error)) {
    if (error) break;
    if (!item.is_directory()) continue;
    StoredPackage package;
    if (!ReadManifestIdentity(item.path(), &package)) {
      if (diagnostic)
        *diagnostic = "invalid installed package: " + item.path().string();
      continue;
    }
    package.enabled = ReadEnabled(root_, package.id);
    result.push_back(std::move(package));
  }
  std::sort(result.begin(), result.end(),
            [](const auto& left, const auto& right) {
              return left.id < right.id;
            });
  if (error && diagnostic) *diagnostic = error.message();
  return result;
}

}  // namespace ppm
