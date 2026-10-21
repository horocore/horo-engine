import importlib.util
import json
import sys
from pathlib import Path

import pytest


SCRIPT = Path(__file__).parents[2] / "scripts" / 'project.py'
SPEC = importlib.util.spec_from_file_location("horo_extension_scaffolder", SCRIPT)
assert SPEC is not None
assert SPEC.loader is not None
SCAFFOLDER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = SCAFFOLDER
SPEC.loader.exec_module(SCAFFOLDER)


def run_scaffolder(monkeypatch, output: Path, shape: str, package_id: str = "com.example.tools"):
    monkeypatch.setattr(
        sys,
        "argv",
        [str(SCRIPT), "--shape", shape, "--id", package_id,
         "--name", "Example Tools", "--output", str(output)],
    )
    return SCAFFOLDER.scaffold_main()


def canonical_id(length: int) -> str:
    assert length >= 3
    return "a." + "b" * (length - 2)


def test_each_shape_is_portable_and_complete(monkeypatch, tmp_path):
    expected_modules = {"gui": 1, "backend": 1, "script": 1, "hybrid": 3}
    for shape, module_count in expected_modules.items():
        output = tmp_path / shape
        assert run_scaffolder(monkeypatch, output, shape) == 0
        manifest = json.loads((output / "extension.json.in").read_text(encoding="utf-8"))
        assert len(manifest["modules"]) == module_count
        assert all(module["entry"].startswith("bin/") for module in manifest["modules"])
        assert all(module["entry"].endswith("@CMAKE_SHARED_MODULE_SUFFIX@") for module in manifest["modules"])
        assert len(list((output / "src").glob("*.c"))) == module_count
        assert len(list((output / "tests").glob("*.c"))) == module_count
        generated = "\n".join(
            path.read_text(encoding="utf-8") for path in output.rglob("*") if path.is_file()
        )
        assert str(SCRIPT.parents[1]) not in generated
        assert "HoroEngine::ExtensionSdk" in generated
        assert "include(CPack)" in generated
        assert "Horo/Extensions/ExtensionAbi.h" in generated


def test_hybrid_keeps_backend_authority_separate(monkeypatch, tmp_path):
    output = tmp_path / "hybrid"
    assert run_scaffolder(monkeypatch, output, "hybrid") == 0
    modules = json.loads((output / "extension.json.in").read_text(encoding="utf-8"))["modules"]
    by_id = {module["id"]: module for module in modules}
    backend = by_id["com.example.tools.backend"]
    assert backend["roles"] == ["backend-capability", "headless-tooling"]
    assert "exports" in backend
    for suffix in ("editor", "script"):
        adapter = by_id[f"com.example.tools.{suffix}"]
        assert adapter["dependencies"] == [backend["id"]]
        assert adapter["imports"][0]["service"] == backend["exports"][0]["id"]
        assert "backend-capability" not in adapter["roles"]


def test_invalid_identity_does_not_publish_partial_output(monkeypatch, tmp_path):
    output = tmp_path / "invalid"
    assert run_scaffolder(monkeypatch, output, "backend", "Invalid Id") == 2
    assert not output.exists()


def test_manifest_validation_rules_match_host_contract(monkeypatch, tmp_path):
    assert run_scaffolder(monkeypatch, tmp_path / "trailing", "backend", "com.example.bad-") == 2
    assert run_scaffolder(monkeypatch, tmp_path / "long", "backend", "a" * 257) == 2
    monkeypatch.setattr(
        sys,
        "argv",
        [str(SCRIPT), "--shape", "backend", "--id", "com.example.tools", "--name", "x" * 4097,
         "--output", str(tmp_path / "long-name")],
    )
    assert SCAFFOLDER.scaffold_main() == 2
    monkeypatch.setattr(
        sys,
        "argv",
        [str(SCRIPT), "--shape", "backend", "--id", "com.example.tools", "--name", "", "--version", "1.2.3-rc.1",
         "--output", str(tmp_path / "valid")],
    )
    assert SCAFFOLDER.scaffold_main() == 0


