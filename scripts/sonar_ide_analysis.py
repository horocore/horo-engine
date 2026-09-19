#!/usr/bin/env python3
"""Request local C/C++ diagnostics from the SonarQube for IDE HTTP bridge.

The bridge is created by a running, trusted VS Code window with the
SonarQube for IDE extension enabled.  This tool does not authenticate to, upload
to, or query SonarQube Cloud.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


SUPPORTED_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"})
DEFAULT_COMPILE_COMMANDS = Path("build/sonar-local/compile_commands.json")
BRIDGE_STATUS_PATH = "/sonarlint/api/status"
BRIDGE_ANALYZE_PATH = "/sonarlint/api/analysis/files"


class AnalysisError(RuntimeError):
    """A prerequisite or the local IDE bridge prevented analysis."""


@dataclass(frozen=True)
class Selection:
    """Files selected for one bridge request and paths intentionally omitted."""

    submitted: list[Path]
    skipped: list[dict[str, str]]
    source: str


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    """Parse the local selection and bridge connection options."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="*", type=Path, help="C/C++ files relative to the worktree")
    selector = parser.add_mutually_exclusive_group()
    selector.add_argument("--base", help="Analyze committed C/C++ changes since this Git ref")
    selector.add_argument("--dirty", action="store_true", help="Analyze staged, unstaged, and untracked changes (default)")
    parser.add_argument("--port", required=True, type=int, help="SonarQube for IDE bridge port (64120-64130)")
    parser.add_argument("--timeout", type=float, default=90.0, help="HTTP timeout in seconds (default: 90)")
    parser.add_argument(
        "--compile-commands",
        type=Path,
        default=DEFAULT_COMPILE_COMMANDS,
        help="Compilation database configured in the VS Code workspace",
    )
    return parser.parse_args(arguments)


def run_git(root: Path, *arguments: str) -> bytes:
    """Run Git in *root* and return its NUL-safe stdout."""
    try:
        return subprocess.run(
            ["git", "-C", str(root), *arguments],
            check=True,
            capture_output=True,
        ).stdout
    except FileNotFoundError as error:
        raise AnalysisError("git is required to select worktree files") from error
    except subprocess.CalledProcessError as error:
        detail = error.stderr.decode("utf-8", errors="replace").strip()
        raise AnalysisError(detail or "git could not select files for analysis") from error


def repository_root() -> Path:
    """Return the root of the current Git worktree."""
    output = run_git(Path.cwd(), "rev-parse", "--show-toplevel")
    return Path(output.decode("utf-8").strip()).resolve()


def paths_from_nul_output(output: bytes) -> list[Path]:
    """Decode Git's NUL-delimited paths without losing spaces or Unicode."""
    return [Path(item.decode("utf-8", errors="surrogateescape")) for item in output.split(b"\0") if item]


def changed_paths(root: Path, base: str | None) -> tuple[list[Path], str]:
    """Return changed paths for a base ref or the current dirty worktree."""
    if base is not None:
        merge_base = run_git(root, "merge-base", base, "HEAD").decode("utf-8").strip()
        return paths_from_nul_output(
            run_git(root, "diff", "--name-only", "--diff-filter=ACMR", "-z", merge_base, "HEAD")
        ), f"base:{base}"

    status = run_git(root, "status", "--porcelain=v1", "-z")
    records = status.split(b"\0")
    paths: list[Path] = []
    index = 0
    while index < len(records):
        record = records[index]
        index += 1
        if not record:
            continue
        if len(record) < 4:
            raise AnalysisError("Git returned an invalid porcelain status record")
        paths.append(Path(record[3:].decode("utf-8", errors="surrogateescape")))
        # Rename/copy records carry a second NUL-delimited original path.
        if record[0:1] in {b"R", b"C"} or record[1:2] in {b"R", b"C"}:
            index += 1
    return paths, "dirty"


def select_files(root: Path, requested: Sequence[Path], source: str) -> Selection:
    """Keep existing worktree C/C++ files and explain every skipped input."""
    submitted: list[Path] = []
    skipped: list[dict[str, str]] = []
    seen: set[Path] = set()
    for candidate in requested:
        path = candidate if candidate.is_absolute() else root / candidate
        path = path.resolve()
        try:
            path.relative_to(root)
        except ValueError:
            skipped.append({"path": str(candidate), "reason": "outside_worktree"})
            continue
        if not path.is_file():
            skipped.append({"path": str(candidate), "reason": "missing_or_not_regular_file"})
            continue
        if path.suffix.lower() not in SUPPORTED_SUFFIXES:
            skipped.append({"path": str(candidate), "reason": "unsupported_file_type"})
            continue
        if path not in seen:
            submitted.append(path)
            seen.add(path)
    return Selection(submitted, skipped, source)


