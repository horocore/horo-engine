#!/usr/bin/env python3
"""Verify the two complete Extension allocation sweeps in the supported Release ABI."""

import argparse
import json
from pathlib import Path
import re
import sys
from defusedxml import ElementTree as element_tree
from defusedxml.common import DefusedXmlException

PREFIX = "HoroExtensionManagerAllocationTests::"
CASES = {
    PREFIX + "Every recovery admission allocation failure precedes native callbacks and preserves independent status":
        ("recovery-admission-allocation", 1),
    PREFIX + "Completed manifest DOM retirement preserves every allocation failure on valid and rejected schemas":
        ("manifest-allocation", 3),
}


def verify_discovery(document):
    """Require both exact enabled executable cases before CTest filtering."""
    selected = [test for test in document.get("tests", []) if test.get("name", "").startswith(PREFIX)]
    if len(selected) != len(CASES) or {test["name"] for test in selected} != set(CASES):
        raise ValueError("Extension allocation discovery must contain both exact cases")
    for test in selected:
        if any(prop.get("name") == "DISABLED" and prop.get("value") for prop in test.get("properties", [])):
            raise ValueError(f"Extension allocation case is disabled: {test['name']}")


def verify_case(case):
    """Validate one actual successful case and every required nonzero completed sweep."""
    if any(case.find(tag) is not None for tag in ("failure", "error", "skipped")) or case.get("status") in ("notrun", "disabled"):
        raise ValueError(f"Extension allocation case did not pass: {case.get('name')}")
    marker, expected = CASES[case.get("name")]
    output = "".join(case.itertext())
    counts = re.findall(r"\[" + re.escape(marker) + r"\] measured=(\d+) completed=(\d+)", output)
    if len(counts) != expected or any(int(measured) == 0 or measured != completed for measured, completed in counts):
        raise ValueError(f"Extension allocation full-sweep evidence differs: {case.get('name')}")


def verify_junit(path):
    """Reject absent execution, failed/skipped cases, or incomplete/nonmeasured sweeps."""
    root = element_tree.parse(path).getroot()
    cases = list(root.iter("testcase"))
    if len(cases) != len(CASES) or {case.get("name") for case in cases} != set(CASES):
        raise ValueError("Extension allocation execution must contain both exact cases")
    for case in cases:
        verify_case(case)
    for suite in root.iter("testsuite"):
        if any(int(suite.get(field, "0")) for field in ("failures", "errors", "skipped", "disabled")):
            raise ValueError("Extension allocation suite contains failures, errors or skips")


def workspace_path(value):
    """Keep all qualification evidence within the invoking workspace."""
    candidate = Path(value).resolve()
    if not candidate.is_relative_to(Path.cwd().resolve()):
        raise argparse.ArgumentTypeError("Qualification evidence must remain inside the current workspace")
    return candidate


def main():
    """Verify discovery before execution, or validate the exact Release JUnit afterward."""
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
        pattern = "^(" + "|".join(re.escape(name) for name in sorted(CASES)) + ")$"
        args.regex_output.write_text(pattern, encoding="utf-8")
    else:
        verify_junit(args.output_junit)
        print("Extension allocation qualification: two cases and all four nonzero measured sweeps completed; zero failures, errors or skips.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, element_tree.ParseError, DefusedXmlException) as error:
        print(f"Extension allocation qualification failed: {error}", file=sys.stderr)
        sys.exit(1)
