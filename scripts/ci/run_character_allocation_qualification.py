#!/usr/bin/env python3
"""Run the exact Character allocation probes unsupported by MSVC Debug iterator proxies."""

import argparse
import json
from pathlib import Path
import re
import sys
from defusedxml import ElementTree as ET
from defusedxml.common import DefusedXmlException

PREFIX = "HoroPhysicsTests::"
CASES = frozenset({PREFIX + "Character preparation allocation failures roll back every retained storage stage"})


def verify_discovery(document):
    """Require every exact case and refuse a disabled replacement for executable evidence."""
    tests = document.get("tests", [])
    families = {name.split(" - ", 1)[0] for name in CASES}
    selected = [test for test in tests if any(
        test.get("name") == family or test.get("name", "").startswith(family + " - ")
        for family in families
    )]
    names = [test["name"] for test in selected]
    if len(names) != len(CASES) or set(names) != CASES:
        raise ValueError(f"Character allocation discovery differs: missing={sorted(CASES - set(names))}")
    for test in selected:
        if any(prop.get("name") == "DISABLED" and prop.get("value") for prop in test.get("properties", [])):
            raise ValueError(f"Character allocation test is disabled: {test['name']}")


def verify_junit(path):
    """Count actual cases rather than assertions and reject every skipped or failed execution."""
    root = ET.parse(path).getroot()
    cases = list(root.iter("testcase"))
    names = [case.get("name") for case in cases]
    if len(cases) != len(CASES) or set(names) != CASES:
        raise ValueError(f"Character allocation execution differs: expected {len(CASES)}, got {len(cases)}")
    for case in cases:
        failed = any(case.find(tag) is not None for tag in ("failure", "error", "skipped"))
        if failed or case.get("status") in ("notrun", "disabled"):
            raise ValueError(f"Character allocation test did not pass: {case.get('name')}")
    for suite in root.iter("testsuite"):
        for field in ("failures", "errors", "skipped", "disabled"):
            if int(suite.get(field, "0")):
                raise ValueError(f"Character allocation suite has nonzero {field}")


def workspace_path(value):
    """Reject CLI traversal and symlink escapes before reading or writing workspace evidence."""
    root = Path.cwd().resolve()
    candidate = Path(value).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise argparse.ArgumentTypeError("Qualification evidence must remain inside the current workspace") from error
    return candidate


def main():
    """Validate discovery before execution, or validate the resulting exact Release JUnit."""
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--discovery", type=workspace_path)
    mode.add_argument("--output-junit", type=workspace_path)
    parser.add_argument("--regex-output", type=workspace_path)
    args = parser.parse_args()
    if args.discovery:
        if args.regex_output is None:
            parser.error("--discovery requires --regex-output")
        verify_discovery(json.loads(args.discovery.read_text(encoding="utf-8-sig")))
        # CTest uses POSIX extended syntax: noncapturing Python groups are not supported.
        pattern = "^(" + "|".join(re.escape(name) for name in sorted(CASES)) + ")$"
        args.regex_output.write_text(pattern, encoding="utf-8")
    else:
        verify_junit(args.output_junit)
        print(f"Character allocation qualification: {len(CASES)} cases passed; zero failures, errors or skips.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, ET.ParseError, DefusedXmlException) as error:
        print(f"Character allocation qualification failed: {error}", file=sys.stderr)
        sys.exit(1)
