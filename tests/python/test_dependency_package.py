"""SDK integrity, fallback safety and native library relocation contracts."""

import copy
import importlib.util
import io
import json
from pathlib import Path
import shutil
import tarfile

import pytest

from test_ci_test_isolation import run


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "dependency_package", ROOT / ".github/scripts/dependency_package.py")
sdk = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sdk)


@pytest.fixture
def identity():
    return {
        "platform": "fixture", "architecture": "native", "compiler": "fixture-1",
        "sdk": "fixture-1", "abi": "fixture", "configuration": "Debug",
        "dependencies": {"probe": "pinned"}, "recipe": "v1",
        "instrumentation": "none", "features": [],
    }


@pytest.fixture
def package(tmp_path, identity):
    root = tmp_path / "staged"
    (root / "include").mkdir(parents=True)
    (root / "include/probe.h").write_text("int probe(void);\n", encoding="utf-8")
    (root / "NOTICE").write_text("Fixture notice\n", encoding="utf-8")
    archive = tmp_path / "sdk.tar.gz"
    sdk.pack(root, identity, archive)
    return archive


def rewrite_archive(source, destination, mutate):
    with tarfile.open(source, "r:gz") as archive:
        members = [(member, archive.extractfile(member).read()) for member in archive]
    mutate(members)
    with tarfile.open(destination, "w:gz") as archive:
        for member, data in members:
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))


def test_round_trip_relocated_directory(package, identity, tmp_path):
    destination = tmp_path / "different directory ü" / "sdk"
    sdk.restore(package, identity, destination)
    assert (destination / "include/probe.h").read_text() == "int probe(void);\n"
    assert (destination / "NOTICE").read_text() == "Fixture notice\n"


@pytest.mark.parametrize("field", sorted(sdk.IDENTITY_FIELDS))
def test_every_compatibility_field_invalidates_package(package, identity, tmp_path, field):
    changed = copy.deepcopy(identity)
    changed[field] = "different"
    destination = tmp_path / "rejected"
    with pytest.raises(ValueError, match="identity mismatch"):
        sdk.restore(package, changed, destination)
    assert not destination.exists()


@pytest.mark.parametrize("mutation", ["corrupt", "missing", "extra", "duplicate", "symlink"])
def test_bad_archive_never_exposes_partial_package(package, identity, tmp_path, mutation):
    def mutate(members):
        if mutation == "corrupt":
            member, data = members[-1]
            members[-1] = (member, b"X" * len(data))
        elif mutation == "missing":
            members.pop()
        elif mutation == "extra":
            members.append((tarfile.TarInfo("extra"), b"unexpected"))
        elif mutation == "duplicate":
            members.append(members[-1])
        else:
            members[-1][0].type = tarfile.SYMTYPE
            members[-1][0].linkname = "../../outside"

    broken = tmp_path / "broken.tar.gz"
    rewrite_archive(package, broken, mutate)
    destination = tmp_path / "consumer/sdk"
    with pytest.raises(ValueError):
        sdk.restore(broken, identity, destination)
    assert not destination.exists()
    assert list(destination.parent.iterdir()) == []


@pytest.mark.parametrize("name", [".", "../escape", "/absolute", "C:/escape", "x\\escape",
                                  "x/../escape", "x//file", "x/./file", "NUL.txt",
                                  "nested/COM1", "trailing.", "trailing ", "bad\x00name"])
def test_paths_are_portable_and_cannot_escape(name):
    with pytest.raises(ValueError):
        sdk.portable_path(name)


def test_live_destination_is_preserved(package, identity, tmp_path):
    destination = tmp_path / "live"
    destination.mkdir()
    sentinel = destination / "sentinel"
    sentinel.write_text("preserve", encoding="utf-8")
    with pytest.raises(ValueError, match="never overwrite"):
        sdk.restore(package, identity, destination)
    assert sentinel.read_text() == "preserve"


def test_duplicate_manifest_properties_are_rejected(package, identity, tmp_path):
    def mutate(members):
        member, data = members[0]
        members[0] = (member, data.replace(b'"schema": 1', b'"schema": 0, "schema": 1'))
    broken = tmp_path / "duplicate-properties.tar.gz"
    rewrite_archive(package, broken, mutate)
    with pytest.raises(ValueError, match="Duplicate"):
        sdk.restore(broken, identity, tmp_path / "rejected")


@pytest.mark.parametrize("field,value", [("size", True), ("size", False), ("size", 1.0),
                                         ("size", -1), ("executable", 1)])
def test_manifest_metadata_rejects_boolean_sizes_and_numeric_modes(package, identity, field, value):
    with tarfile.open(package, "r:gz") as archive:
        manifest = json.load(archive.extractfile(sdk.MANIFEST))
    next(iter(manifest["files"].values()))[field] = value
    with pytest.raises(ValueError, match="metadata"):
        sdk.validate_manifest(manifest, identity)


