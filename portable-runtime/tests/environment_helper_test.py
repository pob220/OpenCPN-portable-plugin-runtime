#!/usr/bin/env python3
"""Exercise the portable environmental decoder's complete public field set."""

import json
import hashlib
import pathlib
import shutil
import subprocess
import sys
import tempfile


EXPECTED_GROUPS = {
    "wind",
    "wind-gust",
    "pressure",
    "wave",
    "current",
    "precipitation",
    "cloud",
    "air-temperature",
    "sea-temperature",
    "cape",
    "composite-reflectivity",
    "relative-humidity",
    "geopotential-height",
}


def run(*arguments):
    return subprocess.run(
        [str(argument) for argument in arguments],
        check=True,
        capture_output=True,
        text=True,
    )


def main():
    if len(sys.argv) not in {2, 3}:
        raise RuntimeError(
            "usage: environment_helper_test.py DECODER [GENERATOR]"
        )
    helper = pathlib.Path(sys.argv[1]).resolve(strict=True)
    generator = (
        pathlib.Path(sys.argv[2]).resolve(strict=True)
        if len(sys.argv) == 3 else None
    )
    grib_set = shutil.which("grib_set")
    codes_info = shutil.which("codes_info")
    if not grib_set or not codes_info:
        raise RuntimeError("ecCodes command-line tools are unavailable")
    samples = pathlib.Path(run(codes_info, "-s").stdout.strip())
    template = samples / "regular_ll_sfc_grib2.tmpl"
    if not template.is_file():
        raise RuntimeError(f"ecCodes sample is absent: {template}")

    named_fields = [
        ("10u", "heightAboveGround", 10),
        ("10v", "heightAboveGround", 10),
        ("gust", "heightAboveGround", 10),
        ("msl", "meanSea", 0),
        ("swh", "surface", 0),
        ("perpw", "surface", 0),
        ("dirpw", "surface", 0),
        ("tp", "surface", 0),
        ("tcc", "entireAtmosphere", 0),
        ("2t", "heightAboveGround", 2),
        ("sst", "surface", 0),
        ("cape", "surface", 0),
        ("r", "isobaricInhPa", 850),
        ("gh", "isobaricInhPa", 850),
        ("u", "isobaricInhPa", 850),
        ("v", "isobaricInhPa", 850),
        ("t", "isobaricInhPa", 850),
    ]
    numbered_fields = [
        (10, 1, 2, "current-u"),
        (10, 1, 3, "current-v"),
        (0, 16, 196, "reflectivity"),
    ]
    with tempfile.TemporaryDirectory(prefix="igrib-field-test-") as temporary:
        root = pathlib.Path(temporary)
        messages = []
        common = "dataDate=20260721,dataTime=0,stepRange=0"
        for index, (short_name, level_type, level) in enumerate(named_fields):
            output = root / f"named-{index}.grb"
            run(
                grib_set,
                "-s",
                f"shortName={short_name},typeOfLevel={level_type},"
                f"level={level},{common}",
                template,
                output,
            )
            messages.append(output.read_bytes())
        for discipline, category, number, name in numbered_fields:
            output = root / f"numbered-{name}.grb"
            run(
                grib_set,
                "-s",
                f"discipline={discipline},parameterCategory={category},"
                f"parameterNumber={number},typeOfLevel=surface,level=0,{common}",
                template,
                output,
            )
            messages.append(output.read_bytes())
        interpolated_start = root / "gust-zero.grb"
        run(
            grib_set,
            "-d",
            "0",
            "-s",
            "shortName=gust,typeOfLevel=heightAboveGround,level=10,"
            "dataDate=20260721,dataTime=0,stepRange=0",
            template,
            interpolated_start,
        )
        messages.append(interpolated_start.read_bytes())
        interpolated_endpoint = root / "gust-plus-two.grb"
        run(
            grib_set,
            "-d",
            "20",
            "-s",
            "shortName=gust,typeOfLevel=heightAboveGround,level=10,"
            "dataDate=20260721,dataTime=0,stepRange=2",
            template,
            interpolated_endpoint,
        )
        messages.append(interpolated_endpoint.read_bytes())
        fixture = root / "all-fields.grb"
        # Appending a duplicate field models overlapping files.  The decoder
        # contract is deterministic last-message-wins, not duplicate samples.
        fixture.write_bytes(b"".join(messages + [messages[0]]))

        metadata = json.loads(run(helper, "inspect", fixture).stdout)
        if metadata.get("schemaVersion") != 2:
            raise RuntimeError("decoder did not publish metadata schema 2")
        groups = {field.get("group") for field in metadata.get("fields", [])}
        if groups != EXPECTED_GROUPS:
            raise RuntimeError(
                f"field group mismatch; missing={EXPECTED_GROUPS - groups}, "
                f"unexpected={groups - EXPECTED_GROUPS}"
            )
        ids = {field.get("fieldId") for field in metadata["fields"]}
        for expected in {
            "wind-u@850hpa",
            "wind-v@850hpa",
            "air-temperature@850hpa",
            "relative-humidity@850hpa",
            "geopotential-height@850hpa",
        }:
            if expected not in ids:
                raise RuntimeError(f"pressure-level field is absent: {expected}")
        frame = json.loads(
            run(helper, "frame", fixture, "20260721T0000Z", "100000").stdout
        )
        if frame.get("schemaVersion") != 2 or not frame.get("fields"):
            raise RuntimeError("decoder did not publish a schema-2 field frame")
        if any("fieldId" not in field or "marine" not in field
               for field in frame["fields"]):
            raise RuntimeError("frame omitted stable field metadata")
        field_ids = [field["fieldId"] for field in frame["fields"]]
        if len(field_ids) != len(set(field_ids)):
            raise RuntimeError("overlapping files produced duplicate field frames")
        midpoint = json.loads(
            run(helper, "frame", fixture, "20260721T0100Z", "100000").stdout
        )
        gust = next(
            field for field in midpoint["fields"]
            if field["fieldId"] == "wind-gust"
        )
        if not gust.get("interpolated") or not gust.get("samples"):
            raise RuntimeError("temporal interpolation was not reported")
        if abs(float(gust["samples"][0][2]) - 10.0) > 0.01:
            raise RuntimeError("temporal interpolation produced the wrong value")
        table_result = root / "weather-table.json"
        run(helper, "table", fixture, "0", "0", table_result)
        table = json.loads(table_result.read_text())
        if table.get("schemaVersion") != 2 or not table.get("rows"):
            raise RuntimeError("decoder did not publish a schema-2 weather table")
        table_groups = {
            field.get("group")
            for field in table["rows"][0].get("fields", {}).values()
        }
        if table_groups != EXPECTED_GROUPS:
            raise RuntimeError("weather table omitted one or more field groups")

        if generator is not None:
            generated = root / "generated-environment.grb"
            job_path = root / "generate-job.json"
            result_path = root / "generate-result.json"
            job = {
                "schemaVersion": 1,
                "operation": "generateEnvironment",
                "request": {
                    "bbox": {
                        "west": -10.0, "south": 45.0,
                        "east": 5.0, "north": 60.0,
                    },
                    "start": "2026-07-21T00:00:00Z",
                    "hours": 0,
                    "stepHours": 1,
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
            run(generator, "run-job", "--job", job_path,
                "--result", result_path)
            generation = json.loads(result_path.read_text())
            if generation.get("status") != "complete" or not generated.is_file():
                raise RuntimeError("local generation did not publish a GRIB")
            external_count = int(run("codes_count", generated).stdout.strip())
            if external_count < 1:
                raise RuntimeError("ecCodes independently rejected generated GRIB")
            strict_metadata = json.loads(run(helper, "inspect", generated).stdout)
            if strict_metadata.get("messageCount") != external_count:
                raise RuntimeError("independent generated-GRIB counts disagree")
            run("grib_ls", "-p", "shortName,validityDate,validityTime",
                generated)

            # Transactional publication is a service invariant: a malformed
            # replacement request must leave the already validated file byte
            # for byte unchanged.
            valid_digest = hashlib.sha256(generated.read_bytes()).hexdigest()
            malformed = root / "malformed.grb"
            malformed.write_bytes(b"not-a-grib")
            job["request"]["weatherFile"] = str(malformed)
            job_path.write_text(json.dumps(job))
            failed = subprocess.run(
                [str(generator), "run-job", "--job", str(job_path),
                 "--result", str(result_path)],
                capture_output=True, text=True,
            )
            if failed.returncode == 0:
                raise RuntimeError("malformed generation unexpectedly succeeded")
            if hashlib.sha256(generated.read_bytes()).hexdigest() != valid_digest:
                raise RuntimeError("failed generation replaced a valid dataset")
    print("portable environmental helper field test passed")


if __name__ == "__main__":
    main()
