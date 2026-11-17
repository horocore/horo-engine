"""Verify one built console catalog/core fixture and its linked profile metadata.

This does not qualify a packaged graphical host, subsystem command adapters,
SDK distribution, performance, or an external quality gate.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess

PROFILES = ("Editor", "Development", "Profile", "Shipping", "Diagnostics", "Server")
CORE = {"help", "find", "version", "clear"}
DIAGNOSTIC = {"log.level", "sys.metrics", "sys.profile_capture"}
DEVELOPMENT = {"game.scene_tree", "game.inspect"}
SUPPORT = {"diag.support_bundle"}
HISTORY = "console.history.limit"
OVERLAYS = {f"overlay.{name}.visible" for name in
            ("logs", "frame", "cpu", "memory", "jobs", "renderer", "assets", "input", "network")}
DEVELOPMENT_VARIABLES = {"overlay.scene.visible", "debug.draw.physics", "renderer.debug.wireframe"}
MAX_ARTIFACT_BYTES = 512 * 1024 * 1024
MAX_MANIFEST_BYTES = 1024 * 1024


class QualificationError(ValueError):
    """A completed observation contradicts the required artifact contract."""


def expected_inventory(profile: str) -> tuple[set[str], set[str]]:
    if profile not in PROFILES:
        raise QualificationError("Unsupported artifact profile")
    commands = set(CORE)
    variables: set[str] = set()
    if profile != "Server":
        commands.add("screenshot")
        variables.add("console.visible")
    if profile != "Shipping":
        commands.update(DIAGNOSTIC)
        variables.update({HISTORY, "asset.streaming.debug"})
        if profile != "Server":
            variables.update(OVERLAYS)
    if profile in {"Editor", "Development"}:
        commands.update(DEVELOPMENT)
        variables.update(DEVELOPMENT_VARIABLES)
    if profile in {"Editor", "Development", "Diagnostics"}:
        commands.update(SUPPORT)
    return commands, variables


def scan_artifact(path: Path, names: set[str]) -> dict:
    path = path.resolve(strict=True)
    size = path.stat().st_size
    if not path.is_file() or not 0 < size <= MAX_ARTIFACT_BYTES:
        raise QualificationError("Artifact must be a nonempty regular file within the size budget")
    tokens = {name: name.encode("ascii") for name in names}
    overlap = max(map(len, tokens.values()), default=1) - 1
    digest = hashlib.sha256()
    present: set[str] = set()
    tail = b""
    observed = 0
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            observed += len(chunk)
            if observed > MAX_ARTIFACT_BYTES:
                raise QualificationError("Artifact grew beyond the size budget")
            digest.update(chunk)
            window = tail + chunk
            present.update(name for name, token in tokens.items() if token in window)
            tail = window[-overlap:] if overlap else b""
    if observed != size:
        raise QualificationError("Artifact changed size during observation")
    return {"path": str(path), "size": size, "sha256": digest.hexdigest(), "present": sorted(present)}


def verify_manifest(profile: str, manifest: str) -> dict[str, str]:
    if len(manifest.encode("utf-8")) > MAX_MANIFEST_BYTES:
        raise QualificationError("Fixture manifest exceeds the output budget")
    lines = manifest.splitlines()
    if not lines or lines[0] != "horo-console-artifact\t1":
        raise QualificationError("Missing fixture manifest identity/version")
    commands: set[str] = set()
    variables: set[str] = set()
    execution: dict[str, str] = {}
    profiles = []
    for line in lines[1:]:
        fields = line.split("\t")
        if len(fields) != 2:
            raise QualificationError("Malformed fixture manifest row")
        kind, value = fields
        if kind == "profile":
            profiles.append(value)
        elif kind in {"command", "variable"}:
            values = commands if kind == "command" else variables
            if value in values:
                raise QualificationError("Duplicate fixture descriptor identity")
            values.add(value)
        elif kind in {"passed", "omitted"}:
            if value in execution:
                raise QualificationError("Duplicate fixture execution evidence")
            execution[value] = kind
        else:
            raise QualificationError("Unknown fixture manifest row")
    expected_commands, expected_variables = expected_inventory(profile)
    if profiles != [profile] or commands != expected_commands or variables != expected_variables:
        raise QualificationError("Fixture profile or complete catalog inventory differs from the required artifact")
    expected_execution = {name: "passed" for name in CORE}
    expected_execution["history"] = "omitted" if profile == "Shipping" else "passed"
    if execution != expected_execution:
        raise QualificationError("Fixture core/history execution evidence is incomplete or contradictory")
    return execution


def qualify(profile: str, binary: Path, archive: Path) -> dict:
    commands, variables = expected_inventory(profile)
    # Optional application adapters are outside this catalog/Core fixture in every profile.
    universe: set[str] = {"net.debug_status"}
    for candidate in PROFILES:
        known_commands, known_variables = expected_inventory(candidate)
        universe.update(known_commands | known_variables)
    allowed = commands | variables
    denied = universe - allowed
    files = {"binary": scan_artifact(binary, universe), "catalogArchive": scan_artifact(archive, universe)}
    for kind, record in files.items():
        forbidden = set(record["present"]) & denied
        if forbidden:
            raise QualificationError(f"{kind} retains denied metadata: {', '.join(sorted(forbidden))}")
    if not allowed <= set(files["binary"]["present"]):
        raise QualificationError("Linked binary lacks required positive metadata controls")
    # Shared control factories belong to Core; only selected catalog strings must exist in this separate archive.
    core_controls = {HISTORY, "console.visible", "overlay.logs.visible", "overlay.jobs.visible", "overlay.scene.visible", "overlay.network.visible"}
    archive_controls = (commands - (CORE | {"sys.metrics", "game.scene_tree", "game.inspect"})) | (variables - core_controls)
    if not archive_controls <= set(files["catalogArchive"]["present"]):
        raise QualificationError("Catalog archive lacks required positive metadata controls")
    result = subprocess.run([files["binary"]["path"]], cwd=binary.resolve().parent,
                            capture_output=True, text=True, encoding="utf-8", errors="strict", timeout=30, check=False)
    if result.returncode:
        raise QualificationError(f"Native fixture exited with {result.returncode}")
    execution = verify_manifest(profile, result.stdout)
    for kind, path in (("binary", binary), ("catalogArchive", archive)):
        fresh = scan_artifact(path, universe)
        if fresh["sha256"] != files[kind]["sha256"] or fresh["size"] != files[kind]["size"]:
            raise QualificationError("Artifact changed between inspection and native execution")
    return {"schemaVersion": 1, "status": "passed", "profile": profile,
            "observedAt": datetime.now(timezone.utc).isoformat(), "scope": "catalog-and-core-fixture",
            "execution": execution, "artifacts": files}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", required=True, choices=PROFILES)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = qualify(args.profile, args.binary, args.archive)
        code = 0
    except QualificationError as error:
        report = {"schemaVersion": 1, "status": "failed", "profile": args.profile, "error": str(error)}
        code = 1
    except (OSError, UnicodeError, subprocess.TimeoutExpired) as error:
        report = {"schemaVersion": 1, "status": "incomplete", "profile": args.profile,
                  "error": f"Artifact observation unavailable ({type(error).__name__})"}
        code = 2
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Console artifact {args.profile}: {report['status']}; report: {args.report}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
