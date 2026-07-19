#!/usr/bin/env python3
"""Create a deterministic .ocpnp archive with optional Ed25519 signing."""

import argparse
import base64
import hashlib
import json
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


def add_bytes(archive, name, data, mode=0o100644):
    info = zipfile.ZipInfo(name, FIXED_TIMESTAMP)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = mode << 16
    info.create_system = 3
    archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--signing-key", type=pathlib.Path)
    parser.add_argument("--key-id", default="")
    args = parser.parse_args()

    root = args.root.resolve(strict=True)
    if not (root / "manifest.json").is_file():
        parser.error("package root has no manifest.json")
    try:
        files = package_files(root)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    checksums = checksum_document(root, files)
    signature = None
    if args.signing_key:
        if not args.key_id:
            parser.error("--key-id is required with --signing-key")
        try:
            from cryptography.hazmat.primitives import serialization
        except ImportError as error:
            parser.error(f"Ed25519 signing requires python cryptography: {error}")
        private_key = serialization.load_pem_private_key(
            args.signing_key.read_bytes(), password=None
        )
        signature = json.dumps(
            {
                "algorithm": "Ed25519",
                "key_id": args.key_id,
                "signed": "checksums.sha256",
                "signature": base64.b64encode(private_key.sign(checksums)).decode(
                    "ascii"
                ),
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode("ascii") + b"\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    with zipfile.ZipFile(temporary, "w", allowZip64=True) as archive:
        for path in files:
            mode = 0o100755 if path.stat().st_mode & 0o111 else 0o100644
            add_bytes(
                archive,
                path.relative_to(root).as_posix(),
                path.read_bytes(),
                mode,
            )
        add_bytes(archive, "checksums.sha256", checksums)
        if signature is not None:
            add_bytes(archive, "signature.json", signature)
    temporary.replace(args.output)
    print(args.output)


if __name__ == "__main__":
    main()