def test_derived_identifier_boundaries_cover_every_shape():
    for modules in SCAFFOLDER.SHAPES.values():
        suffix_bytes = max(len(identifier) - len("a.b")
                           for identifier in SCAFFOLDER.derived_identifiers("a.b", modules))
        boundary_id = canonical_id(SCAFFOLDER.MAXIMUM_IDENTIFIER_BYTES - suffix_bytes)
        assert all(SCAFFOLDER.is_canonical_identifier(identifier)
                   for identifier in SCAFFOLDER.derived_identifiers(boundary_id, modules))
        oversized_id = canonical_id(len(boundary_id) + 1)
        assert not all(SCAFFOLDER.is_canonical_identifier(identifier)
                       for identifier in SCAFFOLDER.derived_identifiers(oversized_id, modules))


def test_generated_filename_boundaries_cover_modules_contract_tests_and_archive():
    backend = SCAFFOLDER.SHAPES["backend"]
    boundary_target_id = canonical_id(229)
    target_filenames = SCAFFOLDER.generated_filenames(boundary_target_id, "1.0.0", backend)
    assert max(len(filename.encode("utf-8")) for filename in target_filenames) == 255
    oversized_target_id = canonical_id(230)
    assert max(len(filename.encode("utf-8"))
               for filename in SCAFFOLDER.generated_filenames(oversized_target_id, "1.0.0", backend)) == 256

    maximum_version = "1.2.3-" + "r" * 58
    boundary_archive_id = canonical_id(186)
    archive_filenames = SCAFFOLDER.generated_filenames(boundary_archive_id, maximum_version, SCAFFOLDER.SHAPES["gui"])
    assert len(archive_filenames[0].encode("utf-8")) == 255
    oversized_archive_id = canonical_id(187)
    oversized_archive = SCAFFOLDER.generated_filenames(oversized_archive_id, maximum_version, SCAFFOLDER.SHAPES["gui"])
    assert len(oversized_archive[0].encode("utf-8")) == 256


def test_default_version_shape_filename_boundaries_publish_only_portable_projects(monkeypatch, tmp_path):
    boundaries = {
        "gui": (230, 231),
        "backend": (229, 230),
        "script": (230, 231),
        "hybrid": (229, 230),
    }
    for shape, (accepted_bytes, rejected_bytes) in boundaries.items():
        accepted_output = tmp_path / f"{shape}-accepted"
        assert run_scaffolder(monkeypatch, accepted_output, shape, canonical_id(accepted_bytes)) == 0
        assert (accepted_output / "extension.json.in").is_file()

        rejected_output = tmp_path / f"{shape}-rejected"
        assert run_scaffolder(monkeypatch, rejected_output, shape, canonical_id(rejected_bytes)) == 2
        assert not rejected_output.exists()


def test_derived_limits_reject_before_output_creation(monkeypatch, tmp_path):
    derived_output = tmp_path / "derived-id"
    assert run_scaffolder(monkeypatch, derived_output, "hybrid", canonical_id(242)) == 2
    assert not derived_output.exists()

    target_output = tmp_path / "target-name"
    assert run_scaffolder(monkeypatch, target_output, "backend", canonical_id(230)) == 2
    assert not target_output.exists()

    archive_output = tmp_path / "archive-name"
    maximum_version = "1.2.3-" + "r" * 58
    monkeypatch.setattr(
        sys,
        "argv",
        [str(SCRIPT), "--shape", "gui", "--id", canonical_id(187), "--name", "Boundary",
         "--version", maximum_version, "--output", str(archive_output)],
    )
    assert SCAFFOLDER.scaffold_main() == 2
    assert not archive_output.exists()


def test_dangling_destination_symlink_is_rejected(monkeypatch, tmp_path):
    output = tmp_path / "dangling"
    try:
        output.symlink_to(tmp_path / "missing", target_is_directory=True)
    except OSError:
        pytest.skip("Platform policy does not permit creating a test symlink")
    assert run_scaffolder(monkeypatch, output, "backend") == 2
    assert output.is_symlink()


def test_keyboard_interrupt_removes_staging_and_is_re_raised(monkeypatch, tmp_path):
    def interrupt(*_args, **_kwargs):
        raise KeyboardInterrupt

    monkeypatch.setattr(SCAFFOLDER, "scaffold_write_project", interrupt)
    output = tmp_path / "cancelled"
    with pytest.raises(KeyboardInterrupt):
        run_scaffolder(monkeypatch, output, "backend")
    assert not output.exists()
    assert not list(tmp_path.glob(".horo-extension.*"))
