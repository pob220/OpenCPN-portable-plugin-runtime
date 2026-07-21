#!/usr/bin/env python3

import importlib.util
import hashlib
import json
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


TOOLS = pathlib.Path(__file__).resolve().parents[1] / "tools"


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


installer = load_tool("install_package")


class PackageToolsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self.temporary.name)
        self.source = self.base / "source"
        (self.source / "component").mkdir(parents=True)
        (self.source / "resources").mkdir()
        (self.source / "component" / "plugin.wasm").write_bytes(b"\0asmcomponent")
        (self.source / "resources" / "icon.svg").write_text("<svg/>")
        (self.source / "manifest.json").write_text(
            json.dumps(
                {
                    "format_version": 1,
                    "id": "org.opencpn.test-plugin",
                    "name": "Test",
                    "version": "1.0.0",
                    "component": "component/plugin.wasm",
                    "development": True,
                }
            )
        )

    def tearDown(self):
        self.temporary.cleanup()

    def build(self, name):
        output = self.base / name
        subprocess.run(
            [
                sys.executable,
                str(TOOLS / "build_package.py"),
                "--root",
                str(self.source),
                "--output",
                str(output),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        return output

    def test_reproducible_and_installable(self):
        first = self.build("first.ocpnp")
        second = self.build("second.ocpnp")
        self.assertEqual(first.read_bytes(), second.read_bytes())
        destination, rollback = installer.install(
            first, self.base / "installed", developer=True
        )
        self.assertIsNone(rollback)
        self.assertEqual(destination.name, "org.opencpn.test-plugin")
        self.assertEqual(
            (destination / "component" / "plugin.wasm").read_bytes(),
            b"\0asmcomponent",
        )

    def test_helper_execute_mode_is_preserved(self):
        helper = self.source / "helpers" / "linux-gnu-x86_64" / "helper"
        helper.parent.mkdir(parents=True)
        helper.write_bytes(b"helper")
        helper.chmod(0o755)
        package = self.build("helper.ocpnp")
        destination, _ = installer.install(
            package, self.base / "installed", developer=True
        )
        installed = destination / "helpers" / "linux-gnu-x86_64" / "helper"
        self.assertTrue(installed.stat().st_mode & stat.S_IXUSR)

    def test_replace_is_atomic_and_retains_rollback(self):
        package = self.build("first.ocpnp")
        destination, _ = installer.install(
            package, self.base / "installed", developer=True
        )
        (self.source / "resources" / "icon.svg").write_text("<svg>new</svg>")
        replacement = self.build("replacement.ocpnp")
        replaced, rollback = installer.install(
            replacement, self.base / "installed", developer=True, replace=True
        )
        self.assertEqual(replaced, destination)
        self.assertIsNotNone(rollback)
        self.assertEqual((replaced / "resources" / "icon.svg").read_text(), "<svg>new</svg>")
        self.assertEqual((rollback / "resources" / "icon.svg").read_text(), "<svg/>")

    def test_executable_outside_helpers_is_rejected(self):
        (self.source / "resources" / "icon.svg").chmod(0o755)
        package = self.build("bad-executable.ocpnp")
        with self.assertRaisesRegex(installer.PackageError, "executable outside"):
            installer.validate(package, developer=True)

    def test_development_package_is_rejected_by_default(self):
        package = self.build("development.ocpnp")
        with self.assertRaisesRegex(installer.PackageError, "development mode"):
            installer.validate(package)

    def test_ed25519_signature_is_verified_and_tampering_rejected(self):
        private_key = Ed25519PrivateKey.generate()
        private_path = self.base / "private.pem"
        public_path = self.base / "public.pem"
        private_path.write_bytes(
            private_key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            )
        )
        public_path.write_bytes(
            private_key.public_key().public_bytes(
                serialization.Encoding.PEM,
                serialization.PublicFormat.SubjectPublicKeyInfo,
            )
        )
        package = self.base / "signed.ocpnp"
        subprocess.run(
            [
                sys.executable,
                str(TOOLS / "build_package.py"),
                "--root",
                str(self.source),
                "--output",
                str(package),
                "--signing-key",
                str(private_path),
                "--key-id",
                "test-key",
            ],
            check=True,
        )
        installer.validate(
            package, {"test-key": public_path}, developer=True
        )
        with self.assertRaisesRegex(installer.PackageError, "signature-untrusted"):
            installer.validate(package, {}, developer=True)

    def test_verified_target_packages_form_one_signed_archive(self):
        private_key = Ed25519PrivateKey.generate()
        private_path = self.base / "private.pem"
        public_path = self.base / "public.pem"
        private_path.write_bytes(
            private_key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            )
        )
        public_path.write_bytes(
            private_key.public_key().public_bytes(
                serialization.Encoding.PEM,
                serialization.PublicFormat.SubjectPublicKeyInfo,
            )
        )
        manifest = json.loads((self.source / "manifest.json").read_text())
        manifest["helpers"] = {
            "decoder": {
                "required": True,
                "protocol": 1,
                "targets": ["linux-gnu-x86_64", "macos-aarch64"],
            }
        }
        (self.source / "manifest.json").write_text(json.dumps(manifest))

        packages = []
        for target in ("linux-gnu-x86_64", "macos-aarch64"):
            shutil.rmtree(self.source / "helpers", ignore_errors=True)
            helper = self.source / "helpers" / target / "decoder"
            helper.parent.mkdir(parents=True)
            helper.write_bytes(target.encode("ascii"))
            helper.chmod(0o755)
            package = self.base / f"{target}.ocpnp"
            subprocess.run(
                [
                    sys.executable,
                    str(TOOLS / "build_package.py"),
                    "--root",
                    str(self.source),
                    "--output",
                    str(package),
                    "--signing-key",
                    str(private_path),
                    "--key-id",
                    "test-key",
                ],
                check=True,
            )
            packages.append(package)

        merged = self.base / "multi-target.ocpnp"
        command = [
            sys.executable,
            str(TOOLS / "assemble_multitarget_package.py"),
            "--trusted-key",
            f"test-key={public_path}",
            "--output",
            str(merged),
            "--signing-key",
            str(private_path),
            "--key-id",
            "test-key",
        ]
        for package in packages:
            command.extend(["--package", str(package)])
        for target in ("linux-gnu-x86_64", "macos-aarch64"):
            command.extend(["--require-target", target])
        subprocess.run(command, check=True, capture_output=True, text=True)
        _, entries = installer.validate(
            merged, {"test-key": public_path}, developer=True
        )
        self.assertIn("helpers/linux-gnu-x86_64/decoder", entries)
        self.assertIn("helpers/macos-aarch64/decoder", entries)

    def test_helper_root_dereferences_only_in_tree_file_symlinks(self):
        private_key = Ed25519PrivateKey.generate()
        private_path = self.base / "private.pem"
        public_path = self.base / "public.pem"
        private_path.write_bytes(
            private_key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            )
        )
        public_path.write_bytes(
            private_key.public_key().public_bytes(
                serialization.Encoding.PEM,
                serialization.PublicFormat.SubjectPublicKeyInfo,
            )
        )
        manifest = json.loads((self.source / "manifest.json").read_text())
        manifest["helpers"] = {
            "decoder": {
                "required": True,
                "protocol": 1,
                "targets": ["linux-gnu-x86_64", "flatpak-x86_64"],
            }
        }
        (self.source / "manifest.json").write_text(json.dumps(manifest))
        decoder = self.source / "helpers/linux-gnu-x86_64/decoder"
        decoder.parent.mkdir(parents=True)
        decoder.write_bytes(b"linux decoder")
        decoder.chmod(0o755)
        package = self.base / "linux.ocpnp"
        subprocess.run(
            [
                sys.executable,
                str(TOOLS / "build_package.py"),
                "--root",
                str(self.source),
                "--output",
                str(package),
                "--signing-key",
                str(private_path),
                "--key-id",
                "test-key",
            ],
            check=True,
        )
        helper_root = self.base / "flatpak"
        (helper_root / "lib").mkdir(parents=True)
        (helper_root / "igrib-environment-helper").write_bytes(b"decoder")
        (helper_root / "environmental-grib").write_bytes(b"generator")
        real_library = helper_root / "lib/libexample.so.1.2"
        real_library.write_bytes(b"library bytes")
        (helper_root / "lib/libexample.so.1").symlink_to(real_library.name)
        merged = self.base / "with-soname.ocpnp"
        subprocess.run(
            [
                sys.executable,
                str(TOOLS / "assemble_multitarget_package.py"),
                "--package",
                str(package),
                "--helper-root",
                f"flatpak-x86_64={helper_root}",
                "--trusted-key",
                f"test-key={public_path}",
                "--require-target",
                "linux-gnu-x86_64",
                "--require-target",
                "flatpak-x86_64",
                "--output",
                str(merged),
                "--signing-key",
                str(private_path),
                "--key-id",
                "test-key",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        _, entries = installer.validate(
            merged, {"test-key": public_path}, developer=True
        )
        alias = "helpers/flatpak-x86_64/lib/libexample.so.1"
        packaged_decoder = (
            "helpers/flatpak-x86_64/igrib-environment-helper"
        )
        packaged_generator = "helpers/flatpak-x86_64/environmental-grib"
        with zipfile.ZipFile(merged) as archive:
            self.assertEqual(archive.read(alias), b"library bytes")
            self.assertTrue(stat.S_ISREG(entries[alias].external_attr >> 16))
            self.assertFalse((entries[alias].external_attr >> 16) & stat.S_IXUSR)
            self.assertTrue(
                (entries[packaged_decoder].external_attr >> 16) & stat.S_IXUSR
            )
            self.assertTrue(
                (entries[packaged_generator].external_attr >> 16) & stat.S_IXUSR
            )

        generator = helper_root / "environmental-grib"
        generator.unlink()
        incomplete = subprocess.run(
            [
                sys.executable,
                str(TOOLS / "assemble_multitarget_package.py"),
                "--package",
                str(package),
                "--helper-root",
                f"flatpak-x86_64={helper_root}",
                "--trusted-key",
                f"test-key={public_path}",
                "--output",
                str(self.base / "missing-generator.ocpnp"),
                "--signing-key",
                str(private_path),
                "--key-id",
                "test-key",
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(incomplete.returncode, 0)
        self.assertIn("generator helper is absent", incomplete.stderr)
        generator.write_bytes(b"generator")

        outside = self.base / "outside.so"
        outside.write_bytes(b"outside")
        (helper_root / "lib/escape.so").symlink_to(outside)
        rejected = subprocess.run(
            [
                sys.executable,
                str(TOOLS / "assemble_multitarget_package.py"),
                "--package",
                str(package),
                "--helper-root",
                f"flatpak-x86_64={helper_root}",
                "--trusted-key",
                f"test-key={public_path}",
                "--output",
                str(self.base / "escape.ocpnp"),
                "--signing-key",
                str(private_path),
                "--key-id",
                "test-key",
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("symlink escapes", rejected.stderr)

    def test_checksum_tamper_is_rejected(self):
        original = self.build("original.ocpnp")
        tampered = self.base / "tampered.ocpnp"
        with zipfile.ZipFile(original) as source, zipfile.ZipFile(tampered, "w") as target:
            for info in source.infolist():
                data = source.read(info)
                if info.filename == "component/plugin.wasm":
                    data += b"tampered"
                target.writestr(info, data)
        with self.assertRaisesRegex(installer.PackageError, "digest-mismatch"):
            installer.validate(tampered, developer=True)

    def test_traversal_is_rejected(self):
        package = self.base / "traversal.ocpnp"
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr("../outside", b"bad")
        with self.assertRaisesRegex(installer.PackageError, "archive-policy"):
            installer.validate(package, developer=True)

    def test_entry_count_remains_bounded(self):
        package = self.base / "too-many-entries.ocpnp"
        with zipfile.ZipFile(package, "w") as archive:
            for index in range(installer.MAX_ENTRIES + 1):
                archive.writestr(f"resources/entry-{index:05d}", b"")
        with self.assertRaisesRegex(installer.PackageError, "too many entries"):
            installer.validate(package, developer=True)

    def test_symlink_is_rejected(self):
        package = self.base / "symlink.ocpnp"
        info = zipfile.ZipInfo("component/plugin.wasm")
        info.create_system = 3
        info.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr(info, b"target")
        with self.assertRaisesRegex(installer.PackageError, "non-regular"):
            installer.validate(package, developer=True)

    def test_builder_rejects_symlink(self):
        (self.source / "resources" / "linked.svg").symlink_to("icon.svg")
        result = subprocess.run(
            [
                sys.executable,
                str(TOOLS / "build_package.py"),
                "--root",
                str(self.source),
                "--output",
                str(self.base / "linked.ocpnp"),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("symbolic link", result.stderr)

    def test_non_string_plugin_id_is_rejected(self):
        (self.source / "manifest.json").write_text(
            json.dumps(
                {
                    "format_version": 1,
                    "id": 42,
                    "component": "component/plugin.wasm",
                    "development": True,
                }
            )
        )
        package = self.build("numeric-id.ocpnp")
        with self.assertRaisesRegex(installer.PackageError, "manifest-invalid"):
            installer.validate(package, developer=True)

    def test_case_colliding_names_are_rejected(self):
        package = self.base / "duplicate.ocpnp"
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr("manifest.json", b"{}")
            archive.writestr("MANIFEST.JSON", b"{}")
        with self.assertRaisesRegex(installer.PackageError, "duplicate"):
            installer.validate(package, developer=True)

    def test_control_character_path_is_rejected(self):
        package = self.base / "control.ocpnp"
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr("resources/bad\x01name", b"bad")
        with self.assertRaisesRegex(installer.PackageError, "archive-policy"):
            installer.validate(package, developer=True)

    def test_cross_platform_reserved_path_is_rejected(self):
        package = self.base / "reserved.ocpnp"
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr("resources/CON.txt", b"bad")
        with self.assertRaisesRegex(installer.PackageError, "archive-policy"):
            installer.validate(package, developer=True)

    def test_duplicate_manifest_key_is_rejected(self):
        package = self.base / "duplicate-key.ocpnp"
        manifest = (
            b'{"format_version":1,"id":"org.opencpn.test-plugin",'
            b'"id":"org.opencpn.other","component":"component/plugin.wasm",'
            b'"development":true}'
        )
        component = b"\0asmcomponent"
        checksums = (
            hashlib.sha256(component).hexdigest()
            + "  component/plugin.wasm\n"
            + hashlib.sha256(manifest).hexdigest()
            + "  manifest.json\n"
        ).encode("ascii")
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr("component/plugin.wasm", component)
            archive.writestr("manifest.json", manifest)
            archive.writestr("checksums.sha256", checksums)
        with self.assertRaisesRegex(installer.PackageError, "duplicate JSON key"):
            installer.validate(package, developer=True)


if __name__ == "__main__":
    unittest.main()
