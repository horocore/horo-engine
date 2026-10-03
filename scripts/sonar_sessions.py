"""
Linux-only owned VS Code sessions for the Sonar IDE bridge.

Slot leases serialize one session's requests. The registry lock protects slot
assignment and eviction; it is never held during an analysis request. Ownership
requires both /proc start time and the exact private --user-data-dir argument.
"""


from __future__ import annotations

import contextlib
import json
import os
import re
import signal
import shutil
import sqlite3
import time
import uuid
from pathlib import Path

from quality_support import CANCELLED, identity, lock, read_json, run, write_json
from sonar_ide_analysis import AnalysisError, BRIDGE_PORTS, BRIDGE_STATUS_PATH, request_bridge


def processes() -> dict[int, dict]:
    """Read process identity without exposing arguments to reports."""
    result = {}
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            fields = (entry / "stat").read_text().rsplit(") ", 1)[1].split()
            argv = (entry / "cmdline").read_bytes().split(b"\0")
            markers = {}
            for variable in (entry / "environ").read_bytes().split(b"\0"):
                if variable.startswith((b"HORO_QUALITY_SESSION=", b"HORO_QUALITY_PROFILE=")):
                    key, value = variable.split(b"=", 1)
                    markers[os.fsdecode(key)] = os.fsdecode(value)
            result[int(entry.name)] = {"parent": int(fields[1]), "started": fields[19],
                                       "argv": [os.fsdecode(arg) for arg in argv if arg], "markers": markers,
                                       "executable": os.readlink(entry / "exe")}
        except (OSError, ValueError, IndexError):
            continue
    return result


def has_profile(process: dict, profile: Path) -> bool:
    """Match a complete argument, never a substring of an unrelated command."""
    argv = process["argv"]
    return f"--user-data-dir={profile}" in argv or any(
        arg == "--user-data-dir" and argv[index + 1:index + 2] == [str(profile)]
        for index, arg in enumerate(argv))


def owned(record: dict, table: dict[int, dict]) -> bool:
    """Reject exited/reused PIDs and records no longer naming our profile."""
    process = table.get(record.get("pid"))
    if not process or process["started"] != record.get("started") or not record.get("nonce"):
        return False
    if process.get("executable") != record.get("executable"):
        return False
    if record.get("title") and process["argv"] != [record["title"]]:
        return False
    return any(table[pid].get("markers", {}).get("HORO_QUALITY_SESSION") == record["nonce"]
               and table[pid].get("markers", {}).get("HORO_QUALITY_PROFILE") == record["profile"]
               for pid in descendants(record["pid"], table))


def descendants(pid: int, table: dict[int, dict]) -> set[int]:
    """Collect only children of the verified Code main process."""
    result = {pid}
    while True:
        added = {child for child, data in table.items() if data["parent"] in result} - result
        if not added:
            return result
        result.update(added)


def listener_port(pids: set[int]) -> tuple[int, int] | None:
    """Match loopback listening sockets to file descriptors in the owned tree."""
    sockets = {}
    for name in ("tcp", "tcp6"):
        for line in Path(f"/proc/net/{name}").read_text().splitlines()[1:]:
            fields = line.split()
            address, port_hex = fields[1].split(":")
            port = int(port_hex, 16)
            loopback = address in {"0100007F", "00000000000000000000000001000000",
                                   "0000000000000000FFFF00000100007F"}
            if fields[3] == "0A" and port in BRIDGE_PORTS and loopback:
                sockets[fields[9]] = port
    matches = set()
    for pid in pids:
        try:
            for descriptor in Path(f"/proc/{pid}/fd").iterdir():
                try:
                    target = os.readlink(descriptor)
                except OSError:
                    continue
                match = re.fullmatch(r"socket:\[(\d+)\]", target)
                if match and match.group(1) in sockets:
                    matches.add((pid, sockets[match.group(1)]))
        except OSError:
            continue
    if len(matches) > 1:
        raise AnalysisError("Owned Code session has ambiguous Sonar listeners")
    return next(iter(matches), None)


