#!/usr/bin/env python3
"""Standalone OPP API 0.5 portable-plugin scaffold, linter and packager."""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile


FIXED_TIMESTAMP = (2026, 1, 1, 0, 0, 0)
SAFE_ID = re.compile(r"^[a-z0-9]+(?:[.-][a-z0-9]+)+$")
SAFE_VERSION = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$"
)
API_05 = ">=0.5.0 <0.6.0"
RUNTIME_01 = ">=0.1.0 <0.2.0"
WORLDS = {
    "plugin",
    "environment-provider-plugin",
    "weather-routing-plugin",
    "passage-weather-routing-plugin",
}
PERMISSIONS = {
    "ui.commands",
    "ui.surfaces",
    "navigation.position.read",
    "navigation.nmea.read",
    "navigation.nmea2000.read",
    "navigation.signalk.read",
    "navigation.ais.read",
    "navigation.active-leg.read",
    "navigation.objects.read",
    "navigation.objects.write",
    "navigation.nmea.write",
    "communications.outputs.read",
    "navigation.nmea2000.write",
    "chart.cursor.read",
    "chart.viewport.read",
    "chart.input.pointer",
    "chart.input.keyboard",
    "plugin.messages.receive",
    "plugin.messages.send",
    "plugin.rpc.request",
    "plugin.rpc.provide",
    "settings.read-write",
    "overlay.submit",
    "jobs.compute",
    "timers.schedule",
    "environment.datasets",
    "storage.user-selected",
    "network.providers",
    "helpers.environment.decode",
    "helpers.environment.generate",
    "charts.coverage",
    "charts.segment-safety",
    "network.http",
    "network.https",
    "storage.private",
    "credentials.provider",
    "weather-routing.compute",
    "environment.consume",
    "navigation.routes.write",
}
EVENT_PERMISSIONS = {
    "navigation.nmea0183": "navigation.nmea.read",
    "navigation.nmea2000": "navigation.nmea2000.read",
    "navigation.signalk": "navigation.signalk.read",
    "navigation.position": "navigation.position.read",
    "navigation.ais": "navigation.ais.read",
    "navigation.active-leg": "navigation.active-leg.read",
    "chart.cursor": "chart.cursor.read",
    "chart.viewport": "chart.viewport.read",
    "opencpn.plugin-message": "plugin.messages.receive",
    "host.environment": None,
}


class LintError(ValueError):
    pass


def load_json(path: pathlib.Path) -> dict:
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise LintError(f"duplicate JSON key: {key}")
            value[key] = item
        return value

    try:
        value = json.loads(path.read_text("utf-8"), object_pairs_hook=unique)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise LintError(f"cannot read manifest: {error}") from error
    if not isinstance(value, dict):
        raise LintError("manifest root must be an object")
    return value