def test_restored_receipt_records_verified_identity_and_payload(package, identity, tmp_path):
    destination = tmp_path / "receipt"
    sdk.restore(package, identity, destination)
    receipt = json.loads((destination / sdk.MANIFEST).read_text(encoding="utf-8"))
    assert receipt["identity"] == identity
    for name, record in receipt["files"].items():
        assert sdk.digest_file(destination / name) == record["sha256"]
    sdk.verify(destination, identity)


@pytest.mark.parametrize("mutation", ["changed", "missing", "extra"])
def test_reconfigure_revalidates_materialized_payload(package, identity, tmp_path, mutation):
    destination = tmp_path / "reconfigure"
    sdk.restore(package, identity, destination)
    header = destination / "include/probe.h"
    if mutation == "changed":
        header.write_text("changed", encoding="utf-8")
    elif mutation == "missing":
        header.unlink()
    else:
        (destination / "unexpected").write_text("extra", encoding="utf-8")
    with pytest.raises(ValueError):
        sdk.verify(destination, identity)


def test_cmake_emits_compatible_identity_and_rejects_instrumentation(tmp_path):
    if not shutil.which("cmake"):
        pytest.skip("CMake unavailable")
    source = tmp_path / "source"
    source.mkdir()
    module = (ROOT / "cmake/HoroDependencySdk.cmake").as_posix()
    (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.25)
