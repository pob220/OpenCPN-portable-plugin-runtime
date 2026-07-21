#!/usr/bin/env python3
"""Run a signed package's Flatpak helper payload inside a real SDK sandbox."""

import argparse
import importlib.util
import json
import pathlib
import platform
import subprocess
import tempfile


TOOLS = pathlib.Path(__file__).resolve().parents[1] / "tools"


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
    parser.add_argument("--runtime", default="org.freedesktop.Sdk//24.08")
    args = parser.parse_args()
    machine = platform.machine().casefold()
    architecture = "aarch64" if machine in {"aarch64", "arm64"} else "x86_64"
    target = f"flatpak-{architecture}"
    installer = load_installer()
    with tempfile.TemporaryDirectory(prefix="igrib-flatpak-") as temporary:
        root = pathlib.Path(temporary).resolve()
        installed, _ = installer.install(
            args.package.resolve(strict=True),
            root / "installed",
            {"org.opencpn.development.igrib-2026": args.trusted_key},
            developer=True,
        )
        helper_root = installed / "helpers" / target
        decoder = helper_root / "igrib-environment-helper"
        generator = helper_root / "environmental-grib"
        if not decoder.is_file() or not generator.is_file():
            raise RuntimeError(f"signed package has no complete {target} payload")
        malformed = root / "malformed.grb"
        result = root / "result.json"
        malformed.write_bytes(b"not a GRIB stream")
        prefix = [
            "flatpak",
            "run",
            "--die-with-parent",
            f"--filesystem={root}",
            "--command=/bin/sh",
            args.runtime,
            "-c",
        ]
        decode = subprocess.run(
            prefix
            + [
                f"'{decoder}' inspect '{malformed}' '{result}'",
            ],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if decode.returncode != 1:
            raise RuntimeError(
                f"Flatpak decoder returned {decode.returncode}: "
                f"{decode.stderr or decode.stdout}"
            )
        document = json.loads(result.read_text())
        if document.get("error", {}).get("code") != "environment-decode-failed":
            raise RuntimeError("Flatpak decoder did not contain malformed input")
        capabilities = subprocess.run(
            prefix + [f"'{generator}' capabilities"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if capabilities.returncode != 0:
            raise RuntimeError(
                f"Flatpak generator returned {capabilities.returncode}: "
                f"{capabilities.stderr or capabilities.stdout}"
            )
        if json.loads(capabilities.stdout).get("schemaVersion") != 1:
            raise RuntimeError("Flatpak generator protocol is incompatible")
        print(
            json.dumps(
                {
                    "status": "passed",
                    "target": target,
                    "runtime": args.runtime,
                    "checks": [
                        "signed-package",
                        "sandbox-execution",
                        "malformed-input-containment",
                        "generator-protocol",
                    ],
                },
                sort_keys=True,
            )
        )


if __name__ == "__main__":
    main()
