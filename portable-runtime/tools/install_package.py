#!/usr/bin/env python3
"""Validate and install an unsigned development .ocpnp package."""

import argparse
import hashlib
import json
import pathlib
import re
import shutil
import stat
import tempfile
import unicodedata
import zipfile


MAX_ARCHIVE_BYTES = 256 * 1024 * 1024
MAX_UNCOMPRESSED_BYTES = 512 * 1024 * 1024
MAX_ENTRIES = 2048
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_RATIO = 200
SAFE_ID = re.compile(r"^[a-z0-9]+(?:[.-][a-z0-9]+)+$")
WINDOWS_DEVICE_NAMES = {
    "CON",
    "PRN",
    "AUX",
    "NUL",
    *(f"COM{number}" for number in range(1, 10)),
    *(f"LPT{number}" for number in range(1, 10)),
}


class PackageError(RuntimeError):
    pass


def checked_name(raw_name):
    if (
        "\\" in raw_name
        or "\0" in raw_name
        or any(ord(character) < 32 or ord(character) == 127 for character in raw_name)
    ):
        raise PackageError(f"archive-policy-violation: invalid path {raw_name!r}")
    name = unicodedata.normalize("NFC", raw_name)
    path = pathlib.PurePosixPath(name)
    if name != raw_name or path.is_absolute() or not path.parts:
        raise PackageError(f"archive-policy-violation: invalid path {raw_name!r}")
    if any(part in {"", ".", ".."} for part in path.parts):
        raise PackageError(f"archive-policy-violation: invalid path {raw_name!r}")
    for part in path.parts:
        if part.endswith((" ", ".")) or ":" in part:
            raise PackageError(f"archive-policy-violation: invalid path {raw_name!r}")
        device_stem = part.split(".", 1)[0].upper()
        if device_stem in WINDOWS_DEVICE_NAMES:
            raise PackageError(f"archive-policy-violation: invalid path {raw_name!r}")
    return path


def unique_json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise PackageError(f"manifest-invalid: duplicate JSON key {key!r}")
        result[key] = value
    return result


def regular_entries(archive):
    infos = archive.infolist()
    if len(infos) > MAX_ENTRIES:
        raise PackageError("archive-policy-violation: too many entries")
    names = set()
    result = {}
    total = 0
    for info in infos:
        path = checked_name(info.filename.rstrip("/"))
        folded = path.as_posix().casefold()
        if folded in names:
            raise PackageError(f"archive-policy-violation: duplicate path {path}")
        names.add(folded)
        mode = info.external_attr >> 16
        file_type = stat.S_IFMT(mode)
        if file_type not in {0, stat.S_IFREG, stat.S_IFDIR}:
            raise PackageError(f"archive-policy-violation: non-regular entry {path}")
        if info.is_dir():
            continue
        total += info.file_size
        if total > MAX_UNCOMPRESSED_BYTES:
            raise PackageError("archive-policy-violation: package is too large")
        if info.compress_size == 0:
            if info.file_size != 0:
                raise PackageError("archive-policy-violation: invalid compression size")
        elif info.file_size / info.compress_size > MAX_RATIO:
            raise PackageError(f"archive-policy-violation: compression ratio for {path}")
        result[path.as_posix()] = info
    return result


def parse_checksums(data):
    expected = {}
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError as error:
        raise PackageError("digest-mismatch: checksums are not ASCII") from error
    for line in text.splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match:
            raise PackageError("digest-mismatch: malformed checksums.sha256")
        name = checked_name(match.group(2)).as_posix()
        if name in expected:
            raise PackageError(f"digest-mismatch: duplicate checksum for {name}")
        expected[name] = match.group(1)
    return expected


def validate(archive_path):
    if archive_path.stat().st_size > MAX_ARCHIVE_BYTES:
        raise PackageError("archive-policy-violation: archive is too large")
    with zipfile.ZipFile(archive_path, "r") as archive:
        entries = regular_entries(archive)
        required = {"manifest.json", "checksums.sha256"}
        if not required.issubset(entries):
            raise PackageError("manifest-invalid: required package files are missing")
        manifest_info = entries["manifest.json"]
        if manifest_info.file_size > MAX_MANIFEST_BYTES:
            raise PackageError("manifest-invalid: manifest is too large")
        try:
            manifest = json.loads(
                archive.read(manifest_info), object_pairs_hook=unique_json_object
            )
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise PackageError("manifest-invalid: malformed UTF-8 JSON") from error
        if not isinstance(manifest, dict):
            raise PackageError("manifest-invalid: manifest root must be an object")
        plugin_id = manifest.get("id", "")
        if (
            manifest.get("format_version") != 1
            or not isinstance(plugin_id, str)
            or not SAFE_ID.fullmatch(plugin_id)
        ):
            raise PackageError("manifest-invalid: unsupported format or plugin id")
        if not manifest.get("development"):
            raise PackageError("signature-untrusted: only development packages are supported")
        component_value = manifest.get("component", "")
        if not isinstance(component_value, str):
            raise PackageError("manifest-invalid: component must be a path string")
        component = checked_name(component_value).as_posix()
        if component not in entries:
            raise PackageError("component-invalid: declared component is missing")
        if not archive.read(entries[component]).startswith(b"\x00asm"):
            raise PackageError("component-invalid: entry is not WebAssembly")

        expected = parse_checksums(archive.read(entries["checksums.sha256"]))
        actual_names = set(entries) - {"checksums.sha256"}
        if set(expected) != actual_names:
            raise PackageError("digest-mismatch: checksum file list differs from archive")
        for name, digest in expected.items():
            actual = hashlib.sha256(archive.read(entries[name])).hexdigest()
            if actual != digest:
                raise PackageError(f"digest-mismatch: {name}")
        return manifest, entries


def install(archive_path, root):
    manifest, entries = validate(archive_path)
    destination = root / manifest["id"]
    if destination.exists():
        raise PackageError(f"already-installed: {destination}")
    root.mkdir(parents=True, exist_ok=True)
    staging = pathlib.Path(tempfile.mkdtemp(prefix=".ocpnp-install-", dir=root))
    try:
        with zipfile.ZipFile(archive_path, "r") as archive:
            for name, info in entries.items():
                if name == "checksums.sha256":
                    continue
                output = staging.joinpath(*pathlib.PurePosixPath(name).parts)
                output.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(info, "r") as source, output.open("xb") as target:
                    shutil.copyfileobj(source, target, length=1024 * 1024)
                output.chmod(0o444)
        staging.replace(destination)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return destination


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=pathlib.Path)
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--developer", action="store_true")
    args = parser.parse_args()
    if not args.developer:
        parser.error("unsigned development packages require --developer")
    try:
        destination = install(args.package.resolve(strict=True), args.root.resolve())
    except (OSError, zipfile.BadZipFile, PackageError) as error:
        parser.exit(1, f"install failed: {error}\n")
    print(destination)


if __name__ == "__main__":
    main()