def validate_compile_commands(compilation_database: Path, files: Sequence[Path]) -> None:
    """Require a concrete compilation command for every submitted C/C++ path."""
    try:
        entries = json.loads(compilation_database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AnalysisError(f"Cannot read compilation database: {compilation_database}") from error
    if not isinstance(entries, list):
        raise AnalysisError(f"Compilation database is not an array: {compilation_database}")

    compiled_files: set[Path] = set()
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            continue
        directory = Path(entry["directory"]) if isinstance(entry.get("directory"), str) else compilation_database.parent
        source = Path(entry["file"])
        compiled_files.add((source if source.is_absolute() else directory / source).resolve())

    missing = [str(path) for path in files if path not in compiled_files]
    if missing:
        raise AnalysisError(
            "Compilation database has no command for submitted file(s): " + ", ".join(missing)
        )


def bridge_url(port: int, path: str) -> str:
    """Build a loopback-only bridge URL after validating Sonar's port range."""
    if not 64120 <= port <= 64130:
        raise AnalysisError("--port must be in SonarQube for IDE's 64120-64130 range")
    return f"http://127.0.0.1:{port}{path}"


def request_bridge(url: str, timeout: float, body: bytes | None = None) -> object:
    """Call the bridge with the localhost headers required by its origin checks."""
    headers = {"Host": "localhost", "Origin": "http://localhost"}
    if body is not None:
        headers["Content-Type"] = "application/json"
    request = Request(url, data=body, headers=headers, method="POST" if body is not None else "GET")
    try:
        with urlopen(request, timeout=timeout) as response:  # nosec B310: loopback URL is validated above
            return json.loads(response.read().decode("utf-8")) if body is not None else response.status
    except HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace").strip()
        raise AnalysisError(f"IDE bridge returned HTTP {error.code}{': ' + detail if detail else ''}") from error
    except URLError as error:
        raise AnalysisError(f"Cannot reach the SonarQube for IDE bridge: {error.reason}") from error
    except TimeoutError as error:
        raise AnalysisError("SonarQube for IDE bridge timed out") from error
    except json.JSONDecodeError as error:
        raise AnalysisError("IDE bridge returned invalid JSON") from error


def normalized_findings(response: object) -> list[dict[str, object]]:
    """Validate the official bridge response shape while preserving Sonar fields."""
    if not isinstance(response, dict) or not isinstance(response.get("findings"), list):
        raise AnalysisError("IDE bridge response did not contain a findings array")
    findings = response["findings"]
    if not all(isinstance(finding, dict) for finding in findings):
        raise AnalysisError("IDE bridge response contained an invalid finding")
    return findings


def result_payload(selection: Selection, port: int, compile_commands: Path, findings: list[dict[str, object]]) -> dict[str, object]:
    """Create the stable machine-readable report written to stdout."""
    return {
        "status": "issues_found" if findings else "clean",
        "bridge": {"port": port, "status": "available"},
        "compileCommands": str(compile_commands),
        "selection": {
            "source": selection.source,
            "submitted": [str(path) for path in selection.submitted],
            "skipped": selection.skipped,
        },
        "findings": findings,
    }


def main(arguments: Sequence[str] | None = None) -> int:
    """Run one local IDE analysis request and print a JSON report."""
    args = parse_arguments(arguments)
    try:
        if args.timeout <= 0:
            raise AnalysisError("--timeout must be greater than zero")
        if args.files and (args.base is not None or args.dirty):
            raise AnalysisError("Explicit files cannot be combined with --base or --dirty")
        root = repository_root()
        compile_commands = (root / args.compile_commands).resolve() if not args.compile_commands.is_absolute() else args.compile_commands.resolve()
        if not compile_commands.is_file():
            raise AnalysisError(f"Compilation database is missing: {compile_commands}")
        requested, source = (list(args.files), "explicit") if args.files else changed_paths(root, args.base)
        selection = select_files(root, requested, source)
        if not selection.submitted:
            raise AnalysisError("No analyzable C/C++ files were selected")
        validate_compile_commands(compile_commands, selection.submitted)
        request_bridge(bridge_url(args.port, BRIDGE_STATUS_PATH), args.timeout)
        response = request_bridge(
            bridge_url(args.port, BRIDGE_ANALYZE_PATH),
            args.timeout,
            json.dumps({"fileAbsolutePaths": [str(path) for path in selection.submitted]}).encode("utf-8"),
        )
        findings = normalized_findings(response)
        print(json.dumps(result_payload(selection, args.port, compile_commands, findings), ensure_ascii=False, indent=2))
        return 1 if findings else 0
    except AnalysisError as error:
        print(json.dumps({"status": "error", "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
