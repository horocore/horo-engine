"""Regression tests for the committed release compatibility catalog generator."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path


def _write_release(root: Path, version: str, baseline: str, contract_value: str) -> None:
    directory = root / version
    directory.mkdir(parents=True)
    recovery_contract = {"contract": "test.migration-journal", "fields": ["operationId"]}
    recovery_hash = "sha256:" + hashlib.sha256(
        json.dumps(recovery_contract, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    (directory / "release.json").write_text(
        json.dumps(
            {
                "horoVersion": version,
                "contractBaseline": baseline,
                "projectContract": "project-contract.json",
                "migrationRecoveryContract": recovery_hash,
                "supportedMigrationRecoveryContracts": [
                    {"contract": recovery_hash, "reader": "journal-v1"}
                ],
            }
        ),
        encoding="utf-8",
    )
    (directory / "migration-recovery-contract.json").write_text(
        json.dumps(recovery_contract), encoding="utf-8"
    )
    (directory / "project-contract.json").write_text(
        json.dumps(
            {
                "horoVersion": version,
                "documents": {"fixture": contract_value},
            }
        ),
        encoding="utf-8",
    )
def _run_generator(
        script: Path,
        releases: Path,
        version: str,
        output: Path,
        definition_hash: str | None = None,
        migration_catalog: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    command = [sys.executable, str(script), "compatibility", str(releases), version, str(output)]
    if definition_hash is not None:
        command.append(definition_hash)
    if migration_catalog is not None:
        command.append(str(migration_catalog))
    return subprocess.run(command, check=False, capture_output=True, text=True)


def test_matching_patch_contract_generates_release_line_catalog(
        tmp_path: Path, compatibility_generator: Path
) -> None:
    releases = tmp_path / "releases"
    output = tmp_path / "Generated.h"
    _write_release(releases, "0.0.1", "0.0.1", "same")
    _write_release(releases, "0.0.2", "0.0.1", "same")

    generated_result = _run_generator(compatibility_generator, releases, "0.0.2", output)

    assert generated_result.returncode == 0, generated_result.stderr
    generated = output.read_text(encoding="utf-8")
    assert '"0.0.1", "0.0.1"' in generated
    assert '"0.0.2", "0.0.1"' in generated


def test_patch_contract_drift_is_rejected(tmp_path: Path, compatibility_generator: Path) -> None:
    releases = tmp_path / "releases"
    output = tmp_path / "Generated.h"
    _write_release(releases, "0.0.1", "0.0.1", "same")
    _write_release(releases, "0.0.2", "0.0.1", "drift")

    drift = _run_generator(compatibility_generator, releases, "0.0.2", output)

    assert drift.returncode != 0
    assert (
            "does not match baseline contract" in drift.stderr
            or "frozen release-line contract" in drift.stderr
    )


def test_contract_change_requires_incoming_migration_definition(
        tmp_path: Path, compatibility_generator: Path
) -> None:
    releases = tmp_path / "releases"
    output = tmp_path / "Generated.h"
    catalog = tmp_path / "catalog.json"
    definition_hash = "sha256:" + "1" * 64
    _write_release(releases, "0.0.1", "0.0.1", "old")
    _write_release(releases, "0.1.0", "0.1.0", "new")
    catalog.write_text("[]", encoding="utf-8")

    missing = _run_generator(
        compatibility_generator, releases, "0.1.0", output, definition_hash, catalog
    )

    assert missing.returncode != 0
    assert "without an incoming migration definition" in missing.stderr

    catalog.write_text(
        json.dumps(
            [
                {
                    "kind": "sequential",
                    "id": "horo.test.contract_upgrade",
                    "source": "0.0.1",
                    "target": "0.1.0",
                    "hash": "sha256:" + "2" * 64,
                }
            ]
        ),
        encoding="utf-8",
    )
    covered = _run_generator(
        compatibility_generator, releases, "0.1.0", output, definition_hash, catalog
    )
    assert covered.returncode == 0, covered.stderr
