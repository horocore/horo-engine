"""Prepare worktree compiler context and require evidence of real C++ analysis."""

from __future__ import annotations

import json
import re
import shlex
import sys
import time
from pathlib import Path

from quality_support import CANCELLED, digest, read_json, run, write_json
from sonar_ide_analysis import (AnalysisError, TRANSLATION_UNIT_SUFFIXES, _load_compilation_database,
                                validate_compile_commands)
from sonar_sessions import SessionPool, log_delta, log_offsets, logs


def compiler_entries(database: Path, root: Path) -> list[dict]:
    """Resolve worktree translation units, object targets and include search paths."""
    result = []
    for entry in _load_compilation_database(database):
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            raise AnalysisError("Malformed compilation database entry")
        directory = Path(entry.get("directory", str(database.parent))).resolve()
        source = (directory / entry["file"]).resolve()
        if not source.is_relative_to(root):
            continue
        argv = entry.get("arguments") or shlex.split(entry.get("command", ""))
        includes = [source.parent]
        output = None
        for index, arg in enumerate(argv):
            if arg == "-o" and index + 1 < len(argv):
                output = (directory / argv[index + 1]).resolve()
            if arg in {"-I", "-isystem", "-iquote"} and index + 1 < len(argv):
                includes.append((directory / argv[index + 1]).resolve())
            elif arg.startswith("-I") and len(arg) > 2:
                includes.append((directory / arg[2:]).resolve())
        if output is None or not output.is_relative_to(database.parent):
            raise AnalysisError(f"No in-build object target for {source.relative_to(root)}")
        result.append({"source": source, "target": str(output.relative_to(database.parent)), "includes": includes})
    return result


def header_context(entries: list[dict], headers: list[Path], root: Path) -> dict[Path, dict]:
    """
    Find a translation unit including each header through its include chain.

    This conservatively traverses literal includes, including conditional branches.
    The subsequent build and IDE sensor evidence validate the chosen context.
    Headers without a provable include chain are never declared clean.
    """
    include_pattern = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.MULTILINE)
    contents = {}
    found = {}
    if any(not path.is_file() for path in headers):
        raise AnalysisError("Selected header is missing")
    for entry in entries:
        if CANCELLED.is_set():
            raise AnalysisError("Header context search cancelled")
        pending = [entry["source"]]
        visited = set()
        while pending:
            path = pending.pop()
            if (path in visited or not path.is_file() or not path.is_relative_to(root)
                    or any(part in {"build", "vendor", "deprecated", "_deps"} for part in path.relative_to(root).parts)):
                continue
            visited.add(path)
            if path in headers:
                found.setdefault(path, entry)
            if path not in contents:
                contents[path] = include_pattern.findall(path.read_text(encoding="utf-8", errors="replace"))
            for include in contents[path]:
                for directory in [path.parent, *entry["includes"]]:
                    candidate = (directory / include).resolve()
                    if candidate.is_file():
                        candidate = original_header(candidate, root)
                        pending.append(candidate)
                        break
        if all(header in found for header in headers):
            return found
    missing = [str(path.relative_to(root)) for path in headers if path not in found]
    raise AnalysisError("No translation unit context for header(s): " + ", ".join(missing))


def original_header(candidate: Path, root: Path) -> Path:
    """Map verified CMake staged public copies to their owning source header."""
    if "target-includes" not in candidate.parts:
        return candidate
    staged = candidate.parts.index("target-includes")
    if candidate.parts[staged + 2:staged + 3] != ("public",):
        return candidate
    original = root / "include" / Path(*candidate.parts[staged + 3:])
    if original.is_file() and digest(original) == digest(candidate):
        return original
    return candidate


