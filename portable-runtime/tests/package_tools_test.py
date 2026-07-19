#!/usr/bin/env python3

import importlib.util
import hashlib
import json
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile


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
        destination = installer.install(first, self.base / "installed")
        self.assertEqual(destination.name, "org.opencpn.test-plugin")
        self.assertEqual(
            (destination / "component" / "plugin.wasm").read_bytes(),
            b"\0asmcomponent",
        )

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
            installer.validate(tampered)

    def test_traversal_is_rejected(self):
        package = self.base / "traversal.ocpnp"
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr("../outside", b"bad")
        with self.assertRaisesRegex(installer.PackageError, "archive-policy"):
            installer.validate(package)

    def test_symlink_is_rejected(self):
        package = self.base / "symlink.ocpnp"
        info = zipfile.ZipInfo("component/plugin.wasm")
        info.create_system = 3
        info.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr(info, b"target")
        with self.assertRaisesRegex(installer.PackageError, "non-regular"):
            installer.validate(package)

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
            installer.validate(package)

    def test_case_colliding_names_are_rejected(self):
        package = self.base / "duplicate.ocpnp"
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr("manifest.json", b"{}")
            archive.writestr("MANIFEST.JSON", b"{}")
        with self.assertRaisesRegex(installer.PackageError, "duplicate"):
            installer.validate(package)

    def test_control_character_path_is_rejected(self):
        package = self.base / "control.ocpnp"
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr("resources/bad\x01name", b"bad")
        with self.assertRaisesRegex(installer.PackageError, "archive-policy"):
            installer.validate(package)

    def test_cross_platform_reserved_path_is_rejected(self):
        package = self.base / "reserved.ocpnp"
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr("resources/CON.txt", b"bad")
        with self.assertRaisesRegex(installer.PackageError, "archive-policy"):
            installer.validate(package)

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
            installer.validate(package)


if __name__ == "__main__":
    unittest.main()
