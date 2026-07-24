#!/usr/bin/env python3

import importlib.util
import json
import pathlib
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
        source = (
            ROOT
            / "plugins/portable_plugin_manager_pi/src/environment_workbench.cpp"
        ).read_text()
        package = (ROOT / "portable-plugins/igrib/package/igrib-viewer.ui.json").read_text()
        self.assertIn("copernicus_nws", package)
        self.assertIn("copernicus_global", package)
        self.assertNotIn("copernicus_nws", source)
        self.assertNotIn("copernicus_global", source)
        self.assertIn("copernicusPasswordEnvironment", package)
        self.assertNotIn("copernicusPasswordEnvironment", source)
        self.assertIn("wxSecretStore", source)
        self.assertIn("#if wxUSE_SECRETSTORE", source)
        self.assertIn("held in memory for this generation only", source)
        self.assertNotIn('request["copernicusPassword"]', source)
        self.assertNotIn('"--password"', source)
        self.assertNotIn("TPXO model directory", source)

    def test_environment_frames_use_byte_budget_prefetch_and_grouped_routing(self):
        source = (
            ROOT
            / "plugins/portable_plugin_manager_pi/src/environment_workbench.cpp"
        ).read_text()

        self.assertIn('ReadLong("frameCacheMiB", 256)', source)
        self.assertIn("Operation::PrefetchFrame", source)
        self.assertIn("PrefetchNextFrame", source)
        self.assertIn("routing_frame_cache_bytes", source)
        self.assertIn("requests_by_time", source)
        self.assertNotIn("kRoutingFrameCacheLimit", source)

        # The memory budget controls retention, not the maximum GRIB area.
        # One decoded frame must remain usable even when it alone exceeds the
        # configured cache budget.
        self.assertIn("routing_frame_lru.size() > 1", source)
        self.assertIn("never\n  // rejected", source)

    def test_igrib_surface_owns_compact_layout_icons_and_generator_presets(self):
        package = ROOT / "portable-plugins/igrib/package"
        surface = json.loads((package / "igrib-viewer.ui.json").read_text())
        manifest = json.loads((package / "manifest.json").read_text())

        self.assertEqual(
            surface["layout"]["primary_field_rows"],
            [["wind", "wave", "current"],
             ["pressure", "air-temperature"]],
        )
        controls = {item["id"]: item for item in surface["controls"]}
        for identifier in (
            "previous", "next", "play", "now", "open", "settings",
            "weather-table", "download", "generate", "cancel",
        ):
            resource = controls[identifier]["icon_resource"]
            self.assertTrue(resource.startswith("resources/controls/"))
            self.assertIn(resource, manifest["resources"])

        generator = surface["generator"]
        self.assertEqual(
            [item["label"] for item in generator["area_presets"]],
            [
                "Custom bbox", "Current chart area",
                "Irish Sea / North Channel", "Western English Channel",
                "North Sea", "Bay of Biscay",
                "Gulf Stream / Florida Straits",
                "US East Coast / Gulf Stream", "Caribbean",
            ],
        )
        self.assertEqual(
            {item["id"] for item in generator["weather_presets"]},
            {"minimal", "routing", "marine"},
        )

        host = (
            ROOT
            / "plugins/portable_plugin_manager_pi/src/environment_workbench.cpp"
        ).read_text()
        for provider_id in (
            "copernicus_nws", "copernicus_global", "noaa_rtofs_global",
        ):
            self.assertNotIn(provider_id, host)
        self.assertIn("SurfaceAreaPresets(surface_definition)", host)
        self.assertIn("SurfaceWeatherPresets(surface_definition)", host)

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
            "libeccodes-tools",
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

    def test_beta_generator_is_vendored_in_the_portable_repository(self):
        source = (BETA / "build-linux.sh").read_text()
        self.assertIn("generator_source=vendored", source)
        self.assertNotIn("submodule update", source)
        self.assertTrue(
            (RUNTIME / "vendor/environmental-grib-generator/CMakeLists.txt")
            .is_file()
        )

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
