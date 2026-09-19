#!/usr/bin/env python3
"""Prepare a worktree and request C/C++ diagnostics from SonarQube for IDE.

With no ``--port``, the script configures a compilation database, writes an
ignored VS Code workspace, opens it in a dedicated window, discovers that
window's bridge port, and analyzes the selected files. JSON is the only stdout.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


SUPPORTED_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"})
TRANSLATION_UNIT_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx"})
DEFAULT_BUILD_DIRECTORY = Path("build/sonar-local")
DEFAULT_COMPILE_COMMANDS = DEFAULT_BUILD_DIRECTORY / "compile_commands.json"
BRIDGE_PORTS = range(64120, 64131)
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
    """Parse preparation, selection, and bridge options."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="*", type=Path, help="C/C++ files relative to the worktree")
    selector = parser.add_mutually_exclusive_group()
    selector.add_argument("--base", help="Analyze committed C/C++ changes since this Git ref")
    selector.add_argument("--dirty", action="store_true", help="Analyze staged, unstaged, and untracked changes (default)")
    parser.add_argument("--port", type=int, help="Use an already identified bridge port instead of opening a VS Code window")
    parser.add_argument("--timeout", type=float, default=600.0, help="Bridge request timeout in seconds (default: 600)")
    parser.add_argument("--startup-timeout", type=float, default=60.0, help="Seconds to wait for a new IDE bridge (default: 60)")
    parser.add_argument("--build-directory", type=Path, default=DEFAULT_BUILD_DIRECTORY)
    parser.add_argument("--connection-id", default="horocore")
    parser.add_argument("--project-key", default="horocore_horo-engine")
    parser.add_argument("--no-prepare", action="store_true", help="Require an existing compilation database and IDE window")
    return parser.parse_args(arguments)


def run_command(command: Sequence[str], error_message: str) -> subprocess.CompletedProcess[str]:
    """Run a prerequisite command without contaminating JSON stdout."""
    try:
        result = subprocess.run(command, check=False, capture_output=True, text=True)
    except FileNotFoundError as error:
        raise AnalysisError(f"{command[0]} is required") from error
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise AnalysisError(f"{error_message}{': ' + detail if detail else ''}")
    return result


def run_git(root: Path, *arguments: str) -> bytes:
    """Run Git in *root* and return its NUL-safe stdout."""
    try:
        return subprocess.run(["git", "-C", str(root), *arguments], check=True, capture_output=True).stdout
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
        tracked = paths_from_nul_output(run_git(root, "diff", "--name-only", "--diff-filter=ACMR", "-z", merge_base))
        untracked = paths_from_nul_output(run_git(root, "ls-files", "--others", "--exclude-standard", "-z"))
        return list(dict.fromkeys([*tracked, *untracked])), f"base:{base}"

    records = run_git(root, "status", "--porcelain=v1", "-z").split(b"\0")
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
        if record[0:1] in {b"R", b"C"} or record[1:2] in {b"R", b"C"}:
            index += 1
    return paths, "dirty"


def select_files(root: Path, requested: Sequence[Path], source: str) -> Selection:
    """Keep existing worktree C/C++ files and explain every skipped input."""
    submitted: list[Path] = []
    skipped: list[dict[str, str]] = []
    seen: set[Path] = set()
    for candidate in requested:
        path = (candidate if candidate.is_absolute() else root / candidate).resolve()
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


def prepare_compilation_database(root: Path, build_directory: Path) -> Path:
    """Configure all first-party targets so changed tests also have commands."""
    build = (root / build_directory).resolve() if not build_directory.is_absolute() else build_directory.resolve()
    run_command(
        [
            "cmake",
            "-S",
            str(root),
            "-B",
            str(build),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DBUILD_TESTING=ON",
        ],
        "CMake could not prepare SonarQube for IDE analysis",
    )
    database = build / "compile_commands.json"
    if not database.is_file():
        raise AnalysisError(f"CMake did not create the compilation database: {database}")
    return database


def validate_compile_commands(compilation_database: Path, files: Sequence[Path]) -> None:
    """Require a command for every submitted translation unit; headers use IDE context."""
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

    required = [path for path in files if path.suffix.lower() in TRANSLATION_UNIT_SUFFIXES]
    missing = [str(path) for path in required if path not in compiled_files]
    if missing:
        raise AnalysisError("Compilation database has no command for submitted file(s): " + ", ".join(missing))


