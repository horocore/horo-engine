"""Codacy CLI, Lizard and CPD adapters; all generated files live in cache."""

from __future__ import annotations

import csv
import fnmatch
import io
import json
import os
import shutil
import xml.etree.ElementTree as element_tree  # nosec B405
# CPD XML declarations are rejected before parsing; no external entities are accepted.
from pathlib import Path

from quality_support import digest, read_json, repository_files, run, write_json
from sonar_ide_analysis import AnalysisError, SUPPORTED_SUFFIXES


def json_command(argv: list[str], root: Path, timeout: float = 600) -> tuple[dict, int]:
    """Require structured output even when a tool's exit code allows partial results."""
    result = run(argv, root, timeout)
    try:
        payload = json.loads(result.stdout)
    except ValueError as error:
        raise AnalysisError(f"{Path(argv[0]).name} returned no usable JSON (exit {result.returncode})") from error
    if not isinstance(payload, dict):
        raise AnalysisError(f"{Path(argv[0]).name} returned a non-object report")
    return payload, result.returncode


def codacy_info(root: Path) -> dict:
    """Read descriptors from the installed CLI rather than hard-code tool routing."""
    payload, code = json_command(["codacy-analysis", "info", "--output-format", "json"], root, 90)
    if code != 0 or not isinstance(payload.get("tools"), list):
        raise AnalysisError("Cannot discover installed Codacy analyzers")
    return {tool["id"]: tool for tool in payload["tools"]}


def snapshot(root: Path, directory: Path) -> Path:
    """
    Copy inputs into a disposable mirror: CLI adapters write .codacy/generated.

    Never use hardlinks: a tool writing its inputs must not modify user files.
    Preserve relative local configuration paths in an independent Git index.
    """
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    if not (directory / ".git").is_dir():
        initialized = run(["git", "init", "--quiet", str(directory)], directory)
        if initialized.returncode:
            raise AnalysisError("Cannot initialize the disposable Codacy input mirror")
    files = repository_files(root)
    manifest_path = directory.parent / "snapshot.json"
    previous = read_json(manifest_path) if manifest_path.exists() else {}
    current = {}
    for relative in files:
        source, destination = root / relative, directory / relative
        content_hash = digest(source)
        current[str(relative)] = content_hash
        if previous.get(str(relative)) != content_hash or not destination.is_file() or digest(destination) != content_hash:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
    for name in previous.keys() - current.keys():
        target = directory / name
        if target.resolve().is_relative_to(directory.resolve()):
            target.unlink(missing_ok=True)
    write_json(manifest_path, current)
    staged = run(["git", "add", "--all", "--", "."], directory)
    if staged.returncode:
        raise AnalysisError("Cannot index the disposable Codacy input mirror")
    return directory


def codacy_check(root: Path, mirror: Path, selected: list[Path], config: dict, info: dict,
                 timeout: float) -> dict:
    """Run configured tools and account for every routed path and execution result."""
    extensions = {str(path): language(path) for path in selected}
    tools = []
    covered = set()
    unsupported = []
    for configured in config["tools"]:
        tool_id = configured["toolId"]
        descriptor = info.get(tool_id)
        if descriptor is None:
            unsupported.append({"toolId": tool_id, "reason": "missing_descriptor"})
            continue
        languages = {item.lower().replace("c++", "cpp").replace("c#", "csharp")
                     for item in descriptor.get("languages", [])}
        routed = {path for path, lang in extensions.items() if lang.lower() in languages}
        if tool_id == "Agentlinter":
            routed = {str(path) for path in selected if str(path).startswith((".codex/", ".github/instructions/"))
                      or path.name in {"AGENTS.md", "CLAUDE.md"}}
        if routed:
            tools.append(tool_id)
            covered.update(routed)
    skipped = [{"path": str(path), "reason": "no_configured_local_analyzer"}
               for path in selected if str(path) not in covered]
    if not selected:
        return {"status": "not_applicable", "issues": [], "skipped": [], "unsupported": []}
    if not tools:
        return {"status": "incomplete", "issues": [], "skipped": skipped, "unsupported": unsupported}
    argv = ["codacy-analysis", "analyze", "--output-format", "json", "--no-log", "--no-update-notifier",
            "--fail-if-missing", "--parallel-tools", "2", "--tool-timeout", str(int(timeout * 1000))]
    for tool in tools:
        argv.extend(["--tool", tool])
    # Literal paths containing glob characters must not be silently expanded by the CLI.
    if any(any(char in str(path) for char in "*?[") for path in selected):
        raise AnalysisError("Codacy --files cannot safely select paths containing glob metacharacters")
    argv.extend(["--files", *[str(path) for path in selected]])
    payload, code = json_command(argv, mirror, timeout)
    errors = execution_errors(payload, tools)
    capability = payload.get("capability", {})
    incomplete = code not in {0, 1} or bool(errors or skipped or unsupported or capability.get("unavailable"))
    payload.update(status="incomplete" if incomplete else ("issues_found" if payload.get("issues") else "clean"),
                   errors=errors, skipped=skipped, unsupported=unsupported, cliExitCode=code)
    return payload


