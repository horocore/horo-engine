"""Offline SDK artifact, version-authority and packaging contracts."""

import json
from pathlib import Path
import re

import pytest


ROOT = Path(__file__).resolve().parents[2]
ARTIFACTS = ("CompatibilityMatrix.json", "MigrationGuide.md")


def configured_values():
    """Read the production version authority without running native configure."""
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    names = (
        "HORO_ENGINE_VERSION",
        "HORO_EXTENSION_ABI_MAJOR_VERSION",
        "HORO_EXTENSION_ABI_PUBLISHED_MINOR",
        "HORO_EXTENSION_ABI_MIN_HOST_MINOR",
        "HORO_EXTENSION_ABI_MAX_HOST_MAJOR_EXCLUSIVE",
    )
    values = {}
    for name in names:
        match = re.search(rf'set\({name} "?([^"\s)]+)"?\)', cmake)
        assert match, name
        values[name] = match.group(1)
    values["HORO_EXTENSION_SDK_VERSION"] = values["HORO_ENGINE_VERSION"]
    values["HORO_SOURCE_REVISION"] = "7d853ba6504a"
    values["CMAKE_EXECUTABLE_SUFFIX"] = ""
    return values


def render(source, values):
    """Mirror configure_file @ONLY substitution, leaving unknown tokens visible."""
    return re.sub(r"@([A-Z_]+)@", lambda match: values.get(match[1], match[0]), source)


@pytest.fixture
def offline_sdk(tmp_path):
    """Copy only rendered public artifacts into an unrelated author directory."""
    root = tmp_path / "offline SDK ünicode with spaces"
    share = root / "share/horo/extension-sdk"
    share.mkdir(parents=True)
    values = configured_values()
    for artifact in (*ARTIFACTS, "extension-sdk.json"):
        source = (ROOT / "sdk" / f"{artifact}.in").read_text(encoding="utf-8")
        (share / artifact).write_text(render(source, values), encoding="utf-8")
    readme = (ROOT / "sdk/ExtensionSdkREADME.md.in").read_text(encoding="utf-8")
    (root / "README.md").write_text(render(readme, values), encoding="utf-8")
    return root


def test_packaged_offline_artifacts_resolve_without_source_checkout(offline_sdk):
    metadata = json.loads(
        (offline_sdk / "share/horo/extension-sdk/extension-sdk.json").read_text(encoding="utf-8")
    )
    for key in ("compatibilityMatrix", "migrationGuide"):
        path = offline_sdk / metadata[key]
        assert path.is_file()
        assert path.resolve().is_relative_to(offline_sdk.resolve())
    matrix_path = offline_sdk / metadata["compatibilityMatrix"]
    matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
    assert (matrix_path.parent / matrix["migrationGuide"]).is_file()
    for path in offline_sdk.rglob("*"):
        if path.is_file():
            content = path.read_text(encoding="utf-8")
            assert str(ROOT) not in content
            assert re.search(r"@[A-Z_]+@", content) is None
            for target in re.findall(r"\]\(([^)]+)\)", content):
                assert (path.parent / target).is_file(), target


