#!/usr/bin/env python3
"""Worktree-safe Sonar IDE and Codacy feedback before opening a PR."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import sys
import time
import uuid
from pathlib import Path

import quality_tools
import sonar_preflight
from quality_support import (CANCELLED, cache_directory, changed_ranges, digest, fingerprint,
                             identity, lock, read_json, run, write_json)
from sonar_ide_analysis import (AnalysisError, SUPPORTED_SUFFIXES, changed_paths,
                                run_git, validated_git_ref)
from sonar_sessions import SessionPool


def arguments(argv=None):
    """Keep selectors consistent across both analyzer families."""
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("check", "doctor", "stop"):
        command = commands.add_parser(name)
        command.add_argument("--worktree", type=Path, default=Path.cwd())
        command.add_argument("--format", choices=("text", "json"), default="text")
        command.add_argument("--timeout", type=float, default=600)
        command.add_argument("--project-key", default="horocore_horo-engine")
        command.add_argument("--connection-id", default="horocore")
        command.add_argument("--code-user-data", type=Path, default=Path.home() / ".config/Code")
        command.add_argument("--extensions-directory", type=Path, default=Path.home() / ".vscode/extensions")
        command.add_argument("--startup-timeout", type=float, default=90)
        if name == "check":
            selectors = command.add_mutually_exclusive_group()
            selectors.add_argument("--base")
            selectors.add_argument("--dirty", action="store_true")
            selectors.add_argument("--files", nargs="+", type=Path)
            command.add_argument("--compile-commands", type=Path)
            command.add_argument("--only", choices=("sonar", "codacy"), help="Intentionally partial provider scope")
        if name == "doctor":
            command.add_argument("--offline", action="store_true", help="Skip cloud configuration comparison")
    args = parser.parse_args(argv)
    if args.timeout <= 0 or args.startup_timeout <= 0:
        parser.error("Timeouts must be positive")
    return args


def selection(root: Path, args) -> tuple[list[Path], list[dict], str, str]:
    """Include committed and local deltas, excluding deleted/outside paths explicitly."""
    head = os.fsdecode(run_git(root, "rev-parse", "HEAD")).strip()
    base = args.base or (None if args.dirty or args.files else "origin/main")
    base_sha = os.fsdecode(run_git(root, "merge-base", "--", validated_git_ref(base), "HEAD")).strip() if base else head
    if args.files:
        requested, source = args.files, "explicit"
    else:
        requested, source = changed_paths(root, base)
    selected, skipped = [], []
    allowed = set(quality_tools.repository_files(root))
    for path in requested:
        absolute = (root / path).resolve()
        if not absolute.is_relative_to(root):
            skipped.append({"path": str(path), "reason": "outside_worktree", "intentional": False})
            continue
        relative = absolute.relative_to(root)
        if not absolute.is_file():
            skipped.append({"path": str(path), "reason": "deleted_or_missing", "intentional": not bool(args.files)})
        elif relative not in allowed:
            skipped.append({"path": str(path), "reason": "excluded_input", "intentional": True})
        elif relative not in selected:
            selected.append(relative)
    return selected, skipped, source, base_sha


def guarded(callback, *args) -> dict:
    """Preserve provider failures alongside successful independent analyses."""
    started = time.monotonic()
    try:
        result = callback(*args)
    except (AnalysisError, OSError, ValueError, KeyError, TypeError) as error:
        result = {"status": "incomplete", "error": str(error)}
    result["durationSeconds"] = round(time.monotonic() - started, 3)
    return result


def tool_versions(root: Path, names: tuple[str, ...]) -> dict:
    """Record executable versions without hiding individual discovery failures."""
    versions = {}
    for tool in names:
        executable = shutil.which(tool)
        try:
            result = run([executable, "--version"], root, 30) if executable else None
            versions[tool] = {"available": bool(result and result.returncode == 0),
                              "version": result.stdout.strip().splitlines()[0]
                              if result and result.stdout.strip() else None}
        except AnalysisError as error:
            versions[tool] = {"available": False, "version": None, "error": str(error)}
    return versions


def annotate(items: list[dict], root: Path, ranges: dict) -> list[dict]:
    """Mark changed-line overlap without hiding other findings or guessing their age."""
    result = []
    for item in items:
        item = dict(item)
        path = item.get("filePath")
        if path:
            absolute = Path(path)
            if absolute.is_absolute() and absolute.is_relative_to(root):
                path = str(absolute.relative_to(root))
            item["filePath"] = path
        text_range = item.get("textRange", {})
        first = item.get("line", text_range.get("startLine"))
        last = item.get("endLine", text_range.get("endLine", first))
        item["changedLine"] = (any(first <= end and last >= start for start, end in ranges.get(path, []))
                               if isinstance(first, int) and isinstance(last, int) else None)
        result.append(item)
    return sorted(result, key=lambda item: (item["changedLine"] is not True, item.get("filePath", ""),
                                           item.get("line", item.get("textRange", {}).get("startLine", 0))))


def codacy_stage(root: Path, cache: Path, paths: list[Path], timeout: float) -> dict:
    """Use the installed CLI snapshot without updating any repository configuration."""
    if not paths:
        return {"status": "not_applicable", "issues": []}
    config = read_json(root / ".codacy/codacy.config.json")
    mirror = quality_tools.snapshot(root, cache / "mirror")
    info = quality_tools.codacy_info(mirror)
    submitted = [path for path in paths if not quality_tools.excluded(path, config)]
    result = quality_tools.codacy_check(root, mirror, submitted, config, info, timeout)
    cpp = [path for path in submitted if path.suffix.lower() in SUPPORTED_SUFFIXES]
    result["complexity"] = guarded(quality_tools.complexity, root, cpp, result.get("capability", {}), timeout)
    result["duplication"] = guarded(quality_tools.duplication, root, mirror, cpp, config, info, timeout)
    result["excluded"] = [str(path) for path in paths if path not in submitted]
    result["submitted"] = [str(path) for path in submitted]
    if any(result[name]["status"] == "incomplete" for name in ("complexity", "duplication")):
        result["status"] = "incomplete"
    # CLI normally reports relative paths; relocate any absolute mirror paths as well.
    for issue in result.get("issues", []):
        path = Path(issue["filePath"])
        if path.is_absolute() and path.is_relative_to(mirror):
            issue["filePath"] = str(path.relative_to(mirror))
    return result


def check(root: Path, repository_cache: Path, args) -> dict:
    """Run independent providers concurrently while holding the worktree lease."""
    cache = repository_cache / "worktrees" / identity(root)
    paths, skipped, source, base_sha = selection(root, args)
    before = fingerprint(root)
    ranges = changed_ranges(root, base_sha, paths)
    report = {"schemaVersion": 1, "worktree": str(root), "head": before["head"], "baseSha": base_sha,
              "selection": source, "files": [str(path) for path in paths], "skipped": skipped,
              "scope": args.only or "sonar_and_codacy", "providers": {},
              "configuration": {str(path): digest(root / path) for path in
                                (Path(".codacy/codacy.config.json"), Path("sonar-project.properties"))
                                if (root / path).is_file()}}
    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
        pending = {}
        if args.only != "codacy":
            cpp = [root / path for path in paths if path.suffix.lower() in SUPPORTED_SUFFIXES]
            pending["sonarIde"] = executor.submit(guarded, sonar_preflight.check, root, cache, repository_cache, cpp, args)
            pending["secrets"] = executor.submit(guarded, quality_tools.secrets, root, paths, args.timeout)
        if args.only != "sonar":
            pending["codacy"] = executor.submit(guarded, codacy_stage, root, cache, paths, args.timeout)
        try:
            report["providers"] = {name: future.result() for name, future in pending.items()}
        except KeyboardInterrupt:
            CANCELLED.set()
            report["providers"] = {name: future.result() for name, future in pending.items()}
            report["cancelled"] = True
    report["stale"] = before != fingerprint(root) or any(
        provider.get("stale") for provider in report["providers"].values())
    for provider in report["providers"].values():
        if "findings" in provider:
            provider["findings"] = annotate(provider["findings"], root, ranges)
        if "issues" in provider:
            provider["issues"] = annotate(provider["issues"], root, ranges)
    report["durationSeconds"] = round(time.monotonic() - started, 3)
    report["versions"] = tool_versions(root, ("sonar", "codacy-analysis", "code", "cmake", "ninja"))
    incomplete = report["stale"] or report.get("cancelled") or any(not item["intentional"] for item in skipped)
    incomplete = incomplete or any(value["status"] == "incomplete" for value in report["providers"].values())
    findings = any(value["status"] == "issues_found" or value.get("issues") or value.get("findings")
                   for value in report["providers"].values())
    report["status"] = "incomplete" if incomplete else ("issues_found" if findings else "clean")
    if not paths and not incomplete:
        report["status"] = "no_changes"
    report["exitCode"] = 2 if incomplete else int(bool(findings))
    return report


def cloud_drift(root: Path, cache: Path, timeout: float) -> dict:
    """Refresh a disposable config for comparison; never import or upload changes."""
    remote = os.fsdecode(run_git(root, "remote", "get-url", "origin")).strip()
    match = re.search(r"github\.com[:/]([^/]+)/([^/]+?)(?:\.git)?$", remote)
    if not match:
        raise AnalysisError("Cannot resolve Codacy GitHub repository from origin")
    mirror = quality_tools.snapshot(root, cache / "mirror")
    target = cache / "cloud-config.json"
    result = run(["codacy-analysis", "init", "--remote", "gh", match[1], match[2],
                  "--config-file", str(target)], mirror, timeout)
    if result.returncode:
        raise AnalysisError(f"Cloud configuration read failed (exit {result.returncode}); verify Codacy authentication")
    local, cloud = read_json(root / ".codacy/codacy.config.json"), read_json(target)

    def comparable(config):
        return {tool["toolId"]: json.dumps({key: value for key, value in tool.items() if key != "patterns"}, sort_keys=True)
                + json.dumps(sorted(tool.get("patterns", []), key=lambda item: item["patternId"]), sort_keys=True)
                for tool in config["tools"]}
    left, right = comparable(local), comparable(cloud)
    changed = sorted(key for key in left.keys() | right.keys() if left.get(key) != right.get(key))
    excludes_changed = set(local.get("exclude", [])) != set(cloud.get("exclude", []))
    return {"status": "drift" if changed or excludes_changed else "matched", "changedTools": changed,
            "excludesChanged": excludes_changed, "localHash": digest(root / ".codacy/codacy.config.json"),
            "cloudHash": digest(target), "snapshot": str(target)}


def doctor(root: Path, cache: Path, args) -> dict:
    """Check local prerequisites and optional Codacy cloud configuration drift."""
    versions = tool_versions(root, ("sonar", "codacy-analysis", "code", "cmake", "ninja"))
    report = {"schemaVersion": 1, "worktree": str(root), "versions": versions,
              "codacy": guarded(quality_tools.codacy_info, root), "ideUserData": str(args.code_user_data),
              "ideExtensions": str(args.extensions_directory), "coverage": "not_in_scope"}
    if not args.offline:
        report["cloudConfiguration"] = guarded(cloud_drift, root, cache / "doctor", args.timeout)
    else:
        report["cloudConfiguration"] = {"status": "not_checked"}
    report["ideConfiguration"] = {
        "settingsAvailable": (args.code_user_data / "User/settings.json").is_file(),
        "extensionAvailable": bool(list(args.extensions_directory.glob("sonarsource.sonarlint-vscode-*/package.json")))}
    ready = (all(item["available"] for item in versions.values())
             and all(report["ideConfiguration"].values()) and report["codacy"].get("status") != "incomplete")
    report["status"] = "ready_for_local_check" if ready else "incomplete"
    report["exitCode"] = 2 if report["status"] == "incomplete" else 0
    return report


def print_provider(name: str, result: dict) -> None:
    """Print provider findings, omissions and advisory metrics."""
    print(f"  {name}: {result['status']} ({result.get('durationSeconds', 0)}s)")
    if result.get("error"):
        print(f"    {result['error']}")
    for error in result.get("errors", []) + result.get("unsupported", []):
        print(f"    incomplete: {error}")
    for unavailable in result.get("capability", {}).get("unavailable", []):
        print(f"    unavailable: {unavailable}")
    for warning in result.get("warnings", []):
        print(f"    warning: {warning}")
    for item in result.get("issues", []) + result.get("findings", []):
        line = item.get("line", item.get("textRange", {}).get("startLine", "?"))
        marker = "changed" if item.get("changedLine") else "context"
        print(f"    [{marker}] {item.get('severity', 'unknown')} {item.get('patternId', item.get('ruleKey', ''))} "
              f"{item.get('filePath', '?')}:{line}: {item.get('message', '')}")
    for metric in ("complexity", "duplication"):
        if metric in result:
            value = result[metric]
            print(f"    {metric} (advisory): {value['status']} {value.get('error', '')}")
            for item in value.get("files", []):
                print(f"      {item['path']}: CCN {item['cyclomaticComplexity']}")
            if value.get("clones"):
                print(f"      {len(value['clones'])} clone groups touch selected files; see JSON for occurrences")
    for item in result.get("skipped", []):
        print(f"    skipped: {item}")


def output(report: dict, path: Path, format_name: str) -> None:
    """Keep the raw report and print a concise, actionable local summary."""
    report["reportPath"] = str(path)
    write_json(path, report)
    if format_name == "json":
        print(json.dumps(report, ensure_ascii=True, indent=2))
        return
    print(f"Quality preflight: {report['status']} | {report['worktree']}")
    if "versions" in report:
        for name, version in report["versions"].items():
            print(f"  {name}: {version['version'] if version['available'] else 'unavailable'}")
        for name in ("cloudConfiguration",):
            if name not in report:
                continue
            value = report.get(name, {})
            print(f"  {name}: {value.get('status')}; {value.get('error') or value.get('changedTools', '')}")
    for name, result in report.get("providers", {}).items():
        print_provider(name, result)
    if report.get("stale"):
        print("  Inputs changed during analysis; rerun this scope.")
    for skipped in report.get("skipped", []):
        print(f"  skipped: {skipped}")
    print(f"Report: {path}")


def main(argv=None) -> int:
    """Never claim a clean result when a provider or prerequisite is missing."""
    args = arguments(argv)
    CANCELLED.clear()
    try:
        if sys.platform != "linux" or not hasattr(os, "pidfd_open"):
            raise AnalysisError("Managed preflight currently requires Linux with pidfd support")
        root = Path(os.fsdecode(run_git(args.worktree, "rev-parse", "--show-toplevel")).strip()).resolve()
        cache = cache_directory(root)
        args.code_user_data = args.code_user_data.resolve()
        args.extensions_directory = args.extensions_directory.resolve()
        if getattr(args, "compile_commands", None):
            args.compile_commands = (root / args.compile_commands).resolve()
        with lock(cache / "worktrees" / identity(root) / "check.lock", args.timeout):
            if args.command == "check":
                report = check(root, cache, args)
            elif args.command == "doctor":
                report = doctor(root, cache, args)
            else:
                stopped = SessionPool(cache, root, args.timeout).stop()
                busy = any(item["status"] in {"busy", "unverified"} for item in stopped)
                report = {"worktree": str(root), "status": "busy" if busy else "stopped", "sessions": stopped,
                          "exitCode": 2 if busy else 0}
        target = cache / "worktrees" / identity(root) / "reports" / f"{time.time_ns()}-{uuid.uuid4().hex[:8]}.json"
        output(report, target, args.format)
        return report["exitCode"]
    except (AnalysisError, OSError, ValueError, KeyboardInterrupt) as error:
        print(json.dumps({"status": "incomplete", "exitCode": 2, "error": str(error)}, ensure_ascii=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
