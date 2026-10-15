"""Require complete Windows Release Save allocation qualification evidence."""
from __future__ import annotations

import argparse
from pathlib import Path
import defusedxml.ElementTree as etree

CASES = {
    "Lifecycle allocation failure after visibility preserves publication knowledge": "lifecycle",
    "Retention allocation failure after selection preserves publication and binding ownership": "retention",
}
PREFIX = "HoroRuntimeSaveSlotLifecycleTests::"


def verify_report(path: Path) -> None:
    """Reject missing, skipped, failed or incompletely exercised allocation cases."""
    data = path.read_bytes()
    root = etree.fromstring(data, forbid_dtd=True)
    if root.tag != "testsuite" or any(root.get(key) != value for key, value in
                                     (("tests", "2"), ("failures", "0"), ("skipped", "0"))):
        raise ValueError("Qualification suite must report two tests with zero failures and skips")
    if root.get("errors", "0") != "0" or root.get("disabled", "0") != "0":
        raise ValueError("Qualification suite contains errors or disabled cases")
    cases = list(root.iter("testcase"))
    expected = {PREFIX + name for name in CASES}
    if len(cases) != 2 or {case.get("name") for case in cases} != expected:
        raise ValueError("Qualification requires exactly the two named Save allocation cases")
    for case in cases:
        if case.get("status") != "run" or any(case.find(tag) is not None for tag in ("skipped", "failure", "error")):
            raise ValueError(f"Qualification case did not pass: {case.get('name')}")
        phase = CASES[case.get("name", "").removeprefix(PREFIX)]
        output = "".join(case.itertext())
        require_variants(output, phase)


def require_variants(output: str, phase: str) -> None:
    """Require both generator paths through recovery and completed fixture cleanup."""
    markers = []
    for variant in ("success", "failure"):
        markers.extend((f"{phase}-sync-{variant}", f"{phase}-failure-reset",
                        f"{phase}-reopen-returned", f"{phase}-reconcile-returned",
                        f"{phase}-fixture-cleanup-next", "fixture-scope-exited"))
    cursor = 0
    for marker in markers:
        literal = f"[save-allocation] {marker}"
        position = output.find(literal, cursor)
        if position < 0:
            raise ValueError(f"Incomplete {phase} variant evidence: {marker}")
        cursor = position + len(literal)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    verify_report(args.report)
    print("Save allocation qualification: 2 passed cases, both variants completed, zero skips")


if __name__ == "__main__":
    main()