def prepare(root: Path, cache: Path, files: list[Path], existing: Path | None,
            timeout: float) -> tuple[Path, dict[Path, dict]]:
    """Let CMake refresh stale inputs and build only selected object targets."""
    if existing is None:
        existing = reusable_database(root, files)
    build = existing.parent.resolve() if existing else cache / "build"
    cmake_cache = build / "CMakeCache.txt"
    if existing:
        if not existing.is_file() or not cmake_cache.is_file():
            raise AnalysisError("Existing compile database requires its CMake build directory")
        home = next((line.split("=", 1)[1] for line in cmake_cache.read_text().splitlines()
                     if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=")), None)
        if home is None or Path(home).resolve() != root:
            raise AnalysisError("Compilation database belongs to a different worktree")
    command = ["cmake", "-S", str(root), "-B", str(build), "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
    if not existing:
        command.extend(["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug", "-DBUILD_TESTING=ON",
                        "-DHORO_BUILD_RENDER_VULKAN=ON", "-DHORO_ENABLE_IMGUI_UI_TESTS=ON",
                        "-DHORO_ENABLE_OPENTELEMETRY=ON", "-DHORO_BUILD_NETWORK_GNS=ON"])
    configured = run(command, root, timeout)
    if configured.returncode:
        raise AnalysisError("CMake configuration failed; " + configured.stderr[-2000:])
    database = build / "compile_commands.json"
    validate_compile_commands(root, database, files)
    entries = compiler_entries(database, root)
    headers = [path for path in files if path.suffix.lower() not in TRANSLATION_UNIT_SUFFIXES]
    contexts = header_context(entries, headers, root) if headers else {}
    requested = {path for path in files if path.suffix.lower() in TRANSLATION_UNIT_SUFFIXES}
    targets = {entry["target"] for entry in entries if entry["source"] in requested}
    targets.update(entry["target"] for entry in contexts.values())
    if targets:
        # Make exposes object rules in subdirectory Makefiles, Ninja at its root.
        make = "CMAKE_GENERATOR:INTERNAL=Unix Makefiles" in cmake_cache.read_text()
        if make:
            owners = {target.split("CMakeFiles/", 1)[1].split(".dir/", 1)[0] for target in targets}
            commands = [["cmake", "--build", str(build), "--parallel", "2", "--target", *sorted(owners)]]
        else:
            commands = [["cmake", "--build", str(build), "--parallel", "2", "--target", *sorted(targets)]]
        for command in commands:
            built = run(command, root, timeout)
            if built.returncode:
                raise AnalysisError("Compiler context preparation failed; " + (built.stderr or built.stdout)[-2000:])
    return database, contexts


def reusable_database(root: Path, files: list[Path]) -> Path | None:
    """Prefer the canonical skeleton build only after checking worktree identity."""
    database = root / "build/skeleton/compile_commands.json"
    cache = database.parent / "CMakeCache.txt"
    if not database.is_file() or not cache.is_file():
        return None
    home = next((line.split("=", 1)[1] for line in cache.read_text().splitlines()
                 if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=")), None)
    if home is None or Path(home).resolve() != root:
        return None
    try:
        validate_compile_commands(root, database, files)
    except AnalysisError:
        return None
    return database


def verify_evidence(text: str, files: list[Path]) -> dict:
    """Require file-specific CFamily execution, active rules, and no sensor error."""
    # Only the explicit no-indexed-files retry may precede the completed request.
    # All sensor errors after its last occurrence remain fatal.
    retry = text.rfind("No files were found to be indexed by SonarQube for IDE")
    if retry >= 0:
        next_request = text.find("Analyzing list of", retry)
        if next_request >= 0:
            text = text[next_request:]
    branch_warning = "Couldn't access repository for path" in text
    # JGit in the IDE can fail to discover linked-worktree metadata. This affects
    # cloud branch matching, not CFamily compilation context; retain the warning.
    sensor_text = "\n".join(line for line in text.splitlines()
                            if "Couldn't access repository for path" not in line)
    if "[Error -" in sensor_text or re.search(r"(?i)(failed to analy[sz]e|could not analy[sz]e|not set to a valid|not analyzed)", sensor_text):
        raise AnalysisError("Sonar log reports analysis errors; no clean result can be inferred")
    counts = [int(value) for value in re.findall(r"(\d+) compilation units analyzed", text)]
    rules = [int(value) for value in re.findall(r"(\d+) (?:cpp|c)[,\]]", text)]
    if not counts or max(counts) < 1 or not rules or max(rules) < 1:
        raise AnalysisError("No positive CFamily sensor execution with active C/C++ rules in this request")
    if "Analysis detected" not in text or any(path.as_uri() not in text for path in files):
        raise AnalysisError("Sonar did not confirm completion for all submitted files")
    return {"compilationUnits": max(counts), "activeCppRules": True, "completion": True,
            "branchMatching": "unavailable" if branch_warning else "no_error_reported"}


def binding_evidence(profile: Path, project: str) -> bool:
    """Require the connected server's readiness acknowledgement for this project."""
    pattern = re.compile(r"isReadyForAnalysis\(connectionId: [^,]+, sonarProjectKey: "
                         + re.escape(project) + r", plugins: [^\n]+\) => (true|false)")
    paths = logs(profile)
    if not paths:
        return False
    latest = max(path.relative_to(profile).parts[1] for path in paths)
    text = "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in paths
                     if path.relative_to(profile).parts[1] == latest)
    matches = pattern.findall(text)
    return bool(matches and matches[-1] == "true")


def wait_binding(profile: Path, project: str, timeout: float) -> None:
    """Wait for initial connected-mode synchronization before requesting analysis."""
    deadline = time.monotonic() + timeout
    while not binding_evidence(profile, project):
        if CANCELLED.is_set():
            raise AnalysisError("Connected-mode synchronization cancelled")
        if time.monotonic() >= deadline:
            raise AnalysisError("Connected-mode project synchronization did not complete before timeout")
        time.sleep(0.5)


def request_analysis(root: Path, port: int, files: list[Path], timeout: float, startup: float) -> list[dict]:
    """Keep an in-flight HTTP request cancellable using an owned worker process."""
    worker = ("import json,sys; from pathlib import Path; "
              "sys.path.insert(0,sys.argv[1]); "
              "from sonar_ide_analysis import analyze_when_indexed; "
              "print(json.dumps(analyze_when_indexed(int(sys.argv[2]), "
              "[Path(p) for p in json.loads(sys.argv[3])], float(sys.argv[4]), float(sys.argv[5]))))")
    result = run([sys.executable, "-c", worker, str(Path(__file__).parent), str(port),
                  json.dumps([str(path) for path in files]), str(timeout), str(startup)], root, timeout)
    if result.returncode:
        raise AnalysisError("Sonar bridge request failed: " + result.stderr[-1500:])
    findings = json.loads(result.stdout)
    if not isinstance(findings, list) or not all(isinstance(item, dict) for item in findings):
        raise AnalysisError("Sonar worker returned malformed findings")
    return findings


def analyze_files(root: Path, record: dict, contexts: dict, files: list[Path], args) -> tuple[list, list]:
    """Collect file-specific sensor evidence while the caller holds the lease."""
    profile = Path(record["profile"])
    findings, evidence = [], []
    for path in files:
        submitted = [path]
        if path in contexts:
            submitted.insert(0, contexts[path]["source"])
        before = log_offsets(profile)
        result = request_analysis(root, record["port"], submitted, args.timeout, args.startup_timeout)
        deadline = time.monotonic() + 5
        while True:
            try:
                proof = verify_evidence(log_delta(profile, before), submitted)
                break
            except AnalysisError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)
        for finding in result:
            reported = finding.get("filePath")
            if isinstance(reported, str) and Path(reported).is_absolute():
                mapped = original_header(Path(reported), root)
                if str(mapped) != reported:
                    finding = {**finding, "filePath": str(mapped), "reportedFilePath": reported}
            findings.append(finding)
        evidence.append({"path": str(path.relative_to(root)), **proof})
    return findings, evidence


def check(root: Path, cache: Path, repository_cache: Path, files: list[Path], args) -> dict:
    """Hold the session lease through all requests and retain all findings."""
    if not files:
        return {"status": "not_applicable", "findings": []}
    database, contexts = prepare(root, cache, files, args.compile_commands, args.timeout)
    workspace = cache / "sonar.code-workspace"
    write_json(workspace, {"folders": [{"path": str(root)}], "settings": {
        "sonarlint.pathToCompileCommands": str(database), "sonarlint.connectedMode.project": {
            "connectionId": args.connection_id, "projectKey": args.project_key}}})
    pool = SessionPool(repository_cache, root, args.timeout)
    configuration = [str(database), digest(database), args.project_key, args.connection_id]
    with pool.lease() as (slot, previous):
        if previous and previous.get("configuration") != configuration:
            from sonar_sessions import retire
            retire(previous)
            previous = {}
        record = pool.open(slot, previous, workspace, args.code_user_data, args.extensions_directory,
                           args.startup_timeout, files[0])
        record["database"] = str(database)
        record["configuration"] = configuration
        write_json(pool.directory / f"{slot}.json", record)
        profile = Path(record["profile"])
        if not record.get("bindingReady"):
            wait_binding(profile, args.project_key, args.timeout)
            record["bindingReady"] = True
            write_json(pool.directory / f"{slot}.json", record)
        findings, evidence = analyze_files(root, record, contexts, files, args)
        if not record.get("bindingReady"):
            raise AnalysisError("Connected-mode project cache is missing; bind/authenticate the managed Sonar profile")
        extension_versions = [read_json(path)["version"] for path in (profile.parent / "extensions").glob("*/package.json")]
    selected = {str(path) for path in files}
    contextual = [item for item in findings if item.get("filePath") not in selected]
    unique = {json.dumps(item, sort_keys=True): item for item in findings if item.get("filePath") in selected}
    stale = digest(database) != configuration[1]
    return {"status": "incomplete" if stale else ("issues_found" if unique else "clean"),
            "stale": stale, "error": "Compilation database changed during analysis" if stale else None,
            "findings": list(unique.values()),
            "contextFindings": contextual, "evidence": evidence, "compileCommands": str(database),
            "bridge": {"port": record["port"], "pid": record["pid"], "started": record["started"]},
            "versions": extension_versions, "backend": "local_sonar_ide", "connectedMode": True,
            "projectKey": args.project_key, "connectionId": args.connection_id,
            "configurationIdentity": configuration,
            "warnings": ["IDE could not match the linked-worktree Git branch; cloud branch parity is unverified"]
            if any(item["branchMatching"] == "unavailable" for item in evidence) else [],
            "submitted": [str(path.relative_to(root)) for path in files]}
