"""Regression tests for automatic project migration catalog generation."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def _contract(marker: str) -> str:
    return "sha256:" + marker * 64


def _create_entry(root: Path, kind: str, source: str, target: str, identity: str) -> Path:
    if kind == "sequential":
        directory = root / "definitions" / target
        manifest_name = "migration.horo.json"
    else:
        directory = root / "checkpoints" / target / source
        manifest_name = "checkpoint.horo.json"
    (directory / "stages").mkdir(parents=True)
    manifest = {
        "id": identity,
        "fromBaseline": source,
        "toBaseline": target,
        "sourceContract": _contract("1"),
        "targetContract": _contract("2"),
        "storageEstimate": {
            "maximumOutputRatioPermille": 1000,
            "maximumAddedBytesPerDocument": 16,
            "maximumFixedBytes": 1024,
        },
    }
    (directory / manifest_name).write_text(json.dumps(manifest), encoding="utf-8")
    (directory / "ProjectMigration.h").write_text(
        "// BuildProjectMigration\n", encoding="utf-8"
    )
    (directory / "ProjectMigration.cpp").write_text("// implementation\n", encoding="utf-8")
    (directory / "stages" / "FixtureStage.cpp").write_text("// stage\n", encoding="utf-8")
    return directory


def _run_generator(script: Path, root: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(script),
            "migrations",
            str(root),
            str(output / "catalog.cpp"),
            str(output / "sources.cmake"),
            str(output / "hash.txt"),
            str(output / "catalog.json"),
        ],
        check=False,
        text=True,
        capture_output=True,
    )


def test_catalog_discovery_is_deterministic_and_freezes_definition_hash(
        tmp_path: Path, migration_generator: Path
) -> None:
    root = tmp_path / "migrations"
    output = tmp_path / "generated"
    sequential = _create_entry(
        root, "sequential", "0.0.1", "0.1.0", "horo.test.sequential"
    )
    _create_entry(root, "checkpoint", "0.0.1", "0.2.0", "horo.test.checkpoint")

    first = _run_generator(migration_generator, root, output)

    assert first.returncode == 0, first.stderr
    catalog = (output / "catalog.cpp").read_text(encoding="utf-8")
    sources = (output / "sources.cmake").read_text(encoding="utf-8")
    first_hash = (output / "hash.txt").read_text(encoding="utf-8")
    generated_catalog = json.loads((output / "catalog.json").read_text(encoding="utf-8"))
    assert "R0_1_0::BuildProjectMigration" in catalog
    assert "R0_2_0::From0_0_1::BuildProjectMigration" in catalog
    assert sources.index("0.1.0/ProjectMigration.cpp") < sources.index(
        "0.2.0/0.0.1/ProjectMigration.cpp"
    )
    assert first_hash.startswith("sha256:")
    assert len(first_hash.strip()) == 71


    manifest_path = sequential / "migration.horo.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["definitionHash"] = next(
        entry["hash"] for entry in generated_catalog if entry["id"] == "horo.test.sequential"
    )
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

    second = _run_generator(migration_generator, root, output)
    assert second.returncode == 0, second.stderr
    assert (output / "hash.txt").read_text(encoding="utf-8") == first_hash

    (sequential / "stages" / "FixtureStage.cpp").write_text("// changed\n", encoding="utf-8")
    third = _run_generator(migration_generator, root, output)
    assert third.returncode != 0
    assert "definitionHash differs" in third.stderr


def test_manifest_target_must_match_directory(tmp_path: Path, migration_generator: Path) -> None:
    root = tmp_path / "migrations"
    output = tmp_path / "generated"
    directory = _create_entry(root, "sequential", "0.0.1", "0.1.0", "horo.test.invalid")
    manifest_path = directory / "migration.horo.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["toBaseline"] = "0.2.0"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

    malformed = _run_generator(migration_generator, root, output)

    assert malformed.returncode != 0
    assert "does not match its directory" in malformed.stderr


def test_definition_requires_standard_factory(tmp_path: Path, migration_generator: Path) -> None:
    root = tmp_path / "migrations"
    output = tmp_path / "generated"
    directory = _create_entry(root, "sequential", "0.0.1", "0.1.0", "horo.test.factory")
    (directory / "ProjectMigration.h").write_text(
        "// no standardized factory\n", encoding="utf-8"
    )

    missing_factory = _run_generator(migration_generator, root, output)

    assert missing_factory.returncode != 0
    assert "BuildProjectMigration factory is missing" in missing_factory.stderr