def terminate(record: dict) -> bool:
    """Pin validated processes with pidfds so PID reuse cannot kill another app."""
    table = processes()
    if not owned(record, table):
        return False
    descriptors = []
    try:
        for pid in descendants(record["pid"], table):
            try:
                descriptor = os.pidfd_open(pid)
            except ProcessLookupError:
                continue
            current = processes().get(pid)
            if not current or current["started"] != table[pid]["started"]:
                os.close(descriptor)
                continue
            descriptors.append(descriptor)
        for descriptor in descriptors:
            with contextlib.suppress(ProcessLookupError):
                signal.pidfd_send_signal(descriptor, signal.SIGTERM)
        time.sleep(0.2)
        for descriptor in descriptors:
            with contextlib.suppress(ProcessLookupError):
                signal.pidfd_send_signal(descriptor, signal.SIGKILL)
    finally:
        for descriptor in descriptors:
            os.close(descriptor)
    return True


def retire(record: dict) -> bool:
    """Keep a live but unverifiable slot reserved instead of orphaning it."""
    stopped = terminate(record)
    process = processes().get(record.get("pid"))
    if not stopped and process and process["started"] == record.get("started"):
        raise AnalysisError("Managed session is still alive but ownership cannot be verified; slot remains reserved")
    return stopped


def copy_encrypted_credentials(profile: Path, source: Path) -> None:
    """Copy only Sonar encrypted keychain references from a read-only database."""
    storage = profile / "User/globalStorage"
    source_db = source / "User/globalStorage/state.vscdb"
    if source_db.is_file() and not (storage / "state.vscdb").exists():
        with sqlite3.connect(f"{source_db.as_uri()}?mode=ro", uri=True) as original:
            encrypted = []
            for key, value in original.execute("SELECT key, value FROM ItemTable WHERE key LIKE 'secret://%'"):
                try:
                    metadata = json.loads(key.removeprefix("secret://"))
                except ValueError:
                    continue
                if metadata.get("extensionId", "").lower() == "sonarsource.sonarlint-vscode":
                    encrypted.append((key, value))
        with sqlite3.connect(storage / "state.vscdb") as destination:
            destination.execute("CREATE TABLE ItemTable (key TEXT UNIQUE ON CONFLICT REPLACE, value BLOB)")
            destination.executemany("INSERT INTO ItemTable VALUES (?, ?)", encrypted)
        os.chmod(storage / "state.vscdb", 0o600)


def seed_profile(profile: Path, source: Path, extensions: Path) -> None:
    """
    Copy connection metadata and only Sonar's encrypted SecretStorage records.

    The source settings/database are read-only. Plaintext tokens are never copied;
    keychain decryption remains VS Code's responsibility. Profiles share the same
    OS keychain service but no mutable workspace or analyzer storage.
    """
    user = profile / "User"
    user.mkdir(parents=True, exist_ok=True, mode=0o700)
    settings_path = source / "User/settings.json"
    try:
        settings = read_json(settings_path)
    except AnalysisError as error:
        raise AnalysisError(f"A JSON VS Code settings file is required at {settings_path}") from error
    # Disable trust prompts only in our isolated profile, before Code starts.
    copied = {"security.workspace.trust.enabled": False, "extensions.autoUpdate": False,
              "extensions.autoCheckUpdates": False, "sonarlint.output.showVerboseLogs": True,
              "sonarlint.automaticAnalysis": False, "window.restoreWindows": "none",
              "telemetry.telemetryLevel": "off", "workbench.startupEditor": "none"}
    # The language server otherwise shares ~/.sonarlint even across Code profiles.
    sonar_home = profile.parent / "sonar-home"
    sonar_home.mkdir(exist_ok=True, mode=0o700)
    copied["sonarlint.ls.vmargs"] = f'"-Duser.home={sonar_home}"'
    exclusions = {f"**/{name}/**": True for name in
                  ("build", "cmake-build-*", "vendor", "deprecated", ".git", "node_modules", ".venv")}
    copied["files.exclude"] = exclusions
    copied["files.watcherExclude"] = exclusions
    copied["search.exclude"] = exclusions
    for kind in ("sonarcloud", "sonarqube"):
        key = f"sonarlint.connectedMode.connections.{kind}"
        copied[key] = [{k: v for k, v in connection.items() if k in
                        {"connectionId", "organizationKey", "serverUrl", "region", "disableNotifications"}}
                       for connection in settings.get(key, [])]
    write_json(user / "settings.json", copied)
    storage = user / "globalStorage"
    storage.mkdir(exist_ok=True, mode=0o700)
    copy_encrypted_credentials(profile, source)
    installed = sorted(extensions.glob("sonarsource.sonarlint-vscode-*/package.json"),
                       key=lambda path: path.stat().st_mtime, reverse=True)
    if not installed:
        raise AnalysisError("SonarQube for IDE is not installed in the supplied extensions directory")
    extension_dir = profile.parent / "extensions"
    extension_dir.mkdir(exist_ok=True, mode=0o700)
    link = extension_dir / installed[0].parent.name
    if link.is_symlink():
        link.unlink()
    if not link.exists():
        shutil.copytree(installed[0].parent, link)


