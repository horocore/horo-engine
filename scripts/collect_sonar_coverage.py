#!/usr/bin/env python3
"""Collect all gcno inputs in bounded batches without changing coverage policy."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

import fastcov_sonar_coverage as converter

# Bound native gcov input as well as worker concurrency: --jobs alone makes
# fastcov 1.17 split the entire input corpus among its workers (970 files each
# in the observed failed run). Sequential batches never overlap their workers.
BATCH_FILES = 64
WORKERS = 2


def collect(root: Path, build: Path, fastcov: str, includes: list[str]) -> dict:
    """
    Collect each sorted gcno once; publish complete reports only after success.

    fastcov 1.17's --gcda-files also accepts gcno and processes its matching gcda;
    --process-gcno retains untested inputs. Its supported --add-tracefile merges
    line/function/branch counters, including shared template instantiations.
    Existing filters and default exclusion-marker handling are unchanged.
    Each output is atomically replaced on the same filesystem after all batches,
    the upstream merge and validated Sonar conversion have succeeded.
    The pair is not transactional: publication I/O failure can replace one file,
    but is terminal and CI must not upload outputs from a failed collection step.
    """
    root = root.resolve(strict=True)
    build = build.resolve(strict=True)
    if not build.is_relative_to(root) or build == root:
        raise ValueError("Coverage build must be inside the repository, not its root")
    files = sorted(build.rglob("*.gcno"))
    if not files:
        raise ValueError("No gcno inputs; refusing to publish empty coverage")
    print(f"Coverage inputs: {len(files)}; batch limit: {BATCH_FILES}; worker limit: {WORKERS}", flush=True)
    with tempfile.TemporaryDirectory(prefix="coverage-batches-", dir=build) as temporary:
        stage = Path(temporary)
        reports = []
        for offset in range(0, len(files), BATCH_FILES):
            batch = files[offset:offset + BATCH_FILES]
            report = stage / f"batch-{len(reports):06d}.json"
            command = [fastcov, "--gcov", "gcov", "--jobs", str(WORKERS),
                       "--process-gcno", "--search-directory", str(build),
                       "--gcda-files", *map(str, batch), "--include", *includes,
                       "--output", str(report)]
            print(f"Coverage batch {len(reports) + 1}: inputs {offset + 1}-{offset + len(batch)}", flush=True)
            # Explicit argv invokes the CI-selected tool without a shell.
            subprocess.run(command, cwd=root, check=True)
            if not report.is_file():
                raise ValueError("Fastcov batch did not produce a report")
            reports.append(report)
        merged = stage / "fastcov-coverage.json"
        subprocess.run([fastcov, "--jobs", str(WORKERS), "--add-tracefile", *map(str, reports),
                        "--output", str(merged)], cwd=root, check=True)
        document, metrics = converter.convert(json.loads(merged.read_text(encoding="utf-8")), root)
        xml = stage / "coverage.xml"
        xml.write_bytes(converter.element_tree.tostring(document, encoding="utf-8", xml_declaration=True))
        # No authoritative output is touched by batch, merge or conversion failure.
        os.replace(merged, build / "fastcov-coverage.json")
        os.replace(xml, build / "coverage.xml")
    return {"inputs": len(files), "batches": len(reports), **metrics}


def main() -> int:
    """Keep collector failure terminal so CI cannot upload partial coverage."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build-directory", type=Path, required=True)
    parser.add_argument("--fastcov", required=True)
    parser.add_argument("--include", nargs="+", required=True)
    args = parser.parse_args()
    try:
        metrics = collect(args.root, args.build_directory, args.fastcov, args.include)
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"Coverage collection failed: {error}\n")
    print(json.dumps(metrics, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
