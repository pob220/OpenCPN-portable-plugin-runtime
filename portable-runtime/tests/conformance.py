#!/usr/bin/env python3
"""Portable iGRIB package/helper conformance runner.

Run this unchanged on each supported host.  It verifies package trust and
installation policy, target-helper selection, malformed-input containment and,
when a fixture is supplied, real metadata/frame decode and generator output.
The JSON output deliberately distinguishes pass, skip and target identity.
"""

import argparse
import importlib.util
import json
import pathlib
import platform
import subprocess
import sys
import tempfile
import time


TOOLS = pathlib.Path(__file__).resolve().parents[1] / "tools"


def load_installer():
    spec = importlib.util.spec_from_file_location(
        "install_package", TOOLS / "install_package.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def target_name():
    machine = platform.machine().casefold()
    architecture = (
        "aarch64" if machine in {"aarch64", "arm64"} else
        "x86_64" if machine in {"x86_64", "amd64"} else None
    )
    if architecture is None:
        raise RuntimeError(f"unsupported processor architecture: {machine}")
    if sys.platform.startswith("linux"):
        return f"linux-gnu-{architecture}"
    if sys.platform == "darwin":
        return f"macos-{architecture}"
    if sys.platform == "win32" and architecture == "x86_64":
        return "windows-x86_64"
    raise RuntimeError(f"unsupported host platform: {sys.platform}/{machine}")


def run(command, timeout=60, expected=0):
    started = time.perf_counter()
    result = subprocess.run(
        [str(item) for item in command],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    elapsed_ms = round((time.perf_counter() - started) * 1000, 3)
    if result.returncode != expected:
        raise RuntimeError(
            f"command returned {result.returncode}, expected {expected}: "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )
    return result, elapsed_ms


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", required=True, type=pathlib.Path)
    parser.add_argument("--trusted-key", required=True, type=pathlib.Path)
    parser.add_argument("--fixture", type=pathlib.Path)
    parser.add_argument("--full-generator", action="store_true")
    parser.add_argument("--output-json", type=pathlib.Path)
    args = parser.parse_args()

    installer = load_installer()
    target = target_name()
    report = {
        "schema_version": 1,
        "status": "failed",
        "target": target,
        "os": platform.platform(),
        "python": platform.python_version(),
        "checks": {},
    }
    with tempfile.TemporaryDirectory(prefix="igrib-conformance-") as temporary:
        work = pathlib.Path(temporary)
        trusted = {"org.opencpn.development.igrib-2026": args.trusted_key}
        destination, rollback = installer.install(
            args.package.resolve(strict=True), work / "installed", trusted, True
        )
        if rollback is not None:
            raise RuntimeError("fresh conformance install unexpectedly created rollback")
        report["checks"]["signed_package_install"] = "passed"

        suffix = ".exe" if sys.platform == "win32" else ""
        helper_root = destination / "helpers" / target
        decoder = helper_root / f"igrib-environment-helper{suffix}"
        generator = helper_root / f"environmental-grib{suffix}"
        if not decoder.is_file():
            raise RuntimeError(f"required decoder helper is absent for {target}")
        report["checks"]["target_helper_selection"] = "passed"

        malformed = work / "malformed.grb"
        malformed.write_bytes(b"not a GRIB stream")
        malformed_result = work / "malformed.json"
        _, malformed_ms = run(
            [decoder, "inspect", malformed, malformed_result], expected=1
        )
        failure = json.loads(malformed_result.read_text())
        if failure.get("error", {}).get("code") != "environment-decode-failed":
            raise RuntimeError("malformed GRIB did not produce a structured error")
        report["checks"]["malformed_input_containment"] = {
            "status": "passed", "elapsed_ms": malformed_ms
        }

        if generator.is_file():
            capabilities, capabilities_ms = run([generator, "capabilities"])
            capability_document = json.loads(capabilities.stdout)
            if capability_document.get("schemaVersion") != 1:
                raise RuntimeError("generator helper protocol is incompatible")
            report["checks"]["generator_protocol"] = {
                "status": "passed", "elapsed_ms": capabilities_ms
            }
        else:
            report["checks"]["generator_protocol"] = "skipped-helper-optional"

        if args.fixture is not None:
            fixture = args.fixture.resolve(strict=True)
            metadata_path = work / "metadata.json"
            _, inspect_ms = run([decoder, "inspect", fixture, metadata_path], 120)
            metadata = json.loads(metadata_path.read_text())
            times = metadata.get("times", [])
            if not times or metadata.get("messageCount", 0) < 1:
                raise RuntimeError("fixture metadata did not contain messages/times")
            frame_path = work / "frame.json"
            _, frame_ms = run(
                [decoder, "frame", fixture, times[0], "1500", frame_path], 120
            )
            frame = json.loads(frame_path.read_text())
            if frame.get("sampleCount", 0) < 1:
                raise RuntimeError("fixture frame contained no supported samples")
            report["checks"]["real_grib_decode"] = {
                "status": "passed",
                "inspect_ms": inspect_ms,
                "frame_ms": frame_ms,
                "bytes": metadata.get("byteCount"),
                "messages": metadata.get("messageCount"),
                "times": len(times),
                "samples": frame.get("sampleCount"),
            }

            if args.full_generator:
                if not generator.is_file():
                    raise RuntimeError("--full-generator requires a target helper")
                generated = work / "generated.grb"
                result_path = work / "generator-result.json"
                job_path = work / "generator-job.json"
                job = {
                    "schemaVersion": 1,
                    "operation": "generateEnvironment",
                    "request": {
                        "bbox": {"west": -8.5, "south": 50.5,
                                 "east": -2.5, "north": 56.5},
                        "start": "2026-07-12T21:00:00Z",
                        "hours": 3,
                        "stepHours": 3,
                        "weatherProvider": "existing-file",
                        "weatherPreset": "viewer",
                        "weatherFile": str(fixture),
                        "includeWaves": False,
                        "currentSource": "none",
                        "output": str(generated),
                        "overwrite": True,
                    },
                }
                job_path.write_text(json.dumps(job))
                _, generator_ms = run(
                    [generator, "run-job", "--job", job_path,
                     "--result", result_path], 300
                )
                result = json.loads(result_path.read_text())
                if result.get("status") != "complete" or not generated.is_file():
                    raise RuntimeError("generator did not publish a complete GRIB")
                report["checks"]["real_generator_output"] = {
                    "status": "passed",
                    "elapsed_ms": generator_ms,
                    "bytes": generated.stat().st_size,
                }
            else:
                report["checks"]["real_generator_output"] = "skipped"
        else:
            report["checks"]["real_grib_decode"] = "skipped-no-fixture"
            report["checks"]["real_generator_output"] = "skipped-no-fixture"

    report["status"] = "passed"
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output_json:
        args.output_json.write_text(rendered)
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"conformance failed: {error}", file=sys.stderr)
        raise SystemExit(1)
