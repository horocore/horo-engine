#!/usr/bin/env python3
"""Run exact Scene, Navigation or Collision probes unsupported by MSVC Debug iterator proxies."""

import argparse
import json
from pathlib import Path
import re
import sys
from defusedxml import ElementTree as ET
from defusedxml.common import DefusedXmlException

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
NAVIGATION_PREFIX = "HoroNavigationProjectSettingsTests::"
NAVIGATION_PHASES = {
    NAVIGATION_PREFIX + "Every navigation commit allocation failure precedes all owner publication": "commit",
    NAVIGATION_PREFIX + "Every navigation grouped replay allocation failure preserves all owner histories": "replay",
    NAVIGATION_PREFIX + "Scene decoding releases completed DOM without allocating during retirement": "scene",
}
NAVIGATION_CASES = frozenset(NAVIGATION_PHASES)


COLLISION_PHASES = {
    "HoroProjectCollisionSettingsTests::Native registry inventory recovers after every one-shot allocation failure": "inventory",
    "HoroProjectCollisionSettingsTests::Collision path admission propagates every allocation failure before owner publication": "paths",
    "HoroProjectCollisionSettingsTests::Collision history publication exhaustively rolls back every one-shot allocation failure": "commit",
    "HoroProjectCollisionSettingsTests::Canonical Scene decoder unwinds every single allocation failure without terminating completed DOM cleanup": "scene",
}
SIDECAR_CASE = ("HoroProjectCollisionSettingsTests::Canonical sidecar decoding unwinds every allocation "
                "across valid invalid and deep compound DOMs")
SIDECAR_PHASES = ("valid", "duplicate", "malformed", "deep")
COLLISION_CASES = frozenset((*COLLISION_PHASES, SIDECAR_CASE))


def verify_discovery(document, cases=CASES):
    """Require every exact case and refuse a disabled replacement for executable evidence."""
    tests = document.get("tests", [])
    families = {name.split(" - ", 1)[0] for name in cases}
    selected = [test for test in tests if any(
        test.get("name") == family or test.get("name", "").startswith(family + " - ")
        for family in families
    )]
    names = [test["name"] for test in selected]
    if len(names) != len(cases) or set(names) != cases:
        raise ValueError(f"Scene allocation discovery differs: missing={sorted(cases - set(names))}")
    for test in selected:
        if any(prop.get("name") == "DISABLED" and prop.get("value") for prop in test.get("properties", [])):
            raise ValueError(f"Scene allocation test is disabled: {test['name']}")

def verify_junit(path, cases=CASES):
    """Count actual cases rather than assertions and reject every skipped or failed execution."""
    path = Path(path).resolve()
    if not path.is_relative_to(Path.cwd().resolve()):
        raise ValueError(WORKSPACE_EVIDENCE_ERROR)
    root = ET.parse(path).getroot()
    executed = list(root.iter("testcase"))
    names = [case.get("name") for case in executed]
    if len(executed) != len(cases) or set(names) != cases:
        raise ValueError(f"Scene allocation execution differs: expected {len(cases)}, got {len(executed)}")
    for case in executed:
        failed = any(case.find(tag) is not None for tag in ("failure", "error", "skipped"))
        if failed or case.get("status") in ("notrun", "disabled"):
            raise ValueError(f"Scene allocation test did not pass: {case.get('name')}")
    if cases == NAVIGATION_CASES:
        verify_navigation_positions(executed)
    for suite in root.iter("testsuite"):
        for field in ("failures", "errors", "skipped", "disabled"):
            if int(suite.get(field, "0")):
                raise ValueError(f"Scene allocation suite has nonzero {field}")


def verify_navigation_positions(executed):
    """Require completed evidence for every measured allocation, without a platform-specific upper bound."""
    for case in executed:
        if case.get("status") != "run":
            raise ValueError(f"Navigation allocation case did not execute: {case.get('name')}")
        phase = NAVIGATION_PHASES[case.get("name")]
        pattern = rf"\[navigation-allocation\] {phase} measured=(\d+) completed=(\d+)"
        markers = re.findall(pattern, "".join(case.itertext()))
        if len(markers) != 1 or int(markers[0][0]) == 0 or markers[0][0] != markers[0][1]:
            raise ValueError(f"Navigation fault positions incomplete: {phase}")


def write_regex(path, cases=CASES):
    """Write only resolved workspace evidence, including for direct non-CLI callers."""
    path = Path(path).resolve()
    if not path.is_relative_to(Path.cwd().resolve()):
        raise ValueError(WORKSPACE_EVIDENCE_ERROR)
    # CTest uses POSIX extended syntax: noncapturing Python groups are not supported.
    pattern = "^(" + "|".join(re.escape(name) for name in sorted(cases)) + ")$"
    path.write_text(pattern, encoding="utf-8")


def verify_collision_positions(path):
    """Require proof of every measured failure position, including probes above Debug-sized budgets."""
    verify_junit(path, COLLISION_CASES)
    for case in ET.parse(path).getroot().iter("testcase"):
        sidecar = case.get("name") == SIDECAR_CASE
        phases = SIDECAR_PHASES if sidecar else (COLLISION_PHASES[case.get("name")],)
        prefix = "collision-sidecar-allocation" if sidecar else "collision-allocation"
        output = "".join(case.itertext())
        for phase in phases:
            positions = re.findall(r"\[" + prefix + r"\] " + phase + r" measured=(\d+) completed=(\d+)", output)
            if case.get("status") != "run" or len(positions) != 1:
                raise ValueError("Collision allocation execution has no unique complete position proof")
            measured, completed = map(int, positions[0])
            if measured == 0 or completed != measured:
                raise ValueError("Collision allocation sweep did not execute every measured position")


def workspace_path(value):
    """Reject CLI traversal and symlink escapes before reading or writing workspace evidence."""
    root = Path.cwd().resolve()
    candidate = Path(value).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise argparse.ArgumentTypeError(WORKSPACE_EVIDENCE_ERROR) from error
    return candidate


def main():
    """Validate discovery before execution, or validate the resulting exact Release JUnit."""
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--discovery", type=workspace_path)
    mode.add_argument("--output-junit", type=workspace_path)
    parser.add_argument("--regex-output", type=workspace_path)
    parser.add_argument("--family", choices=("scene", "navigation", "collision"), default="scene")
    args = parser.parse_args()
    families = {"scene": CASES, "navigation": NAVIGATION_CASES, "collision": COLLISION_CASES}
    cases = families[args.family]
    if args.discovery:
        if args.regex_output is None:
            parser.error("--discovery requires --regex-output")
        verify_discovery(json.loads(args.discovery.read_text(encoding="utf-8-sig")), cases)
        write_regex(args.regex_output, cases)
    else:
        if args.family == "collision":
            verify_collision_positions(args.output_junit)
        else:
            verify_junit(args.output_junit, cases)
        print(f"{args.family} allocation qualification: {len(cases)} cases passed; zero failures, errors or skips.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, ET.ParseError, DefusedXmlException) as error:
        print(f"Scene allocation qualification failed: {error}", file=sys.stderr)
        sys.exit(1)
