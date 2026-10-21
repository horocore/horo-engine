from __future__ import annotations

import sys

import importlib.util
from pathlib import Path

import pytest


GENERATOR_PATH = Path(__file__).resolve().parents[2] / "scripts" / 'project.py'
GENERATOR_SPEC = importlib.util.spec_from_file_location("horo_gameplay_descriptor_generator", GENERATOR_PATH)
if GENERATOR_SPEC is None or GENERATOR_SPEC.loader is None:
    raise RuntimeError(f"could not load gameplay descriptor generator from {GENERATOR_PATH}")
generator = importlib.util.module_from_spec(GENERATOR_SPEC)
sys.modules[GENERATOR_SPEC.name] = generator
GENERATOR_SPEC.loader.exec_module(generator)


def generate(
    output: Path,
    revision: Path,
    sources: list[Path],
    *,
    module_id: str = "game.tests",
    fingerprint: str = "test-fingerprint",
) -> None:
    generator.generate_bundle(output, revision, output.parent, module_id, fingerprint, sources)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_generation_is_deterministic_across_source_order(tmp_path: Path) -> None:
    first = tmp_path / "First.cpp"
    second = tmp_path / "Second.cpp"
    first.write_text('HORO_BEHAVIOR(FirstBehavior, "game.tests.first")\n', encoding="utf-8")
    second.write_text('HORO_BEHAVIOR(SecondBehavior, "game.tests.second")\n', encoding="utf-8")

    first_output = tmp_path / "bundle-first.cpp"
    first_revision = tmp_path / "first.revision"
    second_output = tmp_path / "bundle-second.cpp"
    second_revision = tmp_path / "second.revision"
    generate(first_output, first_revision, [second, first])
    generate(second_output, second_revision, [first, second])

    require(first_output.read_bytes() == second_output.read_bytes(), "generated source depends on input order")
    require(first_revision.read_bytes() == second_revision.read_bytes(), "descriptor revision depends on input order")
    require(int(first_revision.read_text(encoding="utf-8")) > 0, "descriptor revision must be nonzero")


def test_generation_ignores_annotation_text_in_comments_and_strings(tmp_path: Path) -> None:
    source = tmp_path / "Behavior.cpp"
    source.write_text(
        '''
        // HORO_BEHAVIOR(CommentBehavior, "game.tests.comment")
        const char *example = "HORO_BEHAVIOR(StringBehavior, game.tests.string)";
        HORO_BEHAVIOR(RealBehavior, "game.tests.real")
        ''',
        encoding="utf-8",
    )
    output = tmp_path / "bundle.cpp"
    generate(output, tmp_path / "bundle.revision", [source])

    generated = output.read_text(encoding="utf-8")
    require("HoroGeneratedBehaviorDescriptor_RealBehavior" in generated, "real annotation was not emitted")
    require("CommentBehavior" not in generated, "comment annotation was emitted")
    require("StringBehavior" not in generated, "string annotation was emitted")


def test_generation_rejects_duplicate_stable_type_ids(tmp_path: Path) -> None:
    source = tmp_path / "Duplicate.cpp"
    source.write_text(
        '''
        HORO_BEHAVIOR(FirstBehavior, "game.tests.duplicate")
        HORO_BEHAVIOR(SecondBehavior, "game.tests.duplicate")
        ''',
        encoding="utf-8",
    )
    output = tmp_path / "bundle.cpp"

    with pytest.raises(ValueError, match="duplicate native behavior type ID"):
        generate(output, tmp_path / "bundle.revision", [source])
    require(not output.exists(), "failed generation published an output")


@pytest.mark.parametrize(
    ("module_id", "fingerprint"),
    [
        ("game." + "a" * 252, "test-fingerprint"),
        ("game.tests", "unsafe fingerprint"),
        ("invalid", "test-fingerprint"),
    ],
)
def test_generation_rejects_invalid_or_oversized_identity(
    tmp_path: Path,
    module_id: str,
    fingerprint: str,
) -> None:
    source = tmp_path / "Behavior.cpp"
    source.write_text('HORO_BEHAVIOR(Behavior, "game.tests.valid")\n', encoding="utf-8")
    with pytest.raises(ValueError, match="module identity or build fingerprint is invalid"):
        generate(tmp_path / "bundle.cpp", tmp_path / "bundle.revision", [source], module_id=module_id, fingerprint=fingerprint)


def test_generation_rejects_more_than_the_supported_behavior_count(tmp_path: Path) -> None:
    source = tmp_path / "Many.cpp"
    source.write_text(
        "\n".join(f'HORO_BEHAVIOR(Behavior{index}, "game.tests.behavior_{index}")' for index in range(4097)),
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="annotation count exceeds"):
        generate(tmp_path / "bundle.cpp", tmp_path / "bundle.revision", [source])


def test_generation_confines_outputs_to_the_declared_root(tmp_path: Path) -> None:
    source = tmp_path / "Behavior.cpp"
    source.write_text('HORO_BEHAVIOR(Behavior, "game.tests.valid")\n', encoding="utf-8")
    with pytest.raises(ValueError, match="must remain inside"):
        generator.generate_bundle(
            tmp_path.parent / "escaped.cpp",
            tmp_path / "bundle.revision",
            tmp_path,
            "game.tests",
            "test-fingerprint",
            [source],
        )


def test_generation_rejects_duplicate_source_paths_and_malformed_annotations(tmp_path: Path) -> None:
    source = tmp_path / "Behavior.cpp"
    source.write_text('HORO_BEHAVIOR(Behavior, "game.tests.valid")\n', encoding="utf-8")
    with pytest.raises(ValueError, match="duplicate paths"):
        generate(tmp_path / "bundle.cpp", tmp_path / "bundle.revision", [source, source])

    source.write_text("HORO_BEHAVIOR(Behavior)\n", encoding="utf-8")
    with pytest.raises(ValueError, match="malformed HORO_BEHAVIOR annotation"):
        generate(tmp_path / "bundle.cpp", tmp_path / "bundle.revision", [source])
