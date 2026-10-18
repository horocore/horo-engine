"""Shared workspace-evidence admission for mandatory allocation qualification CLIs."""

import argparse
from pathlib import Path
import re

WORKSPACE_EVIDENCE_ERROR = "Qualification evidence must remain inside the current workspace"


def workspace_path(value):
    """Reject traversal and resolved symlink escapes for both input and output evidence."""
    candidate = Path(value).resolve()
    try:
        candidate.relative_to(Path.cwd().resolve())
    except ValueError as error:
        raise argparse.ArgumentTypeError(WORKSPACE_EVIDENCE_ERROR) from error
    return candidate


def parse_evidence_arguments(description, *, families=()):
    """Preserve the common discovery/JUnit CLI and require an explicit discovery output."""
    parser = argparse.ArgumentParser(description=description)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--discovery", type=workspace_path)
    mode.add_argument("--output-junit", type=workspace_path)
    parser.add_argument("--regex-output", type=workspace_path)
    if families:
        parser.add_argument("--family", choices=families, default=families[0])
    arguments = parser.parse_args()
    if arguments.discovery is not None and arguments.regex_output is None:
        parser.error("--discovery requires --regex-output")
    return arguments


def write_case_regex(path, cases):
    """Recheck the actual sink, including direct callers and paths changed since argument parsing."""
    destination = Path(path).resolve()
    if not destination.is_relative_to(Path.cwd().resolve()):
        raise ValueError(WORKSPACE_EVIDENCE_ERROR)
    # CTest uses POSIX extended syntax, without Python noncapturing groups.
    pattern = "^(" + "|".join(re.escape(name) for name in sorted(cases)) + ")$"
    destination.write_text(pattern, encoding="utf-8")