def bridge_url(port: int, path: str) -> str:
    """Build a loopback-only bridge URL after validating Sonar's port range."""
    if port not in BRIDGE_PORTS:
        raise AnalysisError("--port must be in SonarQube for IDE's 64120-64130 range")
    return f"http://127.0.0.1:{port}{path}"


def request_bridge(url: str, timeout: float, body: bytes | None = None) -> object:
    """Call the bridge with the localhost headers required by its origin checks."""
    headers = {"Host": "localhost", "Origin": "http://localhost"}
    if body is not None:
        headers["Content-Type"] = "application/json"
    request = Request(url, data=body, headers=headers, method="POST" if body is not None else "GET")
    try:
        with urlopen(request, timeout=timeout) as response:  # nosec B310: URL is loopback and port-validated.
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


def available_bridge_ports(timeout: float = 0.2) -> set[int]:
    """Return responsive official bridge ports without inspecting unrelated listeners."""
    available: set[int] = set()
    for port in BRIDGE_PORTS:
        try:
            request_bridge(bridge_url(port, BRIDGE_STATUS_PATH), timeout)
            available.add(port)
        except AnalysisError:
            continue
    return available


def write_workspace(root: Path, build_directory: Path, connection_id: str, project_key: str) -> Path:
    """Write an ignored dedicated workspace without mutating user VS Code settings."""
    build = (root / build_directory).resolve() if not build_directory.is_absolute() else build_directory.resolve()
    workspace = build / "sonar-ide.code-workspace"
    payload = {
        "folders": [{"path": str(root)}],
        "settings": {
            "sonarlint.pathToCompileCommands": str(build / "compile_commands.json"),
            "sonarlint.connectedMode.project": {"connectionId": connection_id, "projectKey": project_key},
        },
    }
    workspace.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return workspace


def open_workspace_and_find_bridge(workspace: Path, startup_timeout: float) -> int:
    """Open a dedicated VS Code window and return its new or remembered bridge."""
    if startup_timeout <= 0:
        raise AnalysisError("--startup-timeout must be greater than zero")
    marker = workspace.parent / f"{workspace.name}.bridge.json"
    remembered_port: int | None = None
    try:
        marker_payload = json.loads(marker.read_text(encoding="utf-8"))
        candidate = marker_payload.get("port") if isinstance(marker_payload, dict) else None
        if isinstance(candidate, int) and candidate in BRIDGE_PORTS:
            remembered_port = candidate
    except (OSError, json.JSONDecodeError):
        pass

    existing = available_bridge_ports()
    run_command(["code", "--new-window", str(workspace)], "VS Code could not open the Sonar workspace")
    if remembered_port in existing:
        return remembered_port

    deadline = time.monotonic() + startup_timeout
    while time.monotonic() < deadline:
        created = available_bridge_ports() - existing
        if len(created) == 1:
            port = created.pop()
            marker.write_text(json.dumps({"port": port}) + "\n", encoding="utf-8")
            return port
        if len(created) > 1:
            raise AnalysisError(f"Multiple new IDE bridges appeared: {sorted(created)}")
        time.sleep(0.5)
    raise AnalysisError("The dedicated VS Code window did not expose a new SonarQube for IDE bridge")


def normalized_findings(response: object) -> list[dict[str, object]]:
    """Validate the official bridge response shape while preserving Sonar fields."""
    if not isinstance(response, dict) or not isinstance(response.get("findings"), list):
        raise AnalysisError("IDE bridge response did not contain a findings array")
    findings = response["findings"]
    if not all(isinstance(finding, dict) for finding in findings):
        raise AnalysisError("IDE bridge response contained an invalid finding")
    return findings


def changed_line_ranges(root: Path, base: str, files: Sequence[Path]) -> dict[Path, list[tuple[int, int]]]:
    """Return inclusive added-line ranges, including the full contents of untracked files."""
    merge_base = run_git(root, "merge-base", base, "HEAD").decode("utf-8").strip()
    relative = [str(path.relative_to(root)) for path in files]
    diff = run_git(root, "diff", "--unified=0", "--no-color", "--diff-filter=ACMR", merge_base, "--", *relative).decode(
        "utf-8", errors="surrogateescape"
    )
    ranges: dict[Path, list[tuple[int, int]]] = {}
    current: Path | None = None
    hunk = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
    for line in diff.splitlines():
        if line.startswith("+++ "):
            name = line[4:]
            current = None if name == "/dev/null" else (root / name.removeprefix("b/")).resolve()
            continue
        match = hunk.match(line)
        if current is not None and match is not None:
            start = int(match.group(1))
            count = int(match.group(2) or "1")
            if count > 0:
                ranges.setdefault(current, []).append((start, start + count - 1))

    tracked = set(paths_from_nul_output(run_git(root, "ls-files", "-z")))
    for path in files:
        relative_path = path.relative_to(root)
        if relative_path not in tracked:
            line_count = len(path.read_text(encoding="utf-8", errors="surrogateescape").splitlines())
            if line_count > 0:
                ranges[path] = [(1, line_count)]
    return ranges