def execution_errors(payload: dict, tools: list[str]) -> list:
    """Reject omitted tools, skipped inputs and partially routed executions."""
    executions = {item["toolId"]: item for item in payload.get("toolResults", [])}
    errors = list(payload.get("errors", []))
    reported_skips = payload.get("skippedFiles", [])
    if reported_skips:
        errors.append({"reason": "analyzer_skipped_files", "files": reported_skips})
    for tool in tools:
        result = executions.get(tool)
        if not result or result.get("status") != "success" or result.get("errorCount", 0) or not result.get("filesAnalyzed", 0):
            errors.append({"toolId": tool, "reason": "missing_or_incomplete_execution"})
    capability = payload.get("capability", {})
    for ready in capability.get("ready", []):
        execution = executions.get(ready["toolId"], {})
        if ready.get("filesRouted") != execution.get("filesAnalyzed"):
            errors.append({"toolId": ready["toolId"], "reason": "routed_file_count_mismatch",
                           "routed": ready.get("filesRouted"), "analyzed": execution.get("filesAnalyzed")})
    return errors


def language(path: Path) -> str:
    """Map repository file types to Codacy descriptor language identifiers."""
    if path.suffix.lower() in SUPPORTED_SUFFIXES:
        return "C" if path.suffix.lower() == ".c" else "CPP"
    return {".py": "Python", ".md": "Markdown", ".json": "JSON", ".yml": "YAML", ".yaml": "YAML",
            ".sh": "Shell", ".css": "CSS", ".html": "HTML", ".js": "Javascript",
            ".ts": "Typescript", ".jsx": "Javascript", ".tsx": "Typescript"}.get(path.suffix.lower(), "unknown")


def excluded(path: Path, config: dict) -> bool:
    """Honor the checked-in Codacy snapshot's excludes."""
    return any(fnmatch.fnmatchcase(path.as_posix(), pattern) for pattern in config.get("exclude", []))


def lizard_executable(installation: str | None = None) -> Path:
    """Match the current Codacy adapter's managed-venv-first resolution."""
    managed = Path.home() / ".codacy/runtimes/lizard-1/venv/bin/lizard"
    if installation != "global" and managed.is_file():
        return managed
    executable = shutil.which("lizard")
    if executable is None:
        raise AnalysisError("Lizard is unavailable; prepare Codacy analyzers first")
    return Path(executable)


def complexity(root: Path, files: list[Path], capability: dict, timeout: float) -> dict:
    """Measure raw CCN totals with the same executable/version as the issue pass."""
    if not files:
        return {"status": "not_applicable", "files": []}
    ready = next((item for item in capability.get("ready", []) if item.get("toolId") == "Lizard"), None)
    if ready is None:
        raise AnalysisError("No verified Codacy Lizard execution is available for complexity")
    executable = lizard_executable(ready.get("installation"))
    version = run([str(executable), "--version"], root, timeout)
    if version.returncode or version.stdout.strip() != ready.get("version"):
        raise AnalysisError("Complexity Lizard version differs from the Codacy issue analyzer")
    result = run([str(executable), "--csv", *[str(root / path) for path in files]], root, timeout)
    if result.returncode != 0 or result.stderr.strip():
        raise AnalysisError("Lizard could not complete raw complexity analysis")
    totals = {str(path): {"path": str(path), "cyclomaticComplexity": 0, "functions": []} for path in files}
    for row in csv.reader(io.StringIO(result.stdout)):
        if len(row) != 11:
            raise AnalysisError("Unsupported Lizard CSV format")
        path = str(Path(row[6]).resolve().relative_to(root))
        if path not in totals:
            raise AnalysisError("Lizard returned a file outside the selected set")
        function = {"name": row[7], "cyclomaticComplexity": int(row[1]), "nloc": int(row[0]),
                    "startLine": int(row[9]), "endLine": int(row[10])}
        totals[path]["functions"].append(function)
        totals[path]["cyclomaticComplexity"] += function["cyclomaticComplexity"]
    return {"status": "complete", "advisory": True, "version": ready["version"], "files": list(totals.values())}


