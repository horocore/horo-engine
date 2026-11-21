"""Observe native source publication across a process exit and a fresh editor owner."""

import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


def run_probe(binary: Path, project: Path, phase: str, expected_exit: int = 0) -> None:
    environment = {**os.environ, "HORO_SOURCE_SAVE_TEST_PHASE": phase}
    # CTest supplies the validated binary in its configured test output directory.
    # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-audit.dangerous-subprocess-use-audit, python.lang.security.audit.dangerous-subprocess-use-tainted-env-args.dangerous-subprocess-use-tainted-env-args
    completed = subprocess.run(  # nosec B603
        [str(binary), "[source-save-process]"],
        cwd=project,
        env=environment,
        capture_output=True,
        timeout=10,
        check=False,
    )
    if completed.returncode != expected_exit:
        output = (completed.stdout + completed.stderr).decode(errors="replace")
        raise RuntimeError(f"{phase}: expected exit {expected_exit}, got {completed.returncode}\n{output}")


def check_bytes(path: Path, expected: bytes) -> None:
    if path.read_bytes() != expected:
        raise RuntimeError(f"Unexpected bytes in {path.name}")


def check_interruption(binary: Path, project: Path, phase: str) -> None:
    project.mkdir()
    (project / "source-save-test-fixture").touch(exist_ok=False)
    source = project / "kaynak ü 日本.cpp"
    temporary = source.with_name(source.name + ".horo-source.temporary")
    base = b"base\n"
    edited = "edited ü\r\n".encode()
    source.write_bytes(base)

    run_probe(binary, project, phase, 73)
    if phase == "before":
        check_bytes(source, base)
        check_bytes(temporary, edited)
        run_probe(binary, project, "restart-before")
        check_bytes(source, base)
        check_bytes(temporary, edited)
        temporary.unlink()
        run_probe(binary, project, "retry")
    else:
        run_probe(binary, project, "restart-after")
    check_bytes(source, edited)
    if temporary.exists():
        raise RuntimeError("The finished save left its staging file behind")


def main() -> None:
    if len(sys.argv) != 3:
        raise ValueError("CTest must supply the source test executable and its output directory")
    binary = Path(sys.argv[1]).resolve(strict=True)
    directory = Path(sys.argv[2]).resolve(strict=True)
    if binary.name not in {"HoroSourceFileOpenServiceTests", "HoroSourceFileOpenServiceTests.exe"} or binary.parent != directory:
        raise ValueError("Expected the source test executable from the configured test output directory")
    with TemporaryDirectory(prefix="horo-source-interruption-") as root:
        check_interruption(binary, Path(root) / "önce", "before")
        check_interruption(binary, Path(root) / "sonra", "after")


if __name__ == "__main__":
    main()