def safe_relative(value: object, field: str) -> pathlib.PurePosixPath:
    if not isinstance(value, str) or not value or "\\" in value:
        raise LintError(f"{field} must be a non-empty POSIX relative path")
    path = pathlib.PurePosixPath(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise LintError(f"{field} escapes the package")
    return path


def lint_manifest(path: pathlib.Path, require_files: bool = True) -> dict:
    value = load_json(path)
    if value.get("format_version") != 1:
        raise LintError("format_version must be 1")
    if not isinstance(value.get("id"), str) or not SAFE_ID.fullmatch(value["id"]):
        raise LintError("id must be a lower-case reverse-domain identifier")
    if not isinstance(value.get("name"), str) or not value["name"].strip():
        raise LintError("name is required")
    if not isinstance(value.get("version"), str) or not SAFE_VERSION.fullmatch(
        value["version"]
    ):
        raise LintError("version must be semantic versioning")
    if value.get("runtime") != RUNTIME_01:
        raise LintError(f"runtime must be {RUNTIME_01!r}")
    if value.get("portable_api") != API_05:
        raise LintError(f"portable_api must be {API_05!r} for this SDK")
    if value.get("portable_world") not in WORLDS:
        raise LintError(
            "portable_world must select an OPP API 0.5 universal profile"
        )
    component = safe_relative(value.get("component"), "component")
    permissions = value.get("permissions")
    if not isinstance(permissions, list) or any(
        not isinstance(item, str) for item in permissions
    ):
        raise LintError("permissions must be an array of strings")
    if len(set(permissions)) != len(permissions):
        raise LintError("permissions contains duplicates")
    unknown = sorted(set(permissions) - PERMISSIONS)
    if unknown:
        raise LintError(f"unknown permissions: {', '.join(unknown)}")
    resources = value.get("resources", [])
    if not isinstance(resources, list):
        raise LintError("resources must be an array")
    declared_paths = [safe_relative(item, "resource") for item in resources]
    surfaces = value.get("surfaces", {})
    if not isinstance(surfaces, dict):
        raise LintError("surfaces must be an object")
    for surface_id, resource in surfaces.items():
        if not isinstance(surface_id, str) or not SAFE_ID.fullmatch(surface_id):
            raise LintError(f"invalid surface id: {surface_id!r}")
        declared_paths.append(safe_relative(resource, f"surface {surface_id}"))
    subscriptions = value.get("event_subscriptions", [])
    if not isinstance(subscriptions, list) or len(subscriptions) > 64:
        raise LintError("event_subscriptions must contain at most 64 entries")
    for index, subscription in enumerate(subscriptions):
        if not isinstance(subscription, dict):
            raise LintError(f"subscription {index} must be an object")
        event = subscription.get("event")
        if event not in EVENT_PERMISSIONS:
            raise LintError(f"subscription {index} has an unknown event")
        permission = EVENT_PERMISSIONS[event]
        if permission is not None and permission not in permissions:
            raise LintError(
                f"subscription {index} requires permission {permission}"
            )
        prefix = subscription.get("topic_prefix", "")
        limit = subscription.get("queue_limit", 0)
        if not isinstance(prefix, str) or len(prefix.encode("utf-8")) > 256:
            raise LintError(f"subscription {index} topic prefix is too long")
        if not isinstance(limit, int) or not 1 <= limit <= 1024:
            raise LintError(f"subscription {index} queue_limit must be 1..1024")
    domains = value.get("https_domains", [])
    if not isinstance(domains, list) or len(domains) > 32:
        raise LintError("https_domains must contain at most 32 entries")
    domain_pattern = re.compile(
        r"^(?=.{1,253}$)[a-z0-9](?:[a-z0-9-]*[a-z0-9])?"
        r"(?:\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)+$"
    )
    def valid_https_domain(domain: object) -> bool:
        if (
            not isinstance(domain, str)
            or not domain_pattern.fullmatch(domain)
            or not any(character.isalpha() for character in domain)
        ):
            return False
        try:
            ipaddress.ip_address(domain)
            return False
        except ValueError:
            return True

    if any(not valid_https_domain(domain) for domain in domains):
        raise LintError("https_domains contains an invalid exact domain")
    if len(set(domains)) != len(domains):
        raise LintError("https_domains contains duplicates")
    if domains and "network.https" not in permissions:
        raise LintError("https_domains requires network.https")
    if require_files:
        root = path.parent
        for relative in declared_paths:
            target = root.joinpath(*relative.parts)
            if not target.is_file() or target.is_symlink():
                raise LintError(f"declared resource is missing: {relative}")
        component_path = root.joinpath(*component.parts)
        if component_path.exists() and (
            not component_path.is_file()
            or component_path.is_symlink()
            or not component_path.read_bytes()[:4] == b"\0asm"
        ):
            raise LintError("declared component is not WebAssembly")
    return value


def package_files(root: pathlib.Path) -> list[pathlib.Path]:
    files = []
    for path in root.rglob("*"):
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode):
            raise LintError(f"symbolic links are forbidden: {path.relative_to(root)}")
        if stat.S_ISDIR(mode):
            continue
        if not stat.S_ISREG(mode):
            raise LintError(f"non-regular package entry: {path.relative_to(root)}")
        if path.name != "checksums.sha256":
            files.append(path)
    return sorted(files)