def parse_clones(xml: str, mirror: Path, selected: set[str]) -> list[dict]:
    """Retain all occurrences of any clone touching a selected file."""
    if "<!DOCTYPE" in xml.upper() or "<!ENTITY" in xml.upper():
        raise AnalysisError("CPD returned unexpected XML declarations")
    try:
        document = element_tree.fromstring(xml)  # nosec B314
    except element_tree.ParseError as error:
        raise AnalysisError("CPD returned invalid XML") from error
    if document.tag != "pmd-cpd":
        raise AnalysisError("CPD returned an unexpected report root")
    if document.findall("error") or document.findall("processing-error"):
        raise AnalysisError("CPD reported skipped files or tokenization errors")
    clones = []
    for clone in document.findall("duplication"):
        occurrences = []
        for source in clone.findall("file"):
            path = str(Path(source.attrib["path"]).resolve().relative_to(mirror))
            start = int(source.attrib["line"])
            occurrences.append({"path": path, "startLine": start,
                                "endLine": start + int(clone.attrib["lines"]) - 1})
        if any(item["path"] in selected for item in occurrences):
            clones.append({"tokens": int(clone.attrib["tokens"]), "lines": int(clone.attrib["lines"]),
                           "occurrences": occurrences})
    return clones


def duplication(root: Path, mirror: Path, selected: list[Path], config: dict, info: dict,
                timeout: float) -> dict:
    """Run CPD over the full active C/C++ corpus and report selected-file matches."""
    if not selected:
        return {"status": "not_applicable", "clones": []}
    descriptor = info.get("PMD", {})
    version = descriptor.get("installedVersion")
    executable = Path.home() / f".codacy/tools/PMD/pmd-bin-{version}/bin/run.sh"
    java = Path.home() / ".codacy/runtimes/java-21/bin/java"
    if not executable.is_file():
        raise AnalysisError("Codacy-managed PMD CPD is missing; prepare the PMD analyzer first")
    env = dict(os.environ)
    if java.is_file():
        env["JAVA_HOME"] = str(java.parent.parent)
        env["PATH"] = f"{java.parent}{os.pathsep}{env.get('PATH', '')}"
    # Every path is explicit so generated caches, third-party sources and deleted files cannot enter CPD.
    corpus = [path for path in repository_files(root) if path.suffix.lower() in SUPPORTED_SUFFIXES and not excluded(path, config)]
    if any("\n" in str(path) or "\r" in str(path) for path in corpus):
        raise AnalysisError("CPD file lists cannot represent filenames containing newlines")
    filelist = mirror.parent / "cpd-files.txt"
    filelist.write_text("\n".join(str(mirror / path) for path in corpus) + "\n", encoding="utf-8")
    result = run([str(executable), "cpd", "--minimum-tokens", "50", "--language", "cpp",
                  "--ignore-literals", "--ignore-annotations", "--skip-lexical-errors",
                  "--file-list", str(filelist), "--format", "xml"], mirror, timeout, env)
    if result.returncode not in {0, 4} or result.stderr.strip():
        raise AnalysisError("CPD failed or reported skipped inputs; inspect analyzer setup")
    clones = parse_clones(result.stdout, mirror, {str(path) for path in selected})
    return {"status": "complete", "advisory": True, "estimate": True, "version": version,
            "minimumTokens": 50, "corpusFiles": len(corpus), "clones": clones}


def secrets(root: Path, files: list[Path], timeout: float) -> dict:
    """
    Run only the CLI's local secrets scanner.

    Do not retain scanner text: it can contain secret values. Exit status and a
    local inspection command are retained instead of leaking them into reports.
    """
    if not files:
        return {"status": "not_applicable", "issues": []}
    result = run(["sonar", "analyze", "secrets", *[str(root / path) for path in files]], root, timeout)
    if result.returncode not in {0, 1}:
        raise AnalysisError(f"Sonar secrets scan failed (exit {result.returncode})")
    if result.returncode == 0 and "No secrets found" not in result.stdout:
        raise AnalysisError("Secrets CLI did not confirm completion")
    return {"status": "issues_found" if result.returncode else "clean", "submitted": [str(path) for path in files],
            "issues": [{"message": "Secrets detected; inspect with sonar analyze secrets <selected paths> locally"}]
            if result.returncode else [], "cliExitCode": result.returncode}