project(SdkIdentity LANGUAGES CXX)
include("{module}")
horo_prepare_dependency_sdk(Probe "${{CMAKE_CURRENT_LIST_FILE}}" root identity archive)
file(WRITE "${{CMAKE_BINARY_DIR}}/identity-path.txt" "${{identity}}")
''', encoding="utf-8")
    def configure(name, *flags):
        build = tmp_path / name
        run("cmake", "-S", str(source), "-B", str(build),
            "-DHORO_BUILD_DEPENDENCY_SDK_PACKAGES=ON", *flags)
        path = (build / "identity-path.txt").read_text(encoding="utf-8")
        return json.loads(Path(path).read_text(encoding="utf-8")) if path else None
    first = configure("first", "-DCMAKE_BUILD_TYPE=Debug")
    if first is None:
        pytest.skip("SDK packages require a single-config generator and Python 3.11")
    assert sdk.canonical_identity(first) == sdk.canonical_identity(configure("second", "-DCMAKE_BUILD_TYPE=Debug"))
    assert sdk.canonical_identity(first) != sdk.canonical_identity(configure("release", "-DCMAKE_BUILD_TYPE=Release"))
    assert configure("coverage", "-DHORO_CI_SONAR_COVERAGE=ON") is None


def test_expanded_limit_is_checked_before_extraction(package, identity, tmp_path, monkeypatch):
    monkeypatch.setattr(sdk, "MAX_EXPANDED_BYTES", 1)
    with pytest.raises(ValueError, match="size limit"):
        sdk.restore(package, identity, tmp_path / "oversized")
    assert not (tmp_path / "oversized").exists()


def test_pack_rejects_links_and_output_inside_staging(tmp_path, identity):
    root = tmp_path / "root"
    root.mkdir()
    with pytest.raises(ValueError, match="outside"):
        sdk.pack(root, identity, root / "output.tar.gz")
    try:
        (root / "link").symlink_to(tmp_path / "missing")
    except OSError:
        pytest.skip("Host cannot create symlinks")
    with pytest.raises(ValueError, match="symlinks"):
        sdk.pack(root, identity, tmp_path / "output.tar.gz")


def test_native_archive_links_after_producer_is_removed(tmp_path, identity):
    if not shutil.which("cmake"):
        pytest.skip("CMake unavailable")
    producer = tmp_path / "producer"
    producer.mkdir()
    (producer / "probe.c").write_text("int probe(void) { return 42; }\n", encoding="utf-8")
    (producer / "probe.h").write_text("int probe(void);\n", encoding="utf-8")
    (producer / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(SdkProducer C)
add_library(probe STATIC probe.c)
install(TARGETS probe ARCHIVE DESTINATION lib)
install(FILES probe.h DESTINATION include)
''', encoding="utf-8")
    staged = tmp_path / "staged"

    run("cmake", "-S", str(producer), "-B", str(producer / "build"))
    run("cmake", "--build", str(producer / "build"), "--config", "Debug", "--parallel", "1")
    run("cmake", "--install", str(producer / "build"), "--config", "Debug", "--prefix", str(staged))
    archive = tmp_path / "sdk.tar.gz"
    sdk.pack(staged, identity, archive)
    shutil.rmtree(producer)
    shutil.rmtree(staged)
    restored = tmp_path / "relocated sdk ü"
    sdk.restore(archive, identity, restored)
    consumer = tmp_path / "consumer"
    consumer.mkdir()
    (consumer / "main.c").write_text('#include <probe.h>\nint main(void) { return probe() != 42; }\n', encoding="utf-8")
    (consumer / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(SdkConsumer C)
enable_testing()
find_library(PROBE_LIBRARY NAMES probe PATHS "${SDK_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
add_executable(consumer main.c)
target_include_directories(consumer PRIVATE "${SDK_ROOT}/include")
target_link_libraries(consumer PRIVATE "${PROBE_LIBRARY}")
add_test(NAME sdk-consumer COMMAND consumer)
''', encoding="utf-8")
    run("cmake", "-S", str(consumer), "-B", str(consumer / "build"), f"-DSDK_ROOT={restored}")
    run("cmake", "--build", str(consumer / "build"), "--config", "Debug", "--parallel", "1")
    run("ctest", "--test-dir", str(consumer / "build"), "-C", "Debug", "--output-on-failure")


def _build_unicode_fixture(tmp_path):
    """Build a tiny archive with the layout expected by the Unicode recipe."""
    stage = tmp_path / "verified sdk ü"
    (stage / "include/unicode").mkdir(parents=True)
    (stage / "share").mkdir()
    (stage / "include/unicode/utypes.h").write_text("int unicode_probe(void);\n", encoding="utf-8")
    (stage / "share/ICU.txt").write_text("Fixture notice\n", encoding="utf-8")
    (stage / "share/icudt78l.dat").write_bytes(b"fixture")
    producer = tmp_path / "producer"
    producer.mkdir()
    (producer / "probe.c").write_text("int unicode_probe(void) { return 42; }\n", encoding="utf-8")
    (producer / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(UnicodeFixture C)
add_library(fixture STATIC probe.c)
set_target_properties(fixture PROPERTIES PREFIX "" OUTPUT_NAME unicode
    ARCHIVE_OUTPUT_DIRECTORY "${SDK_ROOT}/lib"
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${SDK_ROOT}/lib")
''', encoding="utf-8")
    run("cmake", "-S", str(producer), "-B", str(producer / "build"), f"-DSDK_ROOT={stage}",
        "-DCMAKE_BUILD_TYPE=Debug")
    run("cmake", "--build", str(producer / "build"), "--config", "Debug", "--parallel", "1")

    return stage


def test_cached_unicode_target_exports_to_external_gameplay_consumer(tmp_path):
    if not shutil.which("cmake"):
        pytest.skip("CMake unavailable")
    stage = _build_unicode_fixture(tmp_path)

    host = tmp_path / "host"
    host.mkdir()
    # Only the independently tested archive verifier is replaced. The real
    # Unicode recipe and the engine's export operation execute unchanged.
    (host / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(UnicodeExportHost C)
include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH "@MODULE_DIR@")
include(HoroUnicodeDependency)
function(horo_prepare_dependency_sdk name recipe output_root output_identity output_archive)
    set(${output_root} "${SDK_ROOT}" PARENT_SCOPE)
endfunction()
horo_add_unicode_dependency()
set_target_properties(HoroThirdPartyUnicode PROPERTIES EXPORT_NAME ThirdPartyUnicode)
export(TARGETS HoroThirdPartyUnicode NAMESPACE HoroEngine::
    FILE "${CMAKE_BINARY_DIR}/HoroEngineGameplayRuntimeUiTargets.cmake")
'''.replace("@MODULE_DIR@", (ROOT / "cmake").as_posix()), encoding="utf-8")
    host_build = host / "build"
    run("cmake", "-S", str(host), "-B", str(host_build), f"-DSDK_ROOT={stage}")

    consumer = tmp_path / "external gameplay"
    consumer.mkdir()
    (consumer / "main.c").write_text(
        '#include <unicode/utypes.h>\nint main(void) { return unicode_probe() != 42; }\n', encoding="utf-8")
    (consumer / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(ExternalGameplayConsumer C)
enable_testing()
find_package(Threads REQUIRED)
include("${SDK_EXPORT}")
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE HoroEngine::ThirdPartyUnicode)
add_test(NAME external-gameplay-unicode COMMAND consumer)
''', encoding="utf-8")
    run("cmake", "-S", str(consumer), "-B", str(consumer / "build"),
        f"-DSDK_EXPORT={host_build / 'HoroEngineGameplayRuntimeUiTargets.cmake'}")
    run("cmake", "--build", str(consumer / "build"), "--config", "Debug", "--parallel", "1")
    run("ctest", "--test-dir", str(consumer / "build"), "-C", "Debug", "--output-on-failure")
