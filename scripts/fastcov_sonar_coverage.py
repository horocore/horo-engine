#!/usr/bin/env python3
"""Convert fastcov counters to Sonar line coverage with the existing noncode policy."""

import argparse
import json
from pathlib import Path
import re
# Only XML construction/serialization is imported; JSON is the sole input format.
from xml.etree.ElementTree import Element, ElementTree, SubElement  # nosec B405

from compare_sonar_coverage import repository_path


def noncode(text: str) -> bool:
    """Match gcovr's zero-hit comment, brace and standalone else heuristic."""
    text = re.sub(r"//.*?$", "", text)
    text = re.sub(r"/\*.*?\*/", "", text)
    text = re.sub(r"\s+", "", text)
    return text in {"", "{", "}", "else"}


def source_counts(tests: dict) -> dict[int, int]:
    """Merge every test and template counter without converting negative hits to coverage."""
    if not isinstance(tests, dict):
        raise ValueError("Invalid fastcov test map")
    result = {}
    for test in tests.values():
        if not isinstance(test, dict) or not isinstance(test.get("lines"), dict):
            raise ValueError("Invalid fastcov line map")
        for line, count in test["lines"].items():
            number = int(line)
            if number < 1 or not isinstance(count, int) or isinstance(count, bool) or count < 0:
                raise ValueError("Invalid fastcov line counter")
            result[number] = result.get(number, 0) + count
    return result


def convert(data: dict, root: Path) -> tuple[Element, dict]:
    """Require real repository sources and emit one boolean per measured source line."""
    document = Element("coverage", version="1")
    measured = covered = excluded = 0
    if not isinstance(data, dict):
        raise ValueError("Invalid fastcov report")
    sources = data["sources"]
    if not isinstance(sources, dict) or not sources:
        raise ValueError("Empty fastcov report")
    for name, tests in sorted(sources.items()):
        path = repository_path(Path(name), root)
        text = path.read_text(encoding="utf-8").splitlines()
        counts = source_counts(tests)
        file = SubElement(document, "file", path=path.relative_to(root).as_posix())
        for number, count in sorted(counts.items()):
            if number > len(text):
                raise ValueError(f"Coverage line exceeds source length: {path.name}:{number}")
            if count == 0 and noncode(text[number - 1]):
                excluded += 1
                continue
            SubElement(file, "lineToCover", lineNumber=str(number),
                                    covered=str(count > 0).lower())
            measured += 1
            covered += count > 0
    if measured == 0:
        raise ValueError("No measured source lines")
    return document, {"lines": measured, "covered": covered, "excludedNoncode": excluded}


def main() -> int:
    """Validate repository paths before reading counters or writing the Sonar report."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path.cwd().resolve()
    try:
        source = repository_path(args.input, root)
        output = repository_path(args.output, root)
        document, metrics = convert(json.loads(source.read_text(encoding="utf-8")), root)
        ElementTree(document).write(output, encoding="utf-8", xml_declaration=True)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"Fastcov conversion failed: {error}\n")
    print(json.dumps(metrics, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