def filter_findings_to_ranges(
    findings: Sequence[dict[str, object]], ranges: dict[Path, list[tuple[int, int]]]
) -> tuple[list[dict[str, object]], int]:
    """Keep findings overlapping changed lines and count file-level baseline findings."""
    selected: list[dict[str, object]] = []
    for finding in findings:
        file_path = finding.get("filePath")
        text_range = finding.get("textRange")
        path = Path(file_path).resolve() if isinstance(file_path, str) else None
        if path not in ranges:
            continue
        if not isinstance(text_range, dict):
            selected.append(finding)
            continue
        start = text_range.get("startLine")
        end = text_range.get("endLine", start)
        if isinstance(start, int) and isinstance(end, int) and any(start <= last and end >= first for first, last in ranges[path]):
            selected.append(finding)
    return selected, len(findings) - len(selected)


def analyze_when_indexed(port: int, files: Sequence[Path], request_timeout: float, startup_timeout: float) -> list[dict[str, object]]:
    """Retry only the bridge's explicit not-yet-indexed startup response."""
    body = json.dumps({"fileAbsolutePaths": [str(path) for path in files]}).encode("utf-8")
    deadline = time.monotonic() + startup_timeout
    while True:
        try:
            response = request_bridge(bridge_url(port, BRIDGE_ANALYZE_PATH), request_timeout, body)
            return normalized_findings(response)
        except AnalysisError as error:
            if "No files were found to be indexed by SonarQube for IDE" not in str(error) or time.monotonic() >= deadline:
                raise
            time.sleep(0.5)


def result_payload(
    selection: Selection,
    port: int,
    compile_commands: Path,
    findings: list[dict[str, object]],
    baseline_findings_suppressed: int = 0,
) -> dict[str, object]:
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
        "baselineFindingsSuppressed": baseline_findings_suppressed,
        "findings": findings,
    }


def main(arguments: Sequence[str] | None = None) -> int:
    """Prepare the worktree, run one local IDE request, and print JSON."""
    args = parse_arguments(arguments)
    try:
        if args.timeout <= 0:
            raise AnalysisError("--timeout must be greater than zero")
        if args.files and (args.base is not None or args.dirty):
            raise AnalysisError("Explicit files cannot be combined with a change selector")
        root = repository_root()
        build_directory = args.build_directory
        database = (
            (root / build_directory / "compile_commands.json").resolve()
            if args.no_prepare
            else prepare_compilation_database(root, build_directory)
        )
        if not database.is_file():
            raise AnalysisError(f"Compilation database is missing: {database}")
        requested, source = (list(args.files), "explicit") if args.files else changed_paths(root, args.base)
        selection = select_files(root, requested, source)
        if not selection.submitted:
            raise AnalysisError("No analyzable C/C++ files were selected")
        validate_compile_commands(database, selection.submitted)

        port = args.port
        if port is None:
            if args.no_prepare:
                raise AnalysisError("--no-prepare requires --port")
            workspace = write_workspace(root, build_directory, args.connection_id, args.project_key)
            port = open_workspace_and_find_bridge(workspace, args.startup_timeout)
        request_bridge(bridge_url(port, BRIDGE_STATUS_PATH), args.timeout)
        findings = analyze_when_indexed(port, selection.submitted, args.timeout, args.startup_timeout)
        suppressed = 0
        if args.base is not None:
            findings, suppressed = filter_findings_to_ranges(findings, changed_line_ranges(root, args.base, selection.submitted))
        print(json.dumps(result_payload(selection, port, database, findings, suppressed), ensure_ascii=False, indent=2))
        return 1 if findings else 0
    except AnalysisError as error:
        print(json.dumps({"status": "error", "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