def logs(profile: Path) -> list[Path]:
    """Return only Sonar logs beneath the private profile."""
    return sorted(profile.glob("logs/**/SonarQube for IDE*.log"), key=lambda path: path.stat().st_mtime_ns)


def log_offsets(profile: Path) -> dict[int, int]:
    """Record byte offsets before submitting a request."""
    return {path.stat().st_ino: path.stat().st_size for path in logs(profile)}


def log_delta(profile: Path, offsets: dict[int, int]) -> str:
    """Read only diagnostics emitted since the current request began."""
    parts = []
    for path in logs(profile):
        with path.open("rb") as stream:
            stream.seek(offsets.get(os.fstat(stream.fileno()).st_ino, 0))
            parts.append(stream.read().decode("utf-8", errors="replace"))
    return "\n".join(parts)


def discover_main(table: dict, profile: Path, extension: Path, workspace: Path, nonce: str) -> list:
    """Identify the launched Code ancestor through its marked extension host."""
    main = []
    for witness in table.values():
        if (witness.get("markers", {}).get("HORO_QUALITY_PROFILE") != str(profile)
                or witness.get("markers", {}).get("HORO_QUALITY_SESSION") != nonce):
            continue
        parent = witness["parent"]
        seen = set()
        while parent in table and parent not in seen:
            seen.add(parent)
            process = table[parent]
            expected_title = (f"{process['executable']} --user-data-dir --extensions-dir "
                              f"--extensionDevelopmentPath --new-window {profile} "
                              f"{profile.parent / 'extensions'} {extension} {workspace}")
            if Path(process["executable"]).name == "code" and (has_profile(process, profile)
                    or process["argv"] == [expected_title]):
                if not any("--type=" in arg for arg in process["argv"]):
                    main.append((parent, process, witness["markers"]["HORO_QUALITY_SESSION"]))
            parent = process["parent"]
    return main


def launch_profile(root: Path, profile: Path, workspace: Path, source: Path, extensions: Path,
                   timeout: float, activation_file: Path) -> tuple[str, Path]:
    """Launch the released Sonar extension in a private Code development host."""
    seed_profile(profile, source, extensions)
    nonce = uuid.uuid4().hex
    env = dict(os.environ)
    env.pop("VSCODE_IPC_HOOK_CLI", None)
    env["HORO_QUALITY_SESSION"] = nonce
    env["HORO_QUALITY_PROFILE"] = str(profile)
    extension = next((profile.parent / "extensions").glob("sonarsource.sonarlint-vscode-*"))
    # Force activation of the existing released extension in this isolated host;
    # newer Code builds can otherwise inherit a shared disabled-extension list.
    run(["code", "--user-data-dir", str(profile), "--extensions-dir", str(profile.parent / "extensions"),
         "--extensionDevelopmentPath", str(extension),
         "--new-window", str(workspace)], root, timeout, env)
    run(["code", "--user-data-dir", str(profile), "--extensions-dir", str(profile.parent / "extensions"),
         "--reuse-window", "--goto", str(activation_file)], root, timeout, env)
    return nonce, extension


