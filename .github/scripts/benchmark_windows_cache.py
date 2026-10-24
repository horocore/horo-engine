"""One-off same-object Windows experiment; this diagnostic branch is never merged."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import time


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/ci-audio-release"
RESULTS = ROOT / "build/windows-cache-benchmark"
GROUPS = ("src/foundation/", "src/runtime/ui/", "src/physics/", "src/runtime/navigation/")


def run(arguments, output, environment):
    """Execute only fixed toolchain commands; generated target names stay argv data."""
    assert arguments[0] in {"cmake", "sccache"}
    executable = shutil.which(arguments[0])
    if executable is None:
        raise RuntimeError("Required benchmark tool is missing")
    with output.open("w", encoding="utf-8") as log:
        subprocess.run(arguments, executable=executable, cwd=ROOT, env=environment, stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=180, shell=False)


def select_objects():
    """Select equal small source groups from actual generated compile commands."""
    commands = json.loads((BUILD / "compile_commands.json").read_text(encoding="utf-8"))
    selected = {}
    for group in GROUPS:
        count = 0
        for command in sorted(commands, key=lambda entry: (entry["file"], entry["output"])):
            source = Path(command["file"]).resolve()
            if not source.is_relative_to(ROOT) or not source.relative_to(ROOT).as_posix().startswith(group):
                continue
            if not Path(command["directory"]).resolve().is_relative_to(BUILD):
                continue
            output = (BUILD / command["output"]).resolve()
            assert output.is_relative_to(BUILD)
            assert output.suffix in {".obj", ".o"}
            target = output.relative_to(BUILD).as_posix()
            selected.setdefault(target, source.relative_to(ROOT).as_posix())
            count += 1
            if count == 12:
                break
        assert count == 12, group
    assert len(selected) == 48
    return selected


def digests(targets):
    """Prove the resulting real object bytes remain identical between modes."""
    result = {}
    for target in targets:
        with (BUILD / target).open("rb") as source:
            result[target] = hashlib.file_digest(source, "sha256").hexdigest()
    return result


def measure(index, mode, workers, targets, environment):
    """Rebuild only selected objects; leave compiler storage and flags unchanged."""
    selected = dict(environment)
    selected.pop("SCCACHE_ERROR_LOG", None)
    selected.pop("SCCACHE_LOG", None)
    selected["SCCACHE_CLIENT_SIDE"] = "1" if mode == "client" else "0"
    for target in targets:
        output = BUILD / target
        assert not output.is_symlink()
        output.unlink(missing_ok=True)
    prefix = RESULTS / f"{index}-{mode}-{workers}"
    run(["sccache", "--zero-stats"], prefix.with_suffix(".reset.log"), selected)
    started = time.monotonic()
    run(["cmake", "--build", str(BUILD), "--parallel", str(workers), "--target", *targets],
        prefix.with_suffix(".build.log"), selected)
    elapsed = time.monotonic() - started
    run(["sccache", "--show-stats"], prefix.with_suffix(".stats.log"), selected)
    return {"mode": mode, "workers": workers, "seconds": elapsed}, digests(targets)


def main():
    RESULTS.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    order = [("server", 2), ("server", 4), ("client", 2), ("client", 4)]
    summary = {"scope": "48 real Release objects on one runner, unchanged flags/cache; no full-job or runtime claim",
               "samples": []}
    try:
        targets = select_objects()
        summary["targets"] = targets
        _, reference = measure("warm", "server", 4, targets, environment)
        for index, (mode, workers) in enumerate(order + list(reversed(order))):
            sample, actual = measure(index, mode, workers, targets, environment)
            assert actual == reference
            summary["samples"].append(sample)
            print(json.dumps(sample), flush=True)
        summary["mediansSeconds"] = {
            f"{mode}-{workers}": statistics.median(sample["seconds"] for sample in summary["samples"]
                                                   if sample["mode"] == mode and sample["workers"] == workers)
            for mode, workers in order}
        summary["objectsIdentical"] = True
        summary["objectSha256"] = reference
    finally:
        (RESULTS / "result.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
