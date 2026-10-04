"""Exercise the production launch boundary, including diagnostics and module admission."""

import pathlib
import subprocess
import sys


def run_case(engine, arguments, expected_code):
    """Run a CTest-owned target directly and check its process-boundary diagnostics."""
    result = subprocess.run(
        [engine, "--run-network-product", "standalone", "2", *arguments],
        capture_output=True, text=True, timeout=30, check=False,
    )
    if result.returncode != expected_code:
        raise RuntimeError((arguments, result.returncode, result.stdout, result.stderr))
    if expected_code:
        if "horo-engine:" not in result.stderr:
            raise RuntimeError((arguments, result.stderr))
    elif "network product completed 2 frames" not in result.stdout:
        raise RuntimeError(result.stdout)


def main():
    engine, module, revision_file, source_root, native = sys.argv[1:]
    executable = pathlib.Path(engine)
    if not executable.is_absolute() or not executable.is_file():
        raise ValueError("CTest must supply its absolute built engine target")
    revision = pathlib.Path(revision_file).read_text(encoding="utf-8").strip()
    script = str(pathlib.Path(source_root) / "tests/fixtures/gameplay_physics/PhysicsContext.horo_script")
    native_args = ["--game-module", module, "game.tests", revision, "grant"]
    script_args = ["--game-script", script, script + ".meta", "game.tests", "grant"]
    invalid = [
        ["--unknown", *native_args[1:]],
        [*native_args[:-1], "implicit"],
        ["--game-module", "relative.so", *native_args[2:]],
        ["--game-module", module, "game.tests", "0", "grant"],
        ["--game-module", module, "game.tests", "bad", "grant"],
        ["--game-module", module, "game.tests", "1tail", "grant"],
        ["--game-module", module, "game.tests", "18446744073709551616", "grant"],
        ["--game-script", "relative.horo_script", *script_args[2:]],
        ["--game-script", script, "relative.meta", "game.tests", "grant"],
        native_args[:-1],
        [*native_args, "extra"],
    ]
    cases = [(arguments, 2) for arguments in invalid]
    # OFF must reject startup rather than silently select another solver.
    expected = 0 if native == "1" else 3
    for selection in (native_args, script_args):
        cases.extend([(selection, expected), ([*selection[:-1], "deny"], expected)])
    cases.append((["--game-script", script + ".missing", script + ".meta", "game.tests", "grant"], 3))
    cases.extend([
        (["--game-module", module, "", revision, "grant"], 3),
        (["--game-script", script, script + ".meta", "", "grant"], 3),
    ])
    for arguments, expected_code in cases:
        run_case(engine, arguments, expected_code)
    print(f"{len(cases)} production gameplay launch cases passed")


if __name__ == "__main__":
    main()
