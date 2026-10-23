"""Require a complete CTest report before publishing main coverage after failures."""

from __future__ import annotations

import json
from collections import Counter
from pathlib import Path

from defusedxml import ElementTree


def read_bounded(path: Path, limit: int) -> bytes:
    """Read a local report with a fixed memory bound."""
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"Report exceeds size limit: {path}")
    return data


def selected_tests(discovery: Path) -> dict[str, bool]:
    """Return independently discovered names and their configured disabled state."""
    manifest = json.loads(read_bounded(discovery, 16 * 1024 * 1024))
    if manifest.get("kind") != "ctestInfo":
        raise ValueError("Expected CTest JSON discovery")
    tests: dict[str, bool] = {}
    for test in manifest["tests"]:
        name = test["name"]
        if not isinstance(name, str) or not name or name in tests:
            raise ValueError("CTest discovery contains invalid or duplicate names")
        tests[name] = any(
            prop["name"] == "DISABLED" and prop["value"] is True
            for prop in test.get("properties", [])
        )
    if not tests:
        raise ValueError("CTest discovery selected no tests")
    return tests


def validate_case(case: ElementTree.Element, disabled: bool) -> None:
    """Reject unexecuted cases unless CTest explicitly skipped or disabled them."""
    status = case.get("status")
    if status == "disabled" and disabled:
        return
    if disabled:
        raise ValueError("Configured disabled test has inconsistent report state")
    if status == "run" and case.find("failure") is None:
        return
    if status == "fail" and case.find("failure") is not None:
        return
    if status == "notrun" and case.find("skipped") is not None:
        return
    raise ValueError(f"CTest case lacks a terminal result: {case.get('name')}")


def validate_completion(discovery: Path, junit: Path) -> None:
    """Accept full failed suites while rejecting truncated or mismatched reports."""
    selected = selected_tests(discovery)
    report = ElementTree.fromstring(read_bounded(junit, 128 * 1024 * 1024))
    if report.tag != "testsuite":
        raise ValueError("Expected CTest JUnit testsuite")
    cases = report.findall("testcase")
    if int(report.attrib["tests"]) != len(selected):
        raise ValueError("CTest report count does not match discovery")
    if Counter(case.get("name") for case in cases) != Counter(selected.keys()):
        raise ValueError("CTest report does not contain every selected test exactly once")
    for case in cases:
        validate_case(case, selected[case.attrib["name"]])


if __name__ == "__main__":
    validate_completion(
        Path("build/sonar/coverage-ctest-discovery.json"),
        Path("build/sonar/ctest.xml"),
    )
