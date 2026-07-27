import importlib.util
import json
import pathlib
import tempfile
import unittest


TOOL = pathlib.Path(__file__).parents[1] / "tools" / "portable_plugin.py"
SPEC = importlib.util.spec_from_file_location("portable_plugin", TOOL)
portable_plugin = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(portable_plugin)


class ToolingTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.package = self.root / "package"
        self.package.mkdir()
        (self.package / "resource.txt").write_text("portable\n", "utf-8")
        self.component = self.root / "plugin.wasm"
        self.component.write_bytes(b"\0asm\x0d\0\x01\0")
        self.manifest = {
            "format_version": 1,
            "id": "org.opencpn.test-author",
            "name": "Test author",
            "version": "1.2.3",
            "description": "fixture",
            "component": "component/plugin.wasm",
            "runtime": ">=0.1.0 <0.2.0",
            "portable_api": ">=0.5.0 <0.6.0",
            "portable_world": "plugin",
            "event_subscriptions": [
                {
                    "event": "chart.cursor",
                    "topic_prefix": "",
                    "queue_limit": 4,
                }
            ],
            "permissions": ["chart.cursor.read"],
            "resources": ["resource.txt"],
            "licenses": ["GPL-2.0-or-later"],
            "development": True,
        }
        self.manifest_path = self.package / "manifest.json"
        self.write_manifest()

    def tearDown(self):
        self.temporary.cleanup()

    def write_manifest(self):
        self.manifest_path.write_text(
            json.dumps(self.manifest, indent=2) + "\n", "utf-8"
        )

    def test_lint_rejects_subscription_without_permission(self):
        self.manifest["permissions"] = []
        self.write_manifest()
        with self.assertRaises(portable_plugin.LintError):
            portable_plugin.lint_manifest(self.manifest_path)
        self.manifest["https_domains"] = ["127.0.0.1"]
        self.write_manifest()
        with self.assertRaises(portable_plugin.LintError):
            portable_plugin.lint_manifest(self.manifest_path)

    def test_packages_are_deterministic(self):
        first = self.root / "first.ocpnp"
        second = self.root / "second.ocpnp"
        portable_plugin.create_package(self.manifest_path, self.component, first)
        portable_plugin.create_package(self.manifest_path, self.component, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_rejects_unknown_permission(self):
        self.manifest["permissions"].append("native.everything")
        self.write_manifest()
        with self.assertRaises(portable_plugin.LintError):
            portable_plugin.lint_manifest(self.manifest_path)

    def test_https_domains_require_permission_and_exact_names(self):
        self.manifest["https_domains"] = ["api.example.org"]
        self.write_manifest()
        with self.assertRaises(portable_plugin.LintError):
            portable_plugin.lint_manifest(self.manifest_path)
        self.manifest["permissions"].append("network.https")
        self.write_manifest()
        portable_plugin.lint_manifest(self.manifest_path)
        self.manifest["https_domains"] = ["*.example.org"]
        self.write_manifest()
        with self.assertRaises(portable_plugin.LintError):
            portable_plugin.lint_manifest(self.manifest_path)
        self.manifest["https_domains"] = ["127.0.0.1"]
        self.write_manifest()
        with self.assertRaises(portable_plugin.LintError):
            portable_plugin.lint_manifest(self.manifest_path)

    def test_host_environment_subscription_needs_no_permission(self):
        self.manifest["event_subscriptions"] = [
            {
                "event": "host.environment",
                "topic_prefix": "",
                "queue_limit": 1,
            }
        ]
        self.write_manifest()
        portable_plugin.lint_manifest(self.manifest_path)

    def test_all_opp_0_5_worlds_are_accepted(self):
        for world in (
            "plugin",
            "environment-provider-plugin",
            "weather-routing-plugin",
            "passage-weather-routing-plugin",
        ):
            self.manifest["portable_world"] = world
            self.write_manifest()
            portable_plugin.lint_manifest(self.manifest_path)
        self.manifest["portable_world"] = "native-plugin"
        self.write_manifest()
        with self.assertRaises(portable_plugin.LintError):
            portable_plugin.lint_manifest(self.manifest_path)


if __name__ == "__main__":
    unittest.main()
