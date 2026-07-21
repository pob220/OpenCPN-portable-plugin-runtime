#!/usr/bin/env python3

import importlib.util
import pathlib
import re
import subprocess
import sys
import unittest


RUNTIME = pathlib.Path(__file__).resolve().parents[1]
BETA = RUNTIME / "beta"
ROOT = RUNTIME.parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, BETA / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


profile = load("configure_profile")
diagnostics = load("collect_diagnostics")


class BetaToolsTest(unittest.TestCase):
    def test_profile_section_is_added_and_idempotent(self):
        initial = ["[Settings]\n", "OpenGL=0\n"]
        first = profile.update(initial)
        second = profile.update(first)
        self.assertEqual(first, second)
        rendered = "".join(first)
        self.assertIn("[PortablePlugins]\n", rendered)
        self.assertIn("EnableExperimental=1\n", rendered)
        self.assertIn("DeveloperMode=1\n", rendered)
        self.assertIn("DeveloperStartupAction=igrib.toggle\n", rendered)

    def test_profile_update_preserves_unrelated_values(self):
        initial = [
            "[PortablePlugins]\n",
            "EnableExperimental=0\n",
            "Unrelated=preserved\n",
            "DeveloperMode=0\n",
            "[Next]\n",
            "Value=1\n",
        ]
        rendered = "".join(profile.update(initial))
        self.assertEqual(rendered.count("EnableExperimental="), 1)
        self.assertEqual(rendered.count("DeveloperMode="), 1)
        self.assertIn("Unrelated=preserved\n", rendered)
        self.assertIn("[Next]\nValue=1\n", rendered)

    def test_diagnostic_redaction(self):
        value = "token=abc password: xyz authorization = Bearer-secret"
        redacted = diagnostics.SECRET.sub(r"\1\2<redacted>", value)
        self.assertNotIn("abc", redacted)
        self.assertNotIn("xyz", redacted)
        self.assertNotIn("Bearer-secret", redacted)

    def test_copernicus_password_stays_out_of_jobs_and_arguments(self):
        source = (ROOT / "gui/src/portable_environment_host.cpp").read_text()
        package = (ROOT / "portable-plugins/igrib/package/igrib-viewer.ui.json").read_text()
        self.assertIn("copernicus_nws", package)
        self.assertIn("copernicus_global", package)
        self.assertNotIn("copernicus_nws", source)
        self.assertNotIn("copernicus_global", source)
        self.assertIn("copernicusPasswordEnvironment", package)
        self.assertNotIn("copernicusPasswordEnvironment", source)
        self.assertIn("wxSecretStore", source)
        self.assertNotIn('request["copernicusPassword"]', source)
        self.assertNotIn('"--password"', source)
        self.assertNotIn("TPXO model directory", source)

    def test_shell_entry_points_have_working_help(self):
        for script in (
            "build-linux.sh",
            "debian-prerequisites.sh",
            "launch-linux.sh",
            "run-conformance.sh",
        ):
            result = subprocess.run(
                [str(BETA / script), "--help"],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Usage:", result.stdout)

    def test_beta_build_creates_portable_data_layout_and_both_plugins(self):
        source = (BETA / "build-linux.sh").read_text()
        self.assertIn('share/opencpn/"*', source)
        self.assertIn('ln -s "../share/opencpn/$name"', source)
        self.assertIn("org.opencpn.igrib-0.1.0.ocpnp", source)
        self.assertIn("org.opencpn.iweather-routing-0.1.0.ocpnp", source)

    def test_beta_prerequisites_cover_generator_dependencies(self):
        source = (BETA / "debian-prerequisites.sh").read_text()
        for package in (
            "libeccodes-dev",
            "libjsoncpp-dev",
            "libnetcdf-dev",
            "libcurl4-openssl-dev",
            "libqhull-dev",
            "libblosc-dev",
            "libzip-dev",
            "libsodium-dev",
            "libzstd-dev",
            "libproj-dev",
            "libbz2-dev",
        ):
            self.assertIn(package, source)

    def test_beta_generator_integrity_pin_matches_submodule(self):
        source = (BETA / "build-linux.sh").read_text()
        match = re.search(r'^generator_commit="([0-9a-f]{40})"$', source, re.M)
        self.assertIsNotNone(match)
        actual = subprocess.run(
            ["git", "-C", str(RUNTIME / "vendor/environmental-grib-generator"),
             "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        self.assertEqual(match.group(1), actual)

    def test_all_linux_eccodes_consumers_validate_the_public_header(self):
        runtime_cmake = (RUNTIME / "CMakeLists.txt").read_text()
        generator_cmake = (
            RUNTIME / "vendor/environmental-grib-generator/CMakeLists.txt"
        ).read_text()
        for source in (runtime_cmake, generator_cmake):
            self.assertIn("find_path(", source)
            self.assertIn("eccodes.h", source)
            self.assertIn("INTERFACE_INCLUDE_DIRECTORIES", source)


if __name__ == "__main__":
    unittest.main()
