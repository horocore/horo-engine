#!/usr/bin/env python3
"""Plan bounded coverage data and publish validated reports; never launch tools."""

import argparse
import json
import os
from pathlib import Path

import fastcov_sonar_coverage as converter

BATCH_FILES = 64


def area(root: Path, build: Path, stage: Path) -> tuple:
    """Require an existing, dedicated staging directory inside the owned build."""
    if stage.is_symlink():
        raise ValueError("Coverage staging directory cannot be a symlink")
    root = root.resolve(strict=True)
    build = build.resolve(strict=True)
    stage = stage.resolve(strict=True)
    if not build.is_relative_to(root) or build == root:
        raise ValueError("Coverage build must be inside the repository, not its root")
    if stage.parent != build or not stage.name.startswith("coverage-batches.") or not stage.is_dir():
        raise ValueError("Coverage staging directory must be a dedicated build child")
    return root, build, stage


def nul_manifest(output: Path, paths: list[Path]) -> None:
    """Preserve filesystem names without whitespace, newline or Unicode splitting."""
    output.write_bytes(b"".join(os.fsencode(path) + b"\0" for path in paths))


def plan(root: Path, build: Path, stage: Path) -> dict:
    """Assign every sorted gcno exactly once to bounded, data-only manifests."""
    _, build, stage = area(root, build, stage)
    files = sorted(build.rglob("*.gcno"))
    if not files:
        raise ValueError("No gcno inputs; refusing to publish empty coverage")
    manifests = []
    for offset in range(0, len(files), BATCH_FILES):
        manifest = stage / f"batch-{len(manifests):06d}.files"
        nul_manifest(manifest, files[offset:offset + BATCH_FILES])
        manifests.append(manifest)
    nul_manifest(stage / "batches.manifest", manifests)
    return {"inputs": len(files), "batches": len(manifests), "batchLimit": BATCH_FILES}


def publish(root: Path, build: Path, stage: Path) -> dict:
    """Validate merged data before individually atomic, nontransactional replaces."""
    root, build, stage = area(root, build, stage)
    merged = stage / "fastcov-coverage.json"
    document, metrics = converter.convert(json.loads(merged.read_text(encoding="utf-8")), root)
    xml = stage / "coverage.xml"
    xml.write_bytes(converter.element_tree.tostring(document, encoding="utf-8", xml_declaration=True))
    # Batch/merge/conversion failures touch neither authoritative output. A
    # publication I/O failure can replace one report: the pair is not a transaction.
    # The CLI fails terminally and the CI host's success-only upload gate stays closed.
    os.replace(merged, build / "fastcov-coverage.json")
    os.replace(xml, build / "coverage.xml")
    return metrics


def main() -> int:
    """Expose only planning/publication data capabilities, not executable inputs."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("plan", "publish"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build-directory", type=Path, required=True)
    parser.add_argument("--stage", type=Path, required=True)
    args = parser.parse_args()
    try:
        operation = plan if args.operation == "plan" else publish
        metrics = operation(args.root, args.build_directory, args.stage)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"Coverage data operation failed: {error}\n")
    print(json.dumps(metrics, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
