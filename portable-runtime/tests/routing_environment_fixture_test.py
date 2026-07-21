#!/usr/bin/env python3
"""Validate the real decoder contract consumed by iWeatherRouting.

The component-level routing fixture lives in bridge_smoke.cpp.  This companion
fixture uses actual GRIB2 messages and the packaged decoder, ensuring the
environment.provider@0.1 inputs used by the route engine are temporally
interpolated, physically bounded, marine-tagged and held in an immutable
private snapshot.
"""

import json
import pathlib
import shutil
import subprocess
import sys
import tempfile


def run(*arguments):
    return subprocess.run(
        [str(argument) for argument in arguments],
        check=True,
        capture_output=True,
        text=True,
    )


def write_message(grib_set, template, output, keys, value):
    run(grib_set, "-d", str(value), "-s", keys, template, output)
    return output.read_bytes()


def main():
    if len(sys.argv) != 2:
        raise RuntimeError("usage: routing_environment_fixture_test.py DECODER")
    decoder = pathlib.Path(sys.argv[1]).resolve(strict=True)
    grib_set = shutil.which("grib_set")
    codes_info = shutil.which("codes_info")
    if not grib_set or not codes_info:
        raise RuntimeError("ecCodes command-line tools are unavailable")
    samples = pathlib.Path(run(codes_info, "-s").stdout.strip())
    template = samples / "regular_ll_sfc_grib2.tmpl"
    if not template.is_file():
        raise RuntimeError(f"ecCodes sample is absent: {template}")

    definitions = [
        ("shortName=10u,typeOfLevel=heightAboveGround,level=10", 5.0, 7.0,
         "wind-u", False),
        ("shortName=10v,typeOfLevel=heightAboveGround,level=10", 2.0, 4.0,
         "wind-v", False),
        ("discipline=10,parameterCategory=1,parameterNumber=2,"
         "typeOfLevel=surface,level=0", 0.4, 0.8, "current-u", True),
        ("discipline=10,parameterCategory=1,parameterNumber=3,"
         "typeOfLevel=surface,level=0", 0.1, 0.3, "current-v", True),
        ("shortName=swh,typeOfLevel=surface,level=0", 1.0, 2.0,
         "wave-height", True),
        ("shortName=perpw,typeOfLevel=surface,level=0", 5.0, 7.0,
         "wave-period", True),
        ("shortName=dirpw,typeOfLevel=surface,level=0", 80.0, 100.0,
         "wave-direction", True),
    ]

    with tempfile.TemporaryDirectory(prefix="iwr-environment-fixture-") as temp:
        root = pathlib.Path(temp)
        messages = []
        for index, (keys, start_value, end_value, _field, _marine) in enumerate(
            definitions
        ):
            start = root / f"{index}-start.grb"
            end = root / f"{index}-end.grb"
            messages.append(write_message(
                grib_set, template, start,
                f"{keys},dataDate=20260721,dataTime=0,stepRange=0",
                start_value,
            ))
            messages.append(write_message(
                grib_set, template, end,
                f"{keys},dataDate=20260721,dataTime=0,stepRange=2",
                end_value,
            ))
        original = root / "route-source.grb"
        original.write_bytes(b"".join(messages))
        snapshot = root / "dataset-immutable.grb"
        shutil.copyfile(original, snapshot)
        snapshot.chmod(0o400)

        metadata = json.loads(run(decoder, "inspect", snapshot).stdout)
        if metadata.get("times") != ["20260721T0000Z", "20260721T0200Z"]:
            raise RuntimeError("route fixture forecast timeline is incorrect")
        frame = json.loads(
            run(decoder, "frame", snapshot, "20260721T0100Z", "100000").stdout
        )
        fields = {field["fieldId"]: field for field in frame.get("fields", [])}
        for _keys, start_value, end_value, field_id, marine in definitions:
            field = fields.get(field_id)
            if not field or not field.get("samples"):
                raise RuntimeError(f"route field is unavailable: {field_id}")
            if bool(field.get("marine")) != marine:
                raise RuntimeError(f"route marine classification is wrong: {field_id}")
            actual = float(field["samples"][0][2])
            expected = (start_value + end_value) / 2.0
            if abs(actual - expected) > 0.02:
                raise RuntimeError(
                    f"route interpolation mismatch for {field_id}: {actual}"
                )
        for field_id in ("current-u", "current-v"):
            if max(abs(float(row[2])) for row in fields[field_id]["samples"]) >= 12:
                raise RuntimeError("route fixture admitted impossible current data")

        # The consumer holds the private copy. Replacing the user source must
        # not alter the selected route dataset or its timeline.
        original.write_bytes(b"not a grib")
        held = json.loads(
            run(decoder, "frame", snapshot, "20260721T0100Z", "100000").stdout
        )
        if held.get("time") != frame.get("time") or not held.get("fields"):
            raise RuntimeError("held environmental dataset was not immutable")

    print("iWeatherRouting real environmental fixture passed")


if __name__ == "__main__":
    main()
