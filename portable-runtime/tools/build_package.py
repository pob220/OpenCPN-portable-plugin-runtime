#!/usr/bin/env python3
"""Create a deterministic development .ocpnp archive."""

import argparse
import hashlib
import pathlib
import stat
import zipfile


FIXED_TIMESTAMP = (2026, 1, 1, 0, 0, 0)


def package_files(root):
    files = []
    for path in root.rglob("*"):
        mode = path.lstat().st_mode
        relative = path.relative_to(root)
        if stat.S_ISLNK(mode):
            raise ValueError(f"package contains a symbolic link: {relative}")
        if stat.S_ISDIR(mode):
            continue
        if not stat.S_ISREG(mode):
            raise ValueError(f"package contains a non-regular file: {relative}")
        if path.name not in {"checksums.sha256", ".assembled"}:
            files.append(path)
    return sorted(files)


def checksum_document(root, files):
    lines = []
    for path in files:
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        lines.append(f"{digest}  {path.relative_to(root).as_posix()}\n")
    return "".join(lines).encode("ascii")


def add_bytes(archive, name, data):
    info = zipfile.ZipInfo(name, FIXED_TIMESTAMP)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o100644 << 16
    info.create_system = 3
    archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()

    root = args.root.resolve(strict=True)
    if not (root / "manifest.json").is_file():
        parser.error("package root has no manifest.json")
    try:
        files = package_files(root)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    checksums = checksum_document(root, files)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    with zipfile.ZipFile(temporary, "w", allowZip64=True) as archive:
        for path in files:
            add_bytes(archive, path.relative_to(root).as_posix(), path.read_bytes())
        add_bytes(archive, "checksums.sha256", checksums)
    temporary.replace(args.output)
    print(args.output)


if __name__ == "__main__":
    main()
