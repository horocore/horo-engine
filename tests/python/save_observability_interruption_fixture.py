"""Verify a support bundle produced by a fresh host after an interrupted Save session."""

import json
import os
from pathlib import Path
import subprocess  # nosec B404
import sys
from tempfile import TemporaryDirectory
from zipfile import ZipFile


def run_probe(binary: Path, directory: Path, phase: str, expected_exit: int) -> None:
    # CTest supplies the executable from the configured test output directory.
    # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit
    completed = subprocess.run(  # nosec B603
        [str(binary), "[save-observability-process]"],
        cwd=directory,
        env={**os.environ, "HORO_SAVE_OBSERVABILITY_PHASE": phase},
        capture_output=True,
        timeout=15,
        check=False,
    )
    if completed.returncode != expected_exit:
        output = (completed.stdout + completed.stderr).decode(errors="replace")
        raise RuntimeError(f"{phase}: expected exit {expected_exit}, got {completed.returncode}\n{output}")


def check_restart(binary: Path, directory: Path) -> None:
    run_probe(binary, directory, "interrupt", 73)
    previous = json.loads((directory / "save.session.json").read_bytes())
    if (directory / "save.shutdown.json").exists() or "interruptedSessionId" in previous:
        raise RuntimeError("The first process unexpectedly recorded a clean or prior session")
    private_marker = b"private-interrupted-save-value"
    if private_marker not in (directory / "save.jsonl").read_bytes():
        raise RuntimeError("The first process did not persist the Save privacy fixture")

    run_probe(binary, directory, "export", 0)
    with ZipFile(directory / "bundle.zip") as bundle:
        current = json.loads(bundle.read("metadata/session.json"))
        if current.get("interruptedSessionId") != previous["sessionId"] or current["sessionId"] == previous["sessionId"]:
            raise RuntimeError("The bundle did not associate the interrupted session with the new session")
        if current["previousCleanShutdown"] is not False:
            raise RuntimeError("The bundle reports a clean shutdown after interruption")
        if b"interrupted-control-log" not in bundle.read("logs/save.jsonl"):
            raise RuntimeError("The bundle lost the preceding session's unrelated log")
        if any(private_marker in bundle.read(name) for name in bundle.namelist()):
            raise RuntimeError("A private Save value escaped into the bundle")

    (directory / "bundle.zip").unlink()
    run_probe(binary, directory, "export", 0)
    with ZipFile(directory / "bundle.zip") as bundle:
        clean = json.loads(bundle.read("metadata/session.json"))
        if clean["previousCleanShutdown"] is not True or "interruptedSessionId" in clean:
            raise RuntimeError("The bundle confused a later clean shutdown with the earlier interruption")


def check_invalid_marker(binary: Path, directory: Path) -> None:
    malformed = "8" * 80 + "-1-2"
    (directory / "save.session.json").write_text(
        json.dumps({"sessionId": malformed}, separators=(",", ":")), encoding="utf-8"
    )
    run_probe(binary, directory, "export", 0)
    with ZipFile(directory / "bundle.zip") as bundle:
        current = json.loads(bundle.read("metadata/session.json"))
        if "interruptedSessionId" in current:
            raise RuntimeError("The bundle carried forward a malformed prior session identity")


def main() -> None:
    if len(sys.argv) != 3:
        raise ValueError("CTest must supply the Save telemetry test executable and its output directory")
    binary = Path(sys.argv[1]).resolve(strict=True)
    output_dir = Path(sys.argv[2]).resolve(strict=True)
    if binary.stem != "HoroRuntimeSaveTelemetryTests" or binary.parent != output_dir:
        raise ValueError("Expected the Save telemetry test executable in its configured output directory")
    with TemporaryDirectory(prefix="horo-save-observability-") as root:
        for check in (check_restart, check_invalid_marker):
            directory = Path(root) / check.__name__
            directory.mkdir()
            (directory / "save-observability-fixture").touch(exist_ok=False)
            check(binary, directory)


if __name__ == "__main__":
    main()
