#!/usr/bin/env python3
"""Verify the signed, platform-neutral iWeatherRouting reference package."""

import argparse
import importlib.util
import json
import pathlib
import tempfile


TOOLS = pathlib.Path(__file__).resolve().parents[1] / "tools"
KEY_ID = "org.opencpn.development.portable-reference-2026"


def load_installer():
    spec = importlib.util.spec_from_file_location(
        "install_package", TOOLS / "install_package.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", required=True, type=pathlib.Path)
    parser.add_argument("--trusted-key", required=True, type=pathlib.Path)
    args = parser.parse_args()

    installer = load_installer()
    with tempfile.TemporaryDirectory(prefix="iweather-routing-conformance-") as work:
        destination, rollback = installer.install(
            args.package.resolve(strict=True),
            pathlib.Path(work) / "installed",
            {KEY_ID: args.trusted_key.resolve(strict=True)},
            True,
        )
        if rollback is not None:
            raise RuntimeError("fresh install unexpectedly created a rollback")
        manifest = json.loads((destination / "manifest.json").read_text())
        if manifest.get("id") != "org.opencpn.iweather-routing":
            raise RuntimeError("package identity is incorrect")
        required = {
            "weather-routing.compute",
            "environment.consume",
            "charts.coverage",
            "navigation.position.read",
            "navigation.objects.read",
            "navigation.routes.write",
        }
        if not required.issubset(set(manifest.get("permissions", []))):
            raise RuntimeError("routing package omits a required capability")
        component = destination / manifest["component"]
        if component.read_bytes()[:4] != b"\0asm":
            raise RuntimeError("routing component is not WebAssembly")
        bundled_polar = (
            destination / "resources" / "Nicholson35_Mk1_cruising_realistic.pol"
        )
        header = bundled_polar.read_text().splitlines()[0].lower()
        if not header.startswith("twa/tws"):
            raise RuntimeError("routing package omits its valid demonstration polar")
        surface_resource = manifest.get("surfaces", {}).get("routing.workbench")
        if not isinstance(surface_resource, str) or not surface_resource:
            raise RuntimeError("routing package omits its workbench surface")
        surface_path = destination / surface_resource
        if destination not in surface_path.resolve().parents:
            raise RuntimeError("routing surface escapes the installed package")
        surface = json.loads(surface_path.read_text())
        if surface.get("schema") != "org.opencpn.portable-ui/0.2" or \
                surface.get("surface") != "routing-workbench":
            raise RuntimeError("routing UI schema is incompatible")
        menu_ids = {
            item.get("id")
            for menu in surface.get("menus", [])
            if isinstance(menu, dict)
            for item in menu.get("items", [])
            if isinstance(item, dict) and item.get("id")
        }
        required_menu_ids = {
            "new-routing", "edit-routing", "compute-routing", "stop-routing",
            "export-gpx", "refresh-positions", "show-configuration",
            "send-to-opencpn",
            "show-results", "show-isochrones", "route-to-cursor",
            "show-stability-corridor", "show-route-wind",
            "boat-at-grib-time", "about", "close",
        }
        if not required_menu_ids.issubset(menu_ids):
            raise RuntimeError("routing UI omits familiar workbench actions")
        manager = surface.get("manager", {})
        manager_actions = {
            action.get("id")
            for action in manager.get("actions", [])
            if isinstance(action, dict)
        }
        if not {"compute-routing", "edit-routing", "export-gpx"}.issubset(
                manager_actions):
            raise RuntimeError("routing UI omits manager actions")
        if not manager.get("positions", {}).get("columns") or not \
                manager.get("routings", {}).get("columns"):
            raise RuntimeError("routing UI omits manager table columns")
        control_ids = {
            control.get("id")
            for control in surface.get("controls", [])
            if isinstance(control, dict)
        }
        required_controls = {
            "start-latitude",
            "start-source",
            "start-waypoint",
            "destination-latitude",
            "destination-source",
            "destination-waypoint",
            "refresh-positions",
            "use-opencpn-route",
            "opencpn-route",
            "departure-utc",
            "vessel-performance-file",
            "vessel-performance-status",
            "environment-provider",
            "avoid-unsafe",
            "minimum-wind-angle",
            "maximum-wind-angle",
            "maximum-true-wind",
            "maximum-apparent-wind",
            "maximum-wave",
            "maximum-opposing-wind-current",
            "land-safety-margin",
            "use-currents",
            "require-current-data",
            "use-waves",
            "require-wave-data",
            "maximum-latitude",
            "upwind-efficiency",
            "downwind-efficiency",
            "tack-penalty",
            "gybe-penalty",
            "allow-motor-sailing",
            "allow-motor",
            "motor-threshold",
            "motor-speed",
            "motor-sailing-boost",
            "motor-hysteresis",
            "minimum-motor-run",
            "mode-change-penalty",
            "maximum-motor-hours",
            "fuel-consumption",
            "maximum-fuel",
            "maximum-search-angle",
            "destination-tolerance",
            "compare-departures",
            "departure-window",
            "departure-workers",
            "route-metrics",
            "route-schedule",
            "validation-diagnostics",
            "adaptive-headings",
            "refined-heading-step",
            "spatial-cell",
            "labels-per-cell",
            "show-stability-corridor",
            "show-route-wind",
            "export-gpx",
            "send-to-opencpn",
            "calculate",
            "cancel",
            "progress",
        }
        if not required_controls.issubset(control_ids):
            raise RuntimeError("routing UI omits required declarative controls")
        controls_by_id = {
            control.get("id"): control
            for control in surface.get("controls", [])
            if isinstance(control, dict) and control.get("id")
        }
        if len(controls_by_id["departure-results"].get("columns", [])) != 21 or \
                len(controls_by_id["route-schedule"].get("columns", [])) != 9:
            raise RuntimeError("routing UI omits its package-owned table layout")
        services = manifest.get("requires", [])
        if not any(
            service.get("interface") == "org.opencpn.environment.provider"
            for service in services
            if isinstance(service, dict)
        ):
            raise RuntimeError("routing package omits its environment service")
        interface = (
            destination / "interfaces" / "opencpn-portable.wit"
        ).read_text()
        for operation in (
            "environment-sample-batch",
            "charts-query-segments",
            "calculate-route",
        ):
            if operation not in interface:
                raise RuntimeError(f"portable interface omits {operation}")
        helper_root = destination / "helpers"
        if helper_root.exists() and any(helper_root.rglob("*")):
            raise RuntimeError("iWeatherRouting must remain a Wasm-only package")

    print("iWeatherRouting package conformance passed")


if __name__ == "__main__":
    main()
