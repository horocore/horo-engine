#!/usr/bin/env python3
"""Report CCN totals with the Lizard executable selected by current Codacy CLI."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from quality_tools import complexity, json_command
from sonar_ide_analysis import AnalysisError, SUPPORTED_SUFFIXES, changed_paths, run_git


def main(argv=None) -> int:
    """Accept explicit C/C++ paths, a branch delta, or the dirty worktree."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="*", type=Path)
    parser.add_argument("--base")
    args = parser.parse_args(argv)
    if args.files and args.base:
        parser.error("Explicit files cannot be combined with --base")
    try:
        root = Path(run_git(Path.cwd(), "rev-parse", "--show-toplevel").decode().strip()).resolve()
        requested = args.files or changed_paths(root, args.base)[0]
        selected = []
        for path in requested:
            absolute = (root / path).resolve()
            if absolute.is_file() and absolute.is_relative_to(root) and absolute.suffix.lower() in SUPPORTED_SUFFIXES:
                selected.append(absolute.relative_to(root))
        capability, code = json_command(["codacy-analysis", "analyze", "--inspect", "--tool", "Lizard",
                                         "--output-format", "json", "--no-log", "--no-update-notifier"], root)
        if code or capability.get("errors") or capability.get("capability", {}).get("unavailable"):
            raise AnalysisError("Codacy could not resolve Lizard")
        print(json.dumps(complexity(root, sorted(set(selected)), capability["capability"], 600), indent=2))
        return 0
    except (AnalysisError, OSError, ValueError) as error:
        print(json.dumps({"status": "incomplete", "error": str(error)}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