def add_bytes(archive: zipfile.ZipFile, name: str, data: bytes, mode: int) -> None:
    info = zipfile.ZipInfo(name, FIXED_TIMESTAMP)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = mode << 16
    info.create_system = 3
    archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def create_package(manifest_path: pathlib.Path, component: pathlib.Path,
                   output: pathlib.Path) -> None:
    manifest = lint_manifest(manifest_path, require_files=True)
    if not component.is_file() or component.read_bytes()[:4] != b"\0asm":
        raise LintError("--component is not a WebAssembly component")
    with tempfile.TemporaryDirectory(prefix="portable-plugin-package-") as temp:
        root = pathlib.Path(temp)
        for source in manifest_path.parent.rglob("*"):
            if source.is_dir():
                continue
            if source.is_symlink():
                raise LintError(f"symbolic links are forbidden: {source}")
            relative = source.relative_to(manifest_path.parent)
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        component_relative = safe_relative(manifest["component"], "component")
        component_target = root.joinpath(*component_relative.parts)
        component_target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(component, component_target)
        files = package_files(root)
        checksums = "".join(
            f"{hashlib.sha256(path.read_bytes()).hexdigest()}  "
            f"{path.relative_to(root).as_posix()}\n"
            for path in files
        ).encode("ascii")
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_suffix(output.suffix + ".tmp")
        with zipfile.ZipFile(temporary, "w", allowZip64=True) as archive:
            for path in files:
                mode = 0o100755 if path.stat().st_mode & 0o111 else 0o100644
                add_bytes(
                    archive,
                    path.relative_to(root).as_posix(),
                    path.read_bytes(),
                    mode,
                )
            add_bytes(archive, "checksums.sha256", checksums, 0o100644)
        temporary.replace(output)


def command_new(args: argparse.Namespace) -> None:
    if not SAFE_ID.fullmatch(args.id):
        raise LintError("--id must be a lower-case reverse-domain identifier")
    if not args.name.strip():
        raise LintError("--name must not be empty")
    template = pathlib.Path(__file__).resolve().parents[1] / "rust-template"
    destination = args.destination.resolve()
    if destination.exists():
        raise LintError(f"destination already exists: {destination}")
    shutil.copytree(template, destination, ignore=shutil.ignore_patterns("target"))
    manifest_path = destination / "package" / "manifest.json"
    manifest = load_json(manifest_path)
    manifest["id"] = args.id
    manifest["name"] = args.name
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", "utf-8")
    contracts = pathlib.Path(__file__).resolve().parents[2] / "contracts" / "0.5"
    shutil.copytree(contracts, destination / "contracts" / "0.5")
    source_path = destination / "src" / "lib.rs"
    source_path.write_text(
        source_path.read_text("utf-8").replace(
            'path: "../../contracts/0.5"', 'path: "contracts/0.5"'
        ),
        "utf-8",
    )
    print(destination)


def command_build(args: argparse.Namespace) -> None:
    subprocess.run(
        [
            "cargo",
            "build",
            "--locked",
            "--release",
            "--target",
            "wasm32-wasip2",
            "--manifest-path",
            str(args.manifest),
        ],
        check=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    new = commands.add_parser("new")
    new.add_argument("--id", required=True)
    new.add_argument("--name", required=True)
    new.add_argument("destination", type=pathlib.Path)
    lint = commands.add_parser("lint")
    lint.add_argument("manifest", type=pathlib.Path)
    build = commands.add_parser("build")
    build.add_argument("manifest", type=pathlib.Path)
    package = commands.add_parser("package")
    package.add_argument("--manifest", required=True, type=pathlib.Path)
    package.add_argument("--component", required=True, type=pathlib.Path)
    package.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        if args.command == "new":
            command_new(args)
        elif args.command == "lint":
            lint_manifest(args.manifest.resolve())
            print(f"OK: {args.manifest}")
        elif args.command == "build":
            command_build(args)
        else:
            create_package(
                args.manifest.resolve(),
                args.component.resolve(),
                args.output.resolve(),
            )
            print(args.output.resolve())
        return 0
    except (LintError, OSError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
