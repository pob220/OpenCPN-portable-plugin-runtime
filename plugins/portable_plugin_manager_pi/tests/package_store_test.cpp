#include "package_store.h"

#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#define CHECK(expression)                                                   \
  do {                                                                      \
    if (!(expression)) {                                                    \
      std::cerr << "check failed at " << __FILE__ << ':' << __LINE__       \
                << ": " #expression "\n";                                  \
      return 1;                                                             \
    }                                                                       \
  } while (false)

namespace {

namespace fs = std::filesystem;

struct FileCloser {
  void operator()(FILE* value) const {
    if (value) std::fclose(value);
  }
};

std::string Digest(const std::string& value) {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
      EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  if (!context ||
      EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context.get(), value.data(), value.size()) != 1 ||
      EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1) {
    return {};
  }
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned index = 0; index < length; ++index)
    output << std::setw(2) << static_cast<unsigned>(digest[index]);
  return output.str();
}

bool AddFile(archive* output, const std::string& name,
             const std::string& contents, int mode = 0644) {
  archive_entry* entry = archive_entry_new();
  archive_entry_set_pathname(entry, name.c_str());
  archive_entry_set_filetype(entry, AE_IFREG);
  archive_entry_set_perm(entry, mode);
  archive_entry_set_size(entry, static_cast<la_int64_t>(contents.size()));
  const bool okay =
      archive_write_header(output, entry) == ARCHIVE_OK &&
      archive_write_data(output, contents.data(), contents.size()) ==
          static_cast<la_ssize_t>(contents.size());
  archive_entry_free(entry);
  return okay;
}

bool AddSymlink(archive* output, const std::string& name,
                const std::string& target) {
  archive_entry* entry = archive_entry_new();
  archive_entry_set_pathname(entry, name.c_str());
  archive_entry_set_filetype(entry, AE_IFLNK);
  archive_entry_set_perm(entry, 0777);
  archive_entry_set_symlink(entry, target.c_str());
  archive_entry_set_size(entry, 0);
  const bool okay = archive_write_header(output, entry) == ARCHIVE_OK;
  archive_entry_free(entry);
  return okay;
}

std::string Base64(const unsigned char* bytes, std::size_t length) {
  std::string encoded(4 * ((length + 2) / 3), '\0');
  const int result =
      EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()), bytes,
                      static_cast<int>(length));
  if (result < 0) return {};
  encoded.resize(static_cast<std::size_t>(result));
  return encoded;
}

std::string Sign(EVP_PKEY* key, const std::string& checksums,
                 bool corrupt) {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
      EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  std::vector<unsigned char> signature(64);
  std::size_t signature_length = signature.size();
  if (!context ||
      EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key) != 1 ||
      EVP_DigestSign(
          context.get(), signature.data(), &signature_length,
          reinterpret_cast<const unsigned char*>(checksums.data()),
          checksums.size()) != 1) {
    return {};
  }
  signature.resize(signature_length);
  std::string encoded = Base64(signature.data(), signature.size());
  if (corrupt && !encoded.empty())
    encoded[0] = encoded[0] == 'A' ? 'B' : 'A';
  return "{\"algorithm\":\"Ed25519\",\"signed\":\"checksums.sha256\","
         "\"key_id\":\"org.opencpn.test-key\",\"signature\":\"" +
         encoded + "\"}";
}

struct PackageOptions {
  bool development = true;
  bool bad_digest = false;
  bool traversal = false;
  bool duplicate_key = false;
  bool escaped_key = false;
  bool malformed_number = false;
  bool case_collision = false;
  bool executable_outside_helpers = false;
  bool symlink = false;
  bool wrong_magic = false;
  bool corrupt_signature = false;
  EVP_PKEY* signing_key = nullptr;
};