def test_matrix_matches_sdk_version_abi_schema_and_platform_authorities(offline_sdk):
    share = offline_sdk / "share/horo/extension-sdk"
    matrix = json.loads((share / ARTIFACTS[0]).read_text(encoding="utf-8"))
    metadata = json.loads((share / "extension-sdk.json").read_text(encoding="utf-8"))
    sdk = matrix["extensionSdk"]
    assert matrix["schemaVersion"] == 1
    assert matrix["artifactEdition"] == "extension-compatibility-1"
    assert matrix["engine"]["version"] == metadata["sdk"]["version"] == sdk["version"]
    assert matrix["engine"]["sourceRevision"] == configured_values()["HORO_SOURCE_REVISION"]
    assert matrix["engine"]["releaseStatus"] == "development"
    for key in ("id", "minimum", "maximumExclusive", "published"):
        assert sdk["hostAbi"][key] == metadata["hostAbi"][key]
    header = (ROOT / "include/Horo/Extensions/ExtensionAbi.h").read_text(encoding="utf-8")
    assert f'HORO_EXTENSION_ABI_MINOR_VERSION = {sdk["hostAbi"]["published"]["minor"]}' in header
    schema = json.loads((ROOT / "sdk/schemas/extension-manifest-v1.schema.json").read_text(encoding="utf-8"))
    assert sdk["extensionManifestSchema"] == schema["properties"]["schemaVersion"]["const"]
    workflow = (ROOT / "sdk/ci/extension-author-ci.yml").read_text(encoding="utf-8")
    assert len(sdk["supportedAuthorCiPlatforms"]) == 4
    for platform in sdk["supportedAuthorCiPlatforms"]:
        assert platform in workflow


def test_gameplay_generation_is_distinct_and_matches_real_native_boundary(offline_sdk):
    matrix_path = offline_sdk / "share/horo/extension-sdk/CompatibilityMatrix.json"
    gameplay = json.loads(matrix_path.read_text(encoding="utf-8"))["gameplay"]
    assert gameplay["separateContract"] is True
    generation = gameplay["boundaryVersion"]
    assert gameplay["fingerprintGeneration"] == f"gameplay-sdk{generation}"
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    header = (ROOT / "include/Horo/Gameplay/GameModule.h").read_text(encoding="utf-8")
    assert f"-gameplay-sdk{generation}-" in cmake
    assert f"GameplaySdkBoundaryVersion = {generation}" in header
    assert "not included" in gameplay["distribution"]


@pytest.mark.parametrize("section", (
    "External C ABI 1.0 → 1.1",
    "External C ABI 1.1 → 1.2 → 1.3",
    "Gameplay SDK8 → SDK9 (native boundary 8 → 9)",
    "Retirement source API migration in this snapshot",
))
def test_each_version_specific_migration_has_action_and_rollback(section, offline_sdk):
    guide = (offline_sdk / "share/horo/extension-sdk/MigrationGuide.md").read_text(encoding="utf-8")
    content = guide.split(f"## {section}\n", 1)[1].split("\n## ", 1)[0]
    assert "rebuild" in content.lower()
    assert "rollback" in content.lower()
    assert "`" in content  # Concrete replacement/negotiation API, not latest-only advice.


def test_packaging_and_reconfigure_inventory_include_both_artifacts():
    packaging = (ROOT / "cmake/HoroExtensionSdk.cmake").read_text(encoding="utf-8")
    inventory = (ROOT / "tests/cmake/RunExtensionSdkReconfigureTest.cmake").read_text(encoding="utf-8")
    assert "foreach(artifact IN ITEMS CompatibilityMatrix.json MigrationGuide.md)" in packaging
    assert '"${PROJECT_SOURCE_DIR}/sdk/${artifact}.in"' in packaging
    assert '"${package_root}/share/horo/extension-sdk/${artifact}"' in packaging
    assert 'install(DIRECTORY "${package_root}/"' in packaging
    for artifact in ARTIFACTS:
        assert f"sdk/{artifact}.in" in inventory
        assert f"share/horo/extension-sdk/{artifact}" in inventory


def test_unknown_provenance_is_explicit_not_a_release_attestation():
    values = configured_values()
    values["HORO_SOURCE_REVISION"] = "unknown"
    source = (ROOT / "sdk/CompatibilityMatrix.json.in").read_text(encoding="utf-8")
    matrix = json.loads(render(source, values))
    assert matrix["engine"]["sourceRevision"] == "unknown"
    guide = render((ROOT / "sdk/MigrationGuide.md.in").read_text(encoding="utf-8"), values)
    assert "provenance is unavailable, not verified compatibility" in guide
    assert "not leak-free unload" in guide
    assert "per manager, not globally bounded" in guide
    assert "not an invented transactional package-upgrade API" in guide
