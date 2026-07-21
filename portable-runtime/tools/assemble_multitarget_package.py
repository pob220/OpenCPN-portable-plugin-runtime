#!/usr/bin/env python3
"""Merge verified per-target helper packages and sign one deterministic archive."""

import argparse
import importlib.util
import json
import pathlib
import re
import stat
import subprocess
import sys
import tempfile
import zipfile


TOOLS = pathlib.Path(__file__).resolve().parent
TARGET = re.compile(
    r"^(linux-gnu-(?:x86_64|aarch64)|flatpak-(?:x86_64|aarch64)|macos-(?:x86_64|aarch64)|windows-x86_64)$"
)


def load_installer():
    spec = importlib.util.spec_from_file_location(
        "install_package", TOOLS / "install_package.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def trusted_keys(values, parser):
    result = {}
    for value in values:
        if "=" not in value:
            parser.error("--trusted-key must be KEY_ID=PUBLIC_KEY_PEM")
        key_id, filename = value.split("=", 1)
        result[key_id] = pathlib.Path(filename).resolve(strict=True)
    return result


def archive_payload(path, installer, trust):
    manifest, entries = installer.validate(path, trust, developer=True)
    payload = {}
    modes = {}
    with zipfile.ZipFile(path) as archive:
        for name, info in entries.items():
            if name in {"checksums.sha256", "signature.json"}:
                continue
            payload[name] = archive.read(info)
            modes[name] = info.external_attr >> 16
    return manifest, payload, modes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", action="append", required=True, type=pathlib.Path)
    parser.add_argument(
        "--helper-root",
        action="append",
        default=[],
        metavar="TARGET=DIRECTORY",
        help="add a CI-built helpers/TARGET payload directory",
    )
    parser.add_argument("--trusted-key", action="append", default=[])
    parser.add_argument("--require-target", action="append", default=[])
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--signing-key", required=True, type=pathlib.Path)
    parser.add_argument("--key-id", required=True)
    parser.add_argument("--report-json", type=pathlib.Path)
    args = parser.parse_args()

    trust = trusted_keys(args.trusted_key, parser)
    if not trust:
        parser.error("at least one --trusted-key is required")
    installer = load_installer()
    required = set(args.require_target)
    invalid = sorted(target for target in required if not TARGET.fullmatch(target))
    if invalid:
        parser.error(f"unsupported required target(s): {', '.join(invalid)}")

    canonical = None
    canonical_modes = None
    helpers = {}
    helper_modes = {}
    manifest = None
    source_digests = []
    try:
        import hashlib

        for package in args.package:
            package = package.resolve(strict=True)
            candidate_manifest, payload, modes = archive_payload(
                package, installer, trust
            )
            source_digests.append(
                {"file": package.name, "sha256": hashlib.sha256(package.read_bytes()).hexdigest()}
            )
            ordinary = {
                name: data for name, data in payload.items() if not name.startswith("helpers/")
            }
            ordinary_modes = {
                name: mode for name, mode in modes.items() if not name.startswith("helpers/")
            }
            if canonical is None:
                canonical = ordinary
                canonical_modes = ordinary_modes
                manifest = candidate_manifest
            elif ordinary != canonical or ordinary_modes != canonical_modes:
                raise ValueError(
                    f"portable payload differs in target package {package.name}"
                )
            for name, data in payload.items():
                if not name.startswith("helpers/"):
                    continue
                parts = pathlib.PurePosixPath(name).parts
                if len(parts) < 3 or not TARGET.fullmatch(parts[1]):
                    raise ValueError(f"invalid target-qualified helper path: {name}")
                if name in helpers and helpers[name] != data:
                    raise ValueError(f"conflicting duplicate helper: {name}")
                helpers[name] = data
                helper_modes[name] = modes[name]
        if canonical is None or manifest is None:
            raise ValueError("no input package was supplied")
        for value in args.helper_root:
            if "=" not in value:
                raise ValueError("--helper-root must be TARGET=DIRECTORY")
            target, directory_name = value.split("=", 1)
            if not TARGET.fullmatch(target):
                raise ValueError(f"unsupported helper target: {target}")
            directory = pathlib.Path(directory_name).resolve(strict=True)
            if not directory.is_dir():
                raise ValueError(f"helper root is not a directory: {directory}")
            suffix = ".exe" if target == "windows-x86_64" else ""
            if not (directory / f"igrib-environment-helper{suffix}").is_file():
                raise ValueError(f"decoder helper is absent from {target}")
            if not (directory / f"environmental-grib{suffix}").is_file():
                raise ValueError(f"generator helper is absent from {target}")
            for source in sorted(directory.rglob("*")):
                if source.is_dir() and not source.is_symlink():
                    continue
                payload_source = source
                if source.is_symlink():
                    try:
                        payload_source = source.resolve(strict=True)
                        payload_source.relative_to(directory)
                    except (OSError, ValueError):
                        raise ValueError(
                            f"helper symlink escapes its target root: {source}"
                        )
                    if not payload_source.is_file():
                        raise ValueError(
                            f"helper symlink does not name a regular file: {source}"
                        )
                elif not source.is_file():
                    raise ValueError(
                        f"helper payload is not a regular file: {source}"
                    )
                relative = source.relative_to(directory).as_posix()
                name = f"helpers/{target}/{relative}"
                data = payload_source.read_bytes()
                if name in helpers and helpers[name] != data:
                    raise ValueError(f"conflicting duplicate helper: {name}")
                helpers[name] = data
                helper_modes[name] = payload_source.stat().st_mode
        available = {pathlib.PurePosixPath(name).parts[1] for name in helpers}
        missing = required - available
        if missing:
            raise ValueError(f"required helper target(s) absent: {', '.join(sorted(missing))}")
        declared = set()
        for declaration in manifest.get("helpers", {}).values():
            declared.update(declaration.get("targets", []))
        undeclared = available - declared
        if undeclared:
            raise ValueError(f"helper target(s) not declared by manifest: {', '.join(sorted(undeclared))}")

        with tempfile.TemporaryDirectory(prefix="ocpnp-multitarget-") as temporary:
            root = pathlib.Path(temporary) / "package"
            for name, data in {**canonical, **helpers}.items():
                output = root.joinpath(*pathlib.PurePosixPath(name).parts)
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(data)
                mode = ({**canonical_modes, **helper_modes})[name]
                output.chmod(0o755 if mode & stat.S_IXUSR else 0o644)
            subprocess.run(
                [
                    sys.executable,
                    str(TOOLS / "build_package.py"),
                    "--root",
                    str(root),
                    "--output",
                    str(args.output),
                    "--signing-key",
                    str(args.signing_key.resolve(strict=True)),
                    "--key-id",
                    args.key_id,
                ],
                check=True,
            )
        report = {
            "schema_version": 1,
            "package_id": manifest["id"],
            "version": manifest.get("version"),
            "targets": sorted(available),
            "required_targets": sorted(required),
            "sources": source_digests,
            "output": str(args.output),
        }
        if args.report_json:
            args.report_json.parent.mkdir(parents=True, exist_ok=True)
            args.report_json.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, sort_keys=True))
    except (OSError, ValueError, zipfile.BadZipFile, installer.PackageError) as error:
        parser.exit(1, f"multi-target assembly failed: {error}\n")


if __name__ == "__main__":
    main()
