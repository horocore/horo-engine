"""Standard-library-only parity runner for real C++ host translations.

CTest invokes this module with site packages disabled so reference-host parity
remains available when repository pytest tests are not installed.
"""

from __future__ import annotations

import importlib.util
import json
import sys
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "horo_errors", Path(__file__).resolve().parents[2] / "scripts" / "horo_errors.py"
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("Cannot load the Python application-error adapter")
errors = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(errors)

EXCEPTIONS = {
    2: errors.HoroUsageError,
    3: errors.HoroValidationError,
    4: errors.HoroCapabilityError,
    5: errors.HoroOperationError,
    6: errors.HoroPermissionError,
    7: errors.HoroCancelledError,
    8: errors.HoroTimeoutError,
    10: errors.HoroInvariantError,
}


def verify_cpp_parity(cases: list[dict]) -> None:
    """Check actual C++ output with assertions that remain active under optimisation."""
    checks = unittest.TestCase()
    checks.assertEqual(len(cases), len(EXCEPTIONS))
    for case in cases:
        cli = case["cli"]
        with checks.assertRaises(EXCEPTIONS[cli["exitCode"]]) as caught:
            errors.raise_application_error(cli)
        failure = caught.exception
        checks.assertEqual(failure.payload, case["gui"])
        checks.assertEqual(case["gui"], case["mcp"]["data"])
        checks.assertEqual(case["mcp"]["data"], cli["error"])
        checks.assertEqual(failure.cause["code"], "project.translation.operation")
        checks.assertEqual(failure.metadata, {})
        checks.assertEqual(len(failure.diagnostics), 2)
        checks.assertEqual(failure.diagnostics[0]["location"]["source"], "")
        checks.assertNotIn("Private", str(failure))
        checks.assertEqual(failure.severity, cli["error"]["severity"])


if __name__ == "__main__":
    verify_cpp_parity(json.load(sys.stdin))
