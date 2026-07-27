#!/usr/bin/env python3
"""Verify the signed, platform-neutral OPP API 0.4 iPolars package."""

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
    with tempfile.TemporaryDirectory(prefix="ipolars-conformance-") as work:
        destination, rollback = installer.install(
            args.package.resolve(strict=True),
            pathlib.Path(work) / "installed",
            {KEY_ID: args.trusted_key.resolve(strict=True)},
            True,
        )
        if rollback is not None:
            raise RuntimeError("fresh install unexpectedly created a rollback")

        manifest = json.loads((destination / "manifest.json").read_text())
        expected_identity = {
            "id": "org.opencpn.ipolars",
            "version": "0.2.0",
            "portable_api": ">=0.4.0 <0.5.0",
            "portable_world": "plugin",
        }
        for field, expected in expected_identity.items():
            if manifest.get(field) != expected:
                raise RuntimeError(f"iPolars manifest has incorrect {field}")

        permissions = set(manifest.get("permissions", []))
        required_permissions = {
            "ui.commands",
            "ui.surfaces",
            "storage.user-selected",
            "navigation.nmea.read",
        }
        if not required_permissions.issubset(permissions):
            raise RuntimeError("iPolars omits a required OPP capability")
        if "settings.read-write" in permissions:
            raise RuntimeError("iPolars requests unused settings authority")

        subscriptions = manifest.get("event_subscriptions", [])
        if subscriptions != [
            {
                "event": "navigation.nmea0183",
                "topic_prefix": "$",
                "queue_limit": 128,
            }
        ]:
            raise RuntimeError("iPolars typed NMEA subscription is incorrect")

        component = destination / manifest["component"]
        if component.read_bytes()[:4] != b"\0asm":
            raise RuntimeError("iPolars component is not WebAssembly")

        surface_resource = manifest.get("surfaces", {}).get("polars.editor")
        if not isinstance(surface_resource, str):
            raise RuntimeError("iPolars omits its declared editor surface")
        surface = json.loads((destination / surface_resource).read_text())
        if surface.get("surface") != "polars.editor":
            raise RuntimeError("iPolars editor surface identity is incorrect")

        interfaces = destination / "interfaces" / "opencpn-opp-0.4"
        if not interfaces.is_dir():
            raise RuntimeError("iPolars omits the authoritative OPP 0.4 WIT")
        contract = "\n".join(
            path.read_text() for path in sorted(interfaces.glob("*.wit"))
        )
        for required in (
            "package opencpn:opp@0.4.0",
            "action-invocation",
            "interface user-files",
            "variant event-payload",
            "world plugin-world",
        ):
            if required not in contract:
                raise RuntimeError(f"OPP 0.4 contract omits {required}")

        helper_root = destination / "helpers"
        if helper_root.exists() and any(helper_root.rglob("*")):
            raise RuntimeError("iPolars must remain a Wasm-only package")

    print("iPolars OPP API 0.4 package conformance passed")


if __name__ == "__main__":
    main()