bool BuildPackage(const fs::path& output_path, const std::string& version,
                  const PackageOptions& options = {}) {
  const std::string wasm = options.wrong_magic
                               ? std::string("not-wasm")
                               : std::string("\0asm\x01\0\0\0", 8);
  std::string manifest =
      "{"
      "\"format_version\":" +
      std::string(options.malformed_number ? "01," : "1,") +
      "\"id\":\"org.opencpn.test-package\",";
  if (options.duplicate_key)
    manifest += "\"id\":\"org.opencpn.shadow-package\",";
  if (options.escaped_key)
    manifest += "\"\\u0069d\":\"org.opencpn.shadow-package\",";
  manifest +=
      "\"name\":\"Package Store Test\","
      "\"version\":\"" +
      version +
      "\","
      "\"component\":\"component/test.wasm\","
      "\"runtime\":\">=0.1.0 <0.2.0\","
      "\"portable_api\":\">=0.1.0 <0.2.0\","
      "\"permissions\":[\"ui.commands\"],"
      "\"development\":" +
      std::string(options.development ? "true" : "false") + "}";
  struct File {
    std::string contents;
    int mode = 0644;
  };
  std::map<std::string, File> files{
      {"component/test.wasm",
       {wasm, options.executable_outside_helpers ? 0755 : 0644}},
      {"manifest.json", {manifest, 0644}}};
  if (options.traversal) files.emplace("../escaped", File{"unsafe", 0644});
  if (options.case_collision) {
    files.emplace("resources/Icon.svg", File{"first", 0644});
    files.emplace("resources/icon.svg", File{"second", 0644});
  }
  if (options.symlink)
    files.emplace("resources/link", File{"target", 0644});

  std::ostringstream checksums;
  for (const auto& item : files) {
    std::string digest = Digest(item.second.contents);
    if (options.bad_digest && item.first == "component/test.wasm")
      digest[0] = digest[0] == '0' ? '1' : '0';
    checksums << digest << "  " << item.first << '\n';
  }
  std::string signature;
  if (options.signing_key)
    signature =
        Sign(options.signing_key, checksums.str(), options.corrupt_signature);

  archive* output = archive_write_new();
  archive_write_set_format_zip(output);
  archive_write_set_options(output, "zip:compression=deflate");
  if (archive_write_open_filename(output, output_path.c_str()) != ARCHIVE_OK) {
    archive_write_free(output);
    return false;
  }
  bool okay = true;
  for (const auto& item : files) {
    if (options.symlink && item.first == "resources/link")
      okay = AddSymlink(output, item.first, "target") && okay;
    else
      okay = AddFile(output, item.first, item.second.contents,
                     item.second.mode) &&
              okay;
  }
  okay = AddFile(output, "checksums.sha256", checksums.str()) && okay;
  if (!signature.empty())
    okay = AddFile(output, "signature.json", signature) && okay;
  okay = archive_write_close(output) == ARCHIVE_OK && okay;
  archive_write_free(output);
  return okay;
}

}  // namespace

