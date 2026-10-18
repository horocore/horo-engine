#!/usr/bin/env python3
"""Verify the complete Extension and Workspace allocation sweeps in the supported Release ABI."""

import json
from pathlib import Path
import re
import sys
import runpy
from defusedxml import ElementTree as element_tree
from defusedxml.common import DefusedXmlException

# Resolve the trusted sibling identically for direct CLI and file-spec loading.
EVIDENCE = runpy.run_path(str(Path(__file__).with_name("allocation_qualification_cli.py")))
parse_evidence_arguments = EVIDENCE["parse_evidence_arguments"]
write_case_regex = EVIDENCE["write_case_regex"]

PREFIX = "HoroExtensionManagerAllocationTests::"
WORKSPACE_CASE = "HoroWorkspaceLayoutPersistenceTests::Split parsing releases every owned allocation when any allocation fails"
CASES = {
    WORKSPACE_CASE:
        ("workspace-allocation", 1),
    PREFIX + "Every recovery admission allocation failure precedes native callbacks and preserves independent status":
        ("recovery-admission-allocation", 1),
    PREFIX + "Completed manifest DOM retirement preserves every allocation failure on valid and rejected schemas":
        ("manifest-allocation", 3),
}


def verify_discovery(document):
    """Require all exact enabled executable cases before CTest filtering."""
    selected = [test for test in document.get("tests", [])
                if test.get("name", "").startswith(PREFIX) or test.get("name") == WORKSPACE_CASE]
    if len(selected) != len(CASES) or {test["name"] for test in selected} != set(CASES):
        raise ValueError("Extension allocation discovery must contain all exact cases")
    for test in selected:
        if any(prop.get("name") == "DISABLED" and prop.get("value") for prop in test.get("properties", [])):
            raise ValueError(f"Extension allocation case is disabled: {test['name']}")


def verify_case(case):
    """Validate one actual successful case and every required nonzero completed sweep."""
    if any(case.find(tag) is not None for tag in ("failure", "error", "skipped")) or case.get("status") in ("notrun", "disabled"):
        raise ValueError(f"Extension allocation case did not pass: {case.get('name')}")
    marker, expected = CASES[case.get("name")]
    output = "".join(case.itertext())
    if marker == "workspace-allocation":
        measured = re.findall(r"\[workspace-allocation\] measured=(\d+)", output)
        completed = re.findall(r"\[workspace-allocation\] completed=(\d+)", output)
        if len(measured) != expected or len(completed) != expected:
            raise ValueError("Workspace allocation full-sweep evidence is missing or duplicated")
        counts = list(zip(measured, completed))
    else:
        counts = re.findall(r"\[" + re.escape(marker) + r"\] measured=(\d+) completed=(\d+)", output)
    if len(counts) != expected or any(int(measured) == 0 or measured != completed for measured, completed in counts):
        raise ValueError(f"Extension allocation full-sweep evidence differs: {case.get('name')}")


def verify_junit(path):
    """Reject absent execution, failed/skipped cases, or incomplete/nonmeasured sweeps."""
    root = element_tree.parse(path).getroot()
    cases = list(root.iter("testcase"))
    if len(cases) != len(CASES) or {case.get("name") for case in cases} != set(CASES):
        raise ValueError("Extension allocation execution must contain all exact cases")
    for case in cases:
        verify_case(case)
    for suite in root.iter("testsuite"):
        if any(int(suite.get(field, "0")) for field in ("failures", "errors", "skipped", "disabled")):
            raise ValueError("Extension allocation suite contains failures, errors or skips")


def main():
    """Verify discovery before execution, or validate the exact Release JUnit afterward."""
    args = parse_evidence_arguments(__doc__)
    if args.discovery:
        verify_discovery(json.loads(args.discovery.read_text(encoding="utf-8-sig")))
        write_case_regex(args.regex_output, CASES)
    else:
        verify_junit(args.output_junit)
        print("Extension allocation qualification: three cases and all five nonzero measured sweeps completed; zero failures, errors or skips.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, element_tree.ParseError, DefusedXmlException) as error:
        print(f"Extension allocation qualification failed: {error}", file=sys.stderr)
        sys.exit(1)
