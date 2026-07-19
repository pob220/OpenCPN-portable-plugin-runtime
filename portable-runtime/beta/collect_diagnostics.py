#!/usr/bin/env python3
"""Collect a privacy-limited Linux beta diagnostic archive."""

import argparse
import datetime
import hashlib
import os
import pathlib
import platform
import re
import shutil
import subprocess
import tarfile


MODULES = (
    "eccodes",
    "jsoncpp",
    "netcdf",
    "libcurl",
    "qhull_r",
    "blosc",
    "libzip",
)
SECRET = re.compile(
    r"(?i)(authorization|password|passwd|secret|token|api[_-]?key)"
    r"(\s*[:=]\s*)([^\s&]+)"
)


def command(arguments, cwd=None):
    try:
        result = subprocess.run(
            arguments,
            cwd=cwd,
            text=True,
            capture_output=True,
            timeout=60,
        )
        output = result.stdout + result.stderr
        return f"$ {' '.join(arguments)}\nexit={result.returncode}\n{output}"
    except Exception as error:
        return f"$ {' '.join(arguments)}\nfailed: {error}\n"


def digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def write(path, content):
    path.write_text(content if content.endswith("\n") else content + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-root", type=pathlib.Path)
    parser.add_argument("--stage-root", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()

    repo = pathlib.Path(__file__).resolve().parents[2]
    build = (args.build_root or repo / "build-portable-beta").resolve()
    stage = (args.stage_root or build / "stage").resolve()
    opencpn_build = pathlib.Path(
        os.environ.get("OCPN_BETA_OPENCPN_BUILD_DIR", build / "opencpn")
    ).resolve()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y%m%dT%H%M%SZ"
    )
    output = (
        args.output or build / f"portable-beta-diagnostics-{stamp}.tar.gz"
    ).resolve()
    work = build / f".diagnostics-{stamp}"
    work.mkdir(parents=True, exist_ok=False)
    try:
        write(
            work / "system.txt",
            f"platform={platform.platform()}\n"
            f"machine={platform.machine()}\n"
            f"python={platform.python_version()}\n"
            + command(["cmake", "--version"])
            + command(["rustc", "--version"])
            + command(["cargo", "--version"])
            + command(["bwrap", "--version"])
            + command(["prlimit", "--version"]),
        )
        write(work / "git.txt", command(["git", "status", "--short"], repo))
        write(
            work / "submodules.txt",
            command(["git", "submodule", "status", "--recursive"], repo),
        )
        pkg_text = "".join(
            command(["pkg-config", "--modversion", module])
            for module in MODULES
        )
        write(work / "native-dependencies.txt", pkg_text)

        identity = stage / "BUILD-IDENTITY.txt"
        if identity.is_file():
            shutil.copyfile(identity, work / "BUILD-IDENTITY.txt")
        executable = stage / "app" / "bin" / "opencpn"
        if executable.is_file():
            write(work / "binary-sha256.txt", f"{digest(executable)}  opencpn")
            write(work / "opencpn-ldd.txt", command(["ldd", str(executable)]))
        package_root = (
            stage
            / "config"
            / "portable-plugins"
            / "org.opencpn.igrib"
        )
        target = (
            "linux-gnu-aarch64"
            if platform.machine().lower() in {"aarch64", "arm64"}
            else "linux-gnu-x86_64"
        )
        decoder = package_root / "helpers" / target / "igrib-environment-helper"
        generator = package_root / "helpers" / target / "environmental-grib"
        for name, binary in (("decoder", decoder), ("generator", generator)):
            if binary.is_file():
                write(work / f"{name}-ldd.txt", command(["ldd", str(binary)]))

        cache = opencpn_build / "CMakeCache.txt"
        if cache.is_file():
            prefixes = (
                "CMAKE_BUILD_TYPE:",
                "CMAKE_INSTALL_PREFIX:",
                "OCPN_ENABLE_PORTABLE_PLUGINS:",
                "OCPN_IGRIB_",
            )
            selected = [
                line
                for line in cache.read_text(errors="replace").splitlines()
                if line.startswith(prefixes)
            ]
            write(work / "cmake-settings.txt", "\n".join(selected))
        write(
            work / "portable-ctest.txt",
            command(
                [
                    "ctest",
                    "--test-dir",
                    str(opencpn_build),
                    "--output-on-failure",
                    "-R",
                    "^portable_",
                ]
            ),
        )

        log = stage / "config" / "opencpn.log"
        if log.is_file():
            tail = log.read_text(errors="replace").splitlines()[-1500:]
            redacted = "\n".join(
                SECRET.sub(r"\1\2<redacted>", line) for line in tail
            )
            write(work / "opencpn-log-tail-redacted.txt", redacted)

        write(
            work / "CONTENTS.txt",
            "This archive excludes OpenCPN configuration, routes, charts, "
            "GRIB files, credentials and plugin-private data. Review every "
            "file before sharing.",
        )
        output.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(output, "w:gz") as archive:
            archive.add(work, arcname="portable-beta-diagnostics")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(output)


if __name__ == "__main__":
    main()