int main() {
  const auto stamp =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const fs::path test_root =
      fs::temp_directory_path() /
      fs::path("ppm package \xCE\x94 " + std::to_string(stamp));
  const fs::path archive_dir = test_root / "archives with spaces";
  const fs::path store_root = test_root / "store";
  fs::create_directories(archive_dir);

  ppm::PackageStore store(store_root);
  store.SetDeveloperMode(true);

  const fs::path first = archive_dir / "first package.ocpnp";
  CHECK(BuildPackage(first, "0.1.0"));
  const auto installed = store.Install(first, false);
  CHECK(installed.okay);
  CHECK(installed.code == "installed");
  CHECK(installed.package_id == "org.opencpn.test-package");
  CHECK(fs::is_regular_file(installed.destination / "component/test.wasm"));
  CHECK(fs::is_regular_file(installed.destination / "checksums.sha256"));
  CHECK(store.AuditInstalled(installed.package_id).okay);

  auto packages = store.Installed();
  CHECK(packages.size() == 1);
  CHECK(packages[0].version == "0.1.0");
  CHECK(!packages[0].enabled);
  CHECK(store.SetEnabled(packages[0].id, true).okay);
  packages = store.Installed();
  CHECK(packages[0].enabled);
  CHECK(!store.Install(first, false).okay);

  const fs::path second = archive_dir / "updated package.ocpnp";
  CHECK(BuildPackage(second, "0.2.0"));
  const auto updated = store.Install(second, true);
  CHECK(updated.okay);
  CHECK(updated.code == "updated");
  CHECK(!updated.recovery.empty());
  packages = store.Installed();
  CHECK(packages.size() == 1);
  CHECK(packages[0].version == "0.2.0");
  CHECK(!packages[0].enabled);

  const auto rolled_back = store.Rollback("org.opencpn.test-package");
  CHECK(rolled_back.okay);
  packages = store.Installed();
  CHECK(packages[0].version == "0.1.0");
  CHECK(store.AuditInstalled(packages[0].id).okay);

  const fs::path bad_digest = archive_dir / "bad digest.ocpnp";
  PackageOptions bad_digest_options;
  bad_digest_options.bad_digest = true;
  CHECK(BuildPackage(bad_digest, "0.3.0", bad_digest_options));
  CHECK(!store.Install(bad_digest, true).okay);
  packages = store.Installed();
  CHECK(packages[0].version == "0.1.0");

  const fs::path traversal = archive_dir / "traversal.ocpnp";
  PackageOptions traversal_options;
  traversal_options.traversal = true;
  CHECK(BuildPackage(traversal, "0.3.0", traversal_options));
  CHECK(!store.Install(traversal, true).okay);
  CHECK(!fs::exists(test_root / "escaped"));

  const fs::path duplicate = archive_dir / "duplicate key.ocpnp";
  PackageOptions duplicate_options;
  duplicate_options.duplicate_key = true;
  CHECK(BuildPackage(duplicate, "0.3.0", duplicate_options));
  CHECK(!store.Install(duplicate, true).okay);

  const fs::path escaped_key = archive_dir / "escaped key.ocpnp";
  PackageOptions escaped_key_options;
  escaped_key_options.escaped_key = true;
  CHECK(BuildPackage(escaped_key, "0.3.0", escaped_key_options));
  CHECK(!store.Inspect(escaped_key).okay);

  const fs::path malformed_number = archive_dir / "bad number.ocpnp";
  PackageOptions malformed_number_options;
  malformed_number_options.malformed_number = true;
  CHECK(BuildPackage(malformed_number, "0.3.0",
                     malformed_number_options));
  CHECK(!store.Inspect(malformed_number).okay);

  const fs::path malformed_version = archive_dir / "bad version.ocpnp";
  CHECK(BuildPackage(malformed_version, "0.3.0-"));
  CHECK(!store.Inspect(malformed_version).okay);

  const fs::path collision = archive_dir / "case collision.ocpnp";
  PackageOptions collision_options;
  collision_options.case_collision = true;
  CHECK(BuildPackage(collision, "0.3.0", collision_options));
  CHECK(!store.Inspect(collision).okay);

  const fs::path executable = archive_dir / "bad executable.ocpnp";
  PackageOptions executable_options;
  executable_options.executable_outside_helpers = true;
  CHECK(BuildPackage(executable, "0.3.0", executable_options));
  CHECK(!store.Inspect(executable).okay);

  const fs::path symlink = archive_dir / "symlink.ocpnp";
  PackageOptions symlink_options;
  symlink_options.symlink = true;
  CHECK(BuildPackage(symlink, "0.3.0", symlink_options));
  CHECK(!store.Inspect(symlink).okay);

  const fs::path wrong_magic = archive_dir / "wrong magic.ocpnp";
  PackageOptions wrong_magic_options;
  wrong_magic_options.wrong_magic = true;
  CHECK(BuildPackage(wrong_magic, "0.3.0", wrong_magic_options));
  CHECK(!store.Install(wrong_magic, true).okay);

  const fs::path unsigned_production =
      archive_dir / "unsigned production.ocpnp";
  PackageOptions production_options;
  production_options.development = false;
  CHECK(BuildPackage(unsigned_production, "0.3.0", production_options));
  CHECK(!store.Install(unsigned_production, true).okay);

  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> key_context(
      EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), &EVP_PKEY_CTX_free);
  EVP_PKEY* generated_key = nullptr;
  CHECK(key_context);
  CHECK(EVP_PKEY_keygen_init(key_context.get()) == 1);
  CHECK(EVP_PKEY_keygen(key_context.get(), &generated_key) == 1);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> signing_key(
      generated_key, &EVP_PKEY_free);
  fs::create_directories(store.TrustRoot());
  const fs::path public_key =
      store.TrustRoot() / "org.opencpn.test-key.pem";
  {
    std::unique_ptr<FILE, FileCloser> output(
        std::fopen(public_key.c_str(), "wb"));
    CHECK(output);
    CHECK(PEM_write_PUBKEY(output.get(), signing_key.get()) == 1);
  }
  const fs::path signed_production =
      archive_dir / "signed production.ocpnp";
  PackageOptions signed_options;
  signed_options.development = false;
  signed_options.signing_key = signing_key.get();
  CHECK(BuildPackage(signed_production, "0.3.0", signed_options));
  CHECK(store.Inspect(signed_production).okay);
  CHECK(store.Install(signed_production, true).okay);
  CHECK(store.AuditInstalled("org.opencpn.test-package").okay);
  CHECK(fs::is_regular_file(
      store.PackagesRoot() / "org.opencpn.test-package" / "signature.json"));

  const fs::path corrupt_signature =
      archive_dir / "corrupt signature.ocpnp";
  signed_options.corrupt_signature = true;
  CHECK(BuildPackage(corrupt_signature, "0.4.0", signed_options));
  CHECK(!store.Inspect(corrupt_signature).okay);

  ppm::PackageStore untrusted_store(test_root / "untrusted store");
  CHECK(!untrusted_store.Inspect(signed_production).okay);

  const fs::path installed_component =
      store.PackagesRoot() / "org.opencpn.test-package" /
      "component/test.wasm";
  fs::permissions(installed_component, fs::perms::owner_write,
                  fs::perm_options::add);
  {
    std::ofstream tampered(installed_component,
                           std::ios::binary | std::ios::trunc);
    tampered << "tampered";
  }
  CHECK(!store.AuditInstalled("org.opencpn.test-package").okay);

  const auto removed = store.Remove("org.opencpn.test-package");
  CHECK(removed.okay);
  CHECK(!removed.recovery.empty());
  CHECK(store.Installed().empty());

  std::error_code ignored;
  fs::remove_all(test_root, ignored);
  return 0;
}
