#!/usr/bin/env python3
"""Compare generic Sonar line coverage reports for a collector experiment."""

import argparse
import json
import os
from pathlib import Path
from defusedxml import ElementTree as element_tree
from defusedxml.common import DefusedXmlException


def repository_path(path: Path, root: Path) -> Path:
    """Reject resolved input/output paths outside the selected repository."""
    resolved = os.path.realpath(path)
    base = os.path.realpath(root)
    if resolved != base and not resolved.startswith(base.rstrip(os.sep) + os.sep):
        raise ValueError(f"Path outside repository: {path.name}")
    return Path(resolved)


def read_report(path: Path, root: Path) -> dict[tuple[str, int], bool]:
    """Normalize source paths and retain every measured line, including misses."""
    document = element_tree.parse(repository_path(path, root)).getroot()
    if document.tag != "coverage" or document.get("version") != "1":
        raise ValueError(f"Unsupported coverage format: {path.name}")
    lines = {}
    for source in document.findall("file"):
        source_path = Path(source.attrib["path"])
        if not source_path.is_absolute():
            source_path = root / source_path
        normalized = source_path.resolve().relative_to(root.resolve()).as_posix()
        for entry in source.findall("lineToCover"):
            number = int(entry.attrib["lineNumber"])
            covered = entry.attrib["covered"]
            if number < 1 or covered not in {"true", "false"}:
                raise ValueError(f"Invalid coverage line in {path.name}")
            key = (normalized, number)
            # gcovr emits repeated lines for multiple template instantiations.
            # A source line is covered if any instance executed.
            lines[key] = lines.get(key, False) or covered == "true"
    if not lines:
        raise ValueError(f"Empty coverage report: {path.name}")
    return lines


def describe_lines(keys: set[tuple[str, int]]) -> list[dict]:
    """Keep complete differences in the artifact, ordered for review."""
    return [{"file": file, "line": line} for file, line in sorted(keys)]


def compare_reports(baseline: dict, candidate: dict) -> dict:
    """Compare line identities and covered flags, rather than just percentages."""
    baseline_keys = set(baseline)
    candidate_keys = set(candidate)
    missing = baseline_keys - candidate_keys
    extra = candidate_keys - baseline_keys
    changed = {
        key for key in baseline_keys & candidate_keys
        if baseline[key] != candidate[key]
    }
    return {
        "equivalent": not (missing or extra or changed),
        "gcovr": {"lines": len(baseline), "covered": sum(baseline.values())},
        "fastcov": {"lines": len(candidate), "covered": sum(candidate.values())},
        "missingLines": describe_lines(missing),
        "extraLines": describe_lines(extra),
        "changedLines": [
            {"file": file, "line": line,
             "gcovrCovered": baseline[(file, line)],
             "fastcovCovered": candidate[(file, line)]}
            for file, line in sorted(changed)
        ],
    }


def format_summary(result: dict) -> str:
    """Explain timing and equivalence separately in the workflow summary."""
    timings = result["seconds"]
    text = [
        "## Fastcov coverage experiment", "",
        "Sonar continues to use the gcovr baseline report.", "",
        "| Collector | Seconds | Measured lines | Covered lines |",
        "| --- | ---: | ---: | ---: |",
    ]
    for name in ("gcovr", "fastcov"):
        metrics = result[name]
        text.append(
            f"| {name} | {timings[name]} | {metrics['lines']} | {metrics['covered']} |"
        )
    text.extend([
        "", f"Line coverage equivalent: **{result['equivalent']}**",
        f"Missing lines: {len(result['missingLines'])}; "
        f"extra lines: {len(result['extraLines'])}; "
        f"changed covered flags: {len(result['changedLines'])}.",
        "", "Fastcov time includes JSON collection and Sonar XML conversion.",
        "This comparison checks line coverage; it does not establish branch parity.",
        "See the coverage experiment artifact for complete differences.", "",
    ])
    return "\n".join(text)


def main() -> int:
    """Write evidence; coverage differences remain diagnostic during the trial."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--gcovr-report", type=Path, required=True)
    parser.add_argument("--fastcov-report", type=Path, required=True)
    parser.add_argument("--gcovr-seconds", type=int, required=True)
    parser.add_argument("--fastcov-seconds", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.root.resolve() != Path.cwd().resolve():
            raise ValueError("Repository root must be the current working directory")
        output = repository_path(args.output, args.root)
        result = compare_reports(
            read_report(args.gcovr_report, args.root),
            read_report(args.fastcov_report, args.root),
        )
        result["seconds"] = {
            "gcovr": args.gcovr_seconds, "fastcov": args.fastcov_seconds,
        }
        output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(format_summary(result))
    except (OSError, element_tree.ParseError, DefusedXmlException, ValueError, KeyError) as error:
        parser.exit(2, f"Coverage comparison failed: {error}\n")
    print(f"Line coverage equivalent: {result['equivalent']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