class SessionPool:

    """Lease up to two isolated sessions per common Git repository."""

    def __init__(self, cache: Path, root: Path, timeout: float = 600):
        """Scope capacity to the repository and sessions to individual worktrees."""
        self.directory = cache / "sessions"
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.root = root
        self.timeout = timeout

    def acquire(self):
        """Claim an idle slot or wait while both sessions are leased."""
        import fcntl

        deadline = time.monotonic() + self.timeout
        stream = None
        while stream is None:
            if CANCELLED.is_set():
                raise AnalysisError("Sonar session acquisition cancelled")
            with lock(self.directory / "registry.lock", self.timeout):
                records = []
                for slot in range(2):
                    path = self.directory / f"{slot}.json"
                    record = read_json(path) if path.exists() else {}
                    records.append((slot, record))
                records.sort(key=lambda item: (item[1].get("worktree") != str(self.root),
                                               item[1].get("usedAt", 0)))
                for slot, record in records:
                    candidate = (self.directory / f"{slot}.lock").open("a", encoding="utf-8")
                    try:
                        fcntl.flock(candidate, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        candidate.close()
                        continue
                    if record.get("worktree") != str(self.root):
                        try:
                            retire(record)
                        except AnalysisError:
                            candidate.close()
                            raise
                        record = {}
                    stream = candidate
                    break
            if stream is None:
                if time.monotonic() >= deadline:
                    raise AnalysisError("Timed out waiting for one of two Sonar sessions")
                time.sleep(0.1)
        return slot, record, stream

    @contextlib.contextmanager
    def lease(self):
        """Release the lease and retire interrupted analyses on every exit."""
        import fcntl

        slot, record, stream = self.acquire()
        try:
            yield slot, record
        except BaseException:
            # Abandon the server too: cancelling an HTTP client does not cancel
            # the IDE's queued analysis, which must not leak into the next lease.
            path = self.directory / f"{slot}.json"
            if path.exists():
                retire(read_json(path))
                path.unlink()
            raise
        finally:
            try:
                path = self.directory / f"{slot}.json"
                if path.exists():
                    record = read_json(path)
                    record["usedAt"] = time.time()
                    write_json(path, record)
            finally:
                fcntl.flock(stream, fcntl.LOCK_UN)
                stream.close()

    def open(self, slot: int, record: dict, workspace: Path, source: Path, extensions: Path,
             startup_timeout: float, activation_file: Path) -> dict:
        """Validate an existing session or launch one with owned process discovery."""
        profile = self.directory / identity(self.root) / "profile"
        if record and record.get("isolationVersion") == 3 and owned(record, processes()):
            table = processes()
            listener = listener_port(descendants(record["pid"], table))
            if (listener and listener == (record.get("listenerPid"), record.get("port"))
                    and table[listener[0]]["started"] == record.get("listenerStarted")):
                request_bridge(listener[1], BRIDGE_STATUS_PATH, 2)
                return record
            retire(record)
        elif record:
            retire(record)
        nonce, extension = launch_profile(self.root, profile, workspace, source, extensions,
                                          startup_timeout, activation_file)
        deadline = time.monotonic() + startup_timeout
        try:
            while time.monotonic() < deadline:
                if CANCELLED.is_set():
                    raise AnalysisError("Sonar startup cancelled")
                table = processes()
                main = discover_main(table, profile, extension, workspace, nonce)
                if main:
                    pid, process, launch_nonce = min(main, key=lambda item: int(item[1]["started"]))
                    record = {"pid": pid, "started": process["started"], "profile": str(profile), "nonce": launch_nonce,
                              "isolationVersion": 3,
                              "executable": process["executable"],
                              "worktree": str(self.root), "workspace": str(workspace), "usedAt": time.time()}
                    if len(process["argv"]) == 1:
                        record["title"] = process["argv"][0]
                    write_json(self.directory / f"{slot}.json", record)
                    listener = listener_port(descendants(pid, table))
                    if listener:
                        record.update(listenerPid=listener[0], port=listener[1],
                                      listenerStarted=table[listener[0]]["started"])
                        request_bridge(listener[1], BRIDGE_STATUS_PATH, 2)
                        write_json(self.directory / f"{slot}.json", record)
                        return record
                time.sleep(0.2)
            raise AnalysisError("Managed VS Code did not expose an owned Sonar bridge before timeout")
        except BaseException:
            terminate(record)
            raise

    def stop(self) -> list[dict]:
        """Leave busy sessions and all unrelated user processes untouched."""
        import fcntl

        result = []
        with lock(self.directory / "registry.lock", self.timeout):
            for slot in range(2):
                with (self.directory / f"{slot}.lock").open("a", encoding="utf-8") as stream:
                    try:
                        fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        result.append({"slot": slot, "status": "busy"})
                        continue
                    path = self.directory / f"{slot}.json"
                    if path.exists():
                        try:
                            stopped = retire(read_json(path))
                        except AnalysisError as error:
                            result.append({"slot": slot, "status": "unverified", "error": str(error)})
                            continue
                        path.unlink()
                        result.append({"slot": slot, "status": "stopped" if stopped else "already_exited"})
        return result
