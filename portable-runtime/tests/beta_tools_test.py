#!/usr/bin/env python3

import importlib.util
import pathlib
import subprocess
import sys
import unittest


RUNTIME = pathlib.Path(__file__).resolve().parents[1]
BETA = RUNTIME / "beta"


def load(name):
    spec = importlib.util.spec_from_file_location(name, BETA / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


profile = load("configure_profile")
diagnostics = load("collect_diagnostics")


class BetaToolsTest(unittest.TestCase):
    def test_profile_section_is_added_and_idempotent(self):
        initial = ["[Settings]\n", "OpenGL=0\n"]
        first = profile.update(initial)
        second = profile.update(first)
        self.assertEqual(first, second)
        rendered = "".join(first)
        self.assertIn("[PortablePlugins]\n", rendered)
        self.assertIn("EnableExperimental=1\n", rendered)
        self.assertIn("DeveloperMode=1\n", rendered)
        self.assertIn("DeveloperStartupAction=igrib.toggle\n", rendered)

    def test_profile_update_preserves_unrelated_values(self):
        initial = [
            "[PortablePlugins]\n",
            "EnableExperimental=0\n",
            "Unrelated=preserved\n",
            "DeveloperMode=0\n",
            "[Next]\n",
            "Value=1\n",
        ]
        rendered = "".join(profile.update(initial))
        self.assertEqual(rendered.count("EnableExperimental="), 1)
        self.assertEqual(rendered.count("DeveloperMode="), 1)
        self.assertIn("Unrelated=preserved\n", rendered)
        self.assertIn("[Next]\nValue=1\n", rendered)

    def test_diagnostic_redaction(self):
        value = "token=abc password: xyz authorization = Bearer-secret"
        redacted = diagnostics.SECRET.sub(r"\1\2<redacted>", value)
        self.assertNotIn("abc", redacted)
        self.assertNotIn("xyz", redacted)
        self.assertNotIn("Bearer-secret", redacted)

    def test_shell_entry_points_have_working_help(self):
        for script in (
            "build-linux.sh",
            "debian-prerequisites.sh",
            "launch-linux.sh",
            "run-conformance.sh",
        ):
            result = subprocess.run(
                [str(BETA / script), "--help"],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Usage:", result.stdout)


if __name__ == "__main__":
    unittest.main()
