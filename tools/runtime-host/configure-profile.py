#!/usr/bin/env python3
"""Create or update the isolated OpenCPN profile for runtime-host testing."""

import argparse
import configparser
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("config", type=Path)
    args = parser.parse_args()

    config = configparser.RawConfigParser(strict=False)
    config.optionxform = str
    if args.config.exists():
        config.read(args.config, encoding="utf-8")

    if not config.has_section("Settings"):
        config.add_section("Settings")
    config.set("Settings", "ConfigVersionString", "Version 5.14.0")
    config.set("Settings", "ShowChartOutlines", "0")
    config.set("Settings", "OpenGL", "0")

    plugin_section = "PlugIns/libportable_plugin_manager_pi.so"
    if not config.has_section(plugin_section):
        config.add_section(plugin_section)
    config.set(plugin_section, "bEnabled", "1")

    args.config.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.config.with_suffix(args.config.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        config.write(stream, space_around_delimiters=False)
    temporary.replace(args.config)
    print(f"configured isolated profile: {args.config}")


if __name__ == "__main__":
    main()
