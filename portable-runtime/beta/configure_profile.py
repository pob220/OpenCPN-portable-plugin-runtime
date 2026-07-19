#!/usr/bin/env python3
"""Update only the experimental portable-plugin section in an INI file."""

import argparse
import os
import pathlib
import tempfile


VALUES = {
    "EnableExperimental": "1",
    "DeveloperMode": "1",
    "DeveloperStartupAction": "igrib.toggle",
}


def update(lines):
    output = []
    section_start = None
    section_end = None
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if section_start is not None and section_end is None:
                section_end = len(output)
            if stripped == "[PortablePlugins]":
                section_start = len(output)
        output.append(line)
    if section_start is not None and section_end is None:
        section_end = len(output)

    rendered = [f"{key}={value}\n" for key, value in VALUES.items()]
    if section_start is None:
        if output and output[-1].strip():
            output.append("\n")
        output.append("[PortablePlugins]\n")
        output.extend(rendered)
        return output

    body_start = section_start + 1
    kept = []
    for line in output[body_start:section_end]:
        key = line.split("=", 1)[0].strip()
        if key not in VALUES:
            kept.append(line)
    output[body_start:section_end] = rendered + kept
    return output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("config", type=pathlib.Path)
    args = parser.parse_args()
    path = args.config
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = path.read_text().splitlines(keepends=True) if path.exists() else []
    content = "".join(update(lines))
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
    temporary = pathlib.Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.chmod(0o600)
        temporary.replace(path)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise


if __name__ == "__main__":
    main()
