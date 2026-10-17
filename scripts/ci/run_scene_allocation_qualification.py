#!/usr/bin/env python3
"""Run the exact Scene allocation probes unsupported by MSVC Debug iterator proxies."""

import json
from pathlib import Path
import sys
from defusedxml import ElementTree as ET
from defusedxml.common import DefusedXmlException

if __name__ == "__main__":
    from allocation_qualification_cli import parse_evidence_arguments, write_case_regex
else:
    from scripts.ci.allocation_qualification_cli import parse_evidence_arguments, write_case_regex

WORKSPACE_EVIDENCE_ERROR = "Qualification evidence must remain inside the current workspace"

PREFIX = "HoroSceneRuntimeConversionTests::"
CASES = frozenset(PREFIX + name for name in (
    "Expansion allocation failure never evicts the prior exact immutable candidate",
    "Scene encoding admission reports each allocation failure without accepting invalid authored bytes",
    "Canonical serialization survives every production allocation failure",
    "Canonical moves retire every wire alternative without allocation - OrderedWireValue",
    "Canonical moves retire every wire alternative without allocation - SortedWireValue",
    "Resolver admission computes actual immutable evidence transactionally under every allocation failure",
    "Recursive canonical copies preserve source and destination at every allocation failure - OrderedWireValue",
    "Recursive canonical copies preserve source and destination at every allocation failure - SortedWireValue",
    "Cache pressure after validated worker completion does not discard complete Scene output",
    "Cancellation during completion memoization cannot publish a validated definition",
))


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
        raise ValueError(f"Scene allocation discovery differs: missing={sorted(CASES - set(names))}")
    for test in selected:
        if any(prop.get("name") == "DISABLED" and prop.get("value") for prop in test.get("properties", [])):
            raise ValueError(f"Scene allocation test is disabled: {test['name']}")


def verify_junit(path):
    """Count actual cases rather than assertions and reject every skipped or failed execution."""
    path = Path(path).resolve()
    if not path.is_relative_to(Path.cwd().resolve()):
        raise ValueError(WORKSPACE_EVIDENCE_ERROR)
    root = ET.parse(path).getroot()
    cases = list(root.iter("testcase"))
    names = [case.get("name") for case in cases]
    if len(cases) != len(CASES) or set(names) != CASES:
        raise ValueError(f"Scene allocation execution differs: expected {len(CASES)}, got {len(cases)}")
    for case in cases:
        failed = any(case.find(tag) is not None for tag in ("failure", "error", "skipped"))
        if failed or case.get("status") in ("notrun", "disabled"):
            raise ValueError(f"Scene allocation test did not pass: {case.get('name')}")
    for suite in root.iter("testsuite"):
        for field in ("failures", "errors", "skipped", "disabled"):
            if int(suite.get(field, "0")):
                raise ValueError(f"Scene allocation suite has nonzero {field}")


def write_regex(path):
    """Write only resolved workspace evidence, including for direct non-CLI callers."""
    write_case_regex(path, CASES)


def main():
    """Validate discovery before execution, or validate the resulting exact Release JUnit."""
    args = parse_evidence_arguments(__doc__)
    if args.discovery:
        verify_discovery(json.loads(args.discovery.read_text(encoding="utf-8-sig")))
        write_regex(args.regex_output)
    else:
        verify_junit(args.output_junit)
        print(f"Scene allocation qualification: {len(CASES)} cases passed; zero failures, errors or skips.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, ET.ParseError, DefusedXmlException) as error:
        print(f"Scene allocation qualification failed: {error}", file=sys.stderr)
        sys.exit(1)
