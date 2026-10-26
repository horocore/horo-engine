"""Private shader data admission tests; no native compiler or GPU substitution."""
import importlib.util
import json
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ui_paint_fixture", ROOT / ".github/scripts/ui_paint_fixture.py")
FIXTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURE)


@pytest.fixture
def handoff(tmp_path, monkeypatch):
    monkeypatch.setattr(FIXTURE, "identity", lambda source: {"sha": "a" * 40, "run_id": "42", "attempt": "1"})
    root = tmp_path / "handoff"
    root.mkdir()
    for entry, _, _ in FIXTURE.STAGES:
        (root / (entry + ".spv")).write_bytes(b"\x03\x02\x23\x07\x00\x00\x00\x00")
        (root / (entry + ".metal")).write_text("#include <metal_stdlib>\n")
        reflection = {"ubos": [{"block_size": 64, "set": 0, "binding": 0, "type": "_1"}],
                      "types": {"_1": {"members": [{"name": name, "offset": offset}
                                                    for name, offset in FIXTURE.PARAMETER_OFFSETS.items()]}}}
        (root / (entry + ".reflection.json")).write_text(json.dumps(reflection))
    manifest = FIXTURE.expected_manifest(ROOT)
    manifest["files"] = {name: FIXTURE.digest(FIXTURE.bounded(root / name)) for name in sorted(FIXTURE.inventory(root))}
    (root / "manifest.json").write_text(json.dumps(manifest))
    return root, manifest


def write_manifest(root, value):
    (root / "manifest.json").write_text(json.dumps(value))


def test_exact_same_run_qualification_data_is_accepted(handoff):
    root, expected = handoff
    assert FIXTURE.verify(root, ROOT) == expected


@pytest.mark.parametrize("run", ["42", "1" + "0" * 19])
def test_run_identity_accepts_only_bounded_ascii_decimal(monkeypatch, run):
    expected = {"GITHUB_SHA": "a" * 40, "GITHUB_RUN_ID": run,
                "GITHUB_RUN_ATTEMPT": "1", "GITHUB_REPOSITORY": "horocore/horo-engine"}
    for key, value in expected.items():
        monkeypatch.setenv(key, value)
    monkeypatch.setattr(FIXTURE.subprocess, "check_output", lambda *args, **kwargs: "a" * 40)
    assert FIXTURE.identity(ROOT) == expected


@pytest.mark.parametrize("run", ["0", "01", "1" + "0" * 20, "1\u0662", "1\uff12"])
def test_run_identity_rejects_noncanonical_decimal_before_checkout(monkeypatch, run):
    for key, value in {"GITHUB_SHA": "a" * 40, "GITHUB_RUN_ID": run,
                       "GITHUB_RUN_ATTEMPT": "1", "GITHUB_REPOSITORY": "horocore/horo-engine"}.items():
        monkeypatch.setenv(key, value)
    with pytest.raises(ValueError, match="Invalid run identity"):
        FIXTURE.identity(ROOT)


def test_catalog_rejects_unclosed_repeated_entries_without_regex_backtracking(monkeypatch):
    marker = "ApprovedShaderCompilerTool{ShaderCompilerTool::Dxc,"
    monkeypatch.setattr(FIXTURE, "bounded", lambda *args: (marker * 1024).encode())
    with pytest.raises(ValueError, match="Malformed canonical tool entry"):
        FIXTURE.catalog(ROOT)


@pytest.mark.parametrize("target", ["private/var", "/private/var"])
def test_only_fixed_macos_system_alias_is_translated(monkeypatch, target):
    monkeypatch.setattr(FIXTURE.platform, "system", lambda: "Darwin")
    monkeypatch.setattr(FIXTURE.os, "readlink", lambda path: target)
    for name in ("handoff/manifest.json", "offline/PaintVertex.metallib", "installed/usr/bin/metal"):
        assert FIXTURE.absolute_data_path(Path("/var/folders/fixture") / name) == Path("/private/var/folders/fixture") / name


@pytest.mark.parametrize("path", ["/private/var/folders/input", "/unrelated/input", "/various/input"])
def test_alias_translation_does_not_resolve_other_path_prefixes(monkeypatch, path):
    monkeypatch.setattr(FIXTURE.platform, "system", lambda: "Darwin")
    assert FIXTURE.absolute_data_path(Path(path)) == Path(path)


@pytest.mark.parametrize("target", ["/unexpected-root", "private/var/../../unexpected-root", "/private/var/other"])
def test_unexpected_macos_system_alias_is_rejected(monkeypatch, target):
    monkeypatch.setattr(FIXTURE.platform, "system", lambda: "Darwin")
    monkeypatch.setattr(FIXTURE.os, "readlink", lambda path: target)
    with pytest.raises(ValueError, match="Unexpected macOS temporary-root alias"):
        FIXTURE.absolute_data_path(Path("/var/folders/fixture/manifest.json"))


def test_missing_macos_system_alias_is_rejected(monkeypatch):
    monkeypatch.setattr(FIXTURE.platform, "system", lambda: "Darwin")

    def missing(path):
        raise FileNotFoundError(path)

    monkeypatch.setattr(FIXTURE.os, "readlink", missing)
    with pytest.raises(ValueError, match="Unexpected macOS temporary-root alias"):
        FIXTURE.absolute_data_path(Path("/var/folders/fixture/manifest.json"))


@pytest.mark.skipif(FIXTURE.platform.system() != "Darwin", reason="Actual macOS /var system alias required")
def test_bounded_reads_actual_macos_temporary_root_alias(tmp_path):
    physical = tmp_path.resolve()
    if physical.parts[:3] != ("/", "private", "var"):
        pytest.skip("Temporary root is not beneath the macOS /var alias")
    (physical / "input").write_bytes(b"same physical input")
    alias = Path("/var").joinpath(*physical.parts[3:]) / "input"
    assert FIXTURE.bounded(alias) == FIXTURE.bounded(physical / "input") == b"same physical input"
    assert FIXTURE.absolute_data_path(alias.parent / "offline") == physical / "offline"
    (physical / "escape").symlink_to(physical / "input")
    with pytest.raises(ValueError, match="confined regular file"):
        FIXTURE.bounded(alias.parent / "escape")


@pytest.mark.parametrize("kind", ["directory", "file"])
def test_bounded_rejects_caller_controlled_symlink_components(tmp_path, kind):
    physical = tmp_path.resolve()
    outside = physical / "outside"
    outside.mkdir()
    (outside / "input").write_bytes(b"must not follow")
    if kind == "directory":
        link = physical / "alias"
        link.symlink_to(outside, target_is_directory=True)
        path = link / "input"
    else:
        path = physical / "input"
        path.symlink_to(outside / "input")
    with pytest.raises(ValueError, match="confined regular file"):
        FIXTURE.bounded(path)


def test_parent_traversal_is_rejected_before_alias_translation(monkeypatch):
    monkeypatch.setattr(FIXTURE.platform, "system", lambda: "Darwin")
    with pytest.raises(ValueError, match="Parent traversal forbidden"):
        FIXTURE.absolute_data_path(Path("/var/folders/../escape"))


@pytest.mark.parametrize("field", ["identity", "source", "tools", "dxc_options", "cross_options", "reflect_options", "apple_options", "target", "entries"])
def test_stale_or_modified_provenance_rejected_before_tools(handoff, field):
    root, manifest = handoff
    manifest[field] = None
    write_manifest(root, manifest)
    with pytest.raises(ValueError, match="Stale/tampered"):
        FIXTURE.verify(root, ROOT)


@pytest.mark.parametrize("mutation", ["missing", "tampered", "unexpected", "nested", "symlink", "symlink_root", "oversized"])
def test_malformed_file_sets_fail_closed(handoff, tmp_path, mutation):
    root, _ = handoff
    target = root / "PaintVertex.metal"
    if mutation == "missing":
        target.unlink()
    elif mutation == "tampered":
        target.write_text("#include <metal_stdlib>\nchanged")
    elif mutation == "unexpected":
        (root / "../escape.txt").write_text("not an input")
        (root / "unexpected").write_text("not an input")
    elif mutation == "nested":
        (root / "nested").mkdir()
    elif mutation == "symlink":
        target.unlink()
        target.symlink_to(ROOT / FIXTURE.SOURCE)
    elif mutation == "symlink_root":
        link = tmp_path / "link"
        link.symlink_to(root, target_is_directory=True)
        root = link
    elif mutation == "oversized":
        with target.open("wb") as stream:
            stream.truncate(FIXTURE.LIMIT + 1)
    with pytest.raises(ValueError):
        FIXTURE.verify(root, ROOT)


def test_duplicate_manifest_keys_are_not_last_write_wins(handoff):
    root, _ = handoff
    (root / "manifest.json").write_text('{"schema":1,"schema":1}')
    with pytest.raises(ValueError, match="Duplicate"):
        FIXTURE.verify(root, ROOT)


def test_attested_bad_spirv_magic_is_still_rejected(handoff):
    root, manifest = handoff
    (root / "PaintVertex.spv").write_bytes(b"not SPIRV")
    manifest["files"]["PaintVertex.spv"] = FIXTURE.digest(b"not SPIRV")
    write_manifest(root, manifest)
    with pytest.raises(ValueError, match="SPIR-V magic"):
        FIXTURE.verify(root, ROOT)


def test_apple_does_not_execute_any_tool_when_handoff_is_invalid(handoff, monkeypatch):
    root, _ = handoff
    (root / "PaintVertex.metal").unlink()
    calls = []
    monkeypatch.setattr(FIXTURE.subprocess, "run", lambda *args, **kwargs: calls.append(args))
    args = type("Arguments", (), {"root": root, "source": ROOT})()
    with pytest.raises(ValueError):
        FIXTURE.apple(args)
    assert calls == []


def test_native_mapping_must_agree_between_actual_generated_stages(handoff):
    root, _ = handoff
    vertex = "constant PaintParameters& p [[buffer(0)]]"
    fragment = vertex + " texture2d<float> PaintTexture [[texture(1)]] "
    fragment += "texture2d<float> CoverageTexture [[texture(2)]] sampler PaintSampler [[sampler(3)]]"
    (root / "PaintVertex.metal").write_text(vertex)
    (root / "PaintFragment.metal").write_text(fragment)
    assert FIXTURE.native_bindings(root) == [0, 1, 2, 3]
    (root / "PaintVertex.metal").write_text(vertex.replace("buffer(0)", "buffer(4)"))
    with pytest.raises(ValueError, match="stage bindings disagree"):
        FIXTURE.native_bindings(root)


@pytest.mark.parametrize("mutation", ["size", "offset", "duplicate_member"])
def test_actual_compiler_reflection_must_match_exact_uniform_abi(handoff, mutation):
    root, _ = handoff
    path = root / "PaintFragment.reflection.json"
    reflection = json.loads(path.read_text())
    if mutation == "size":
        reflection["ubos"][0]["block_size"] = 80
    elif mutation == "offset":
        reflection["types"]["_1"]["members"][5]["offset"] = 32
    else:
        reflection["types"]["_1"]["members"].append(reflection["types"]["_1"]["members"][0])
    path.write_text(json.dumps(reflection))
    with pytest.raises(ValueError, match="raster ABI"):
        FIXTURE.validate_reflection(path)


MAIN_PUBLICATION = ("github.repository == 'horocore/horo-engine' && github.ref == 'refs/heads/main' && "
                    "(github.event_name == 'push' || github.event_name == 'workflow_dispatch')")
NATIVE_PARTITIONS = ("", " && !(" + MAIN_PUBLICATION + ")", " && " + MAIN_PUBLICATION)


def assert_native_guard(block):
    """Accept only required gates with the explicit canonical main/PR partition."""
    match = re.search(r"^    if: \$\{\{ (.*?) \}\}$", block, re.MULTILINE)
    assert match is not None
    prefix = "!cancelled() && needs.tooling.result == 'success'"
    assert match[1] in tuple(prefix + partition for partition in NATIVE_PARTITIONS)


@pytest.mark.parametrize("partition", NATIVE_PARTITIONS)
def test_native_guard_accepts_explicit_main_pr_partitions(partition):
    assert_native_guard("    if: ${{ !cancelled() && needs.tooling.result == 'success'" + partition + " }}")


@pytest.mark.parametrize("guard", ["needs.tooling.result == 'success'", "!cancelled()",
                                   "cancelled() && needs.tooling.result == 'success'",
                                   "!cancelled() || needs.tooling.result == 'success'",
                                   "!cancelled() && needs.tooling.result != 'success'",
                                   "!cancelled() && needs.tooling.result == 'success' || true"])
def test_native_guard_rejects_missing_or_weakened_required_gates(guard):
    with pytest.raises(AssertionError):
        assert_native_guard("    if: ${{ " + guard + " }}")


def test_workflow_keeps_same_run_metal_handoff_and_independent_native_checks():
    workflow = (ROOT / ".github/workflows/ci.yml").read_text()
    native = workflow.split("\n  test:\n", 1)[1]
    assert "needs: [tooling, shaders]" in native
    assert_native_guard(native)
    assert_native_guard(workflow.split("\n  test-main:\n", 1)[1])
    artifact = "ui-paint-metal-msl-${{ github.sha }}-${{ github.run_attempt }}"
    assert workflow.count("name: " + artifact) == 2
    assert workflow.index("uses: ./.github/actions/cook-vulkan-viewport") < workflow.index("ui_paint_fixture.py produce")
    assert native.index("ui_paint_fixture.py apple") < native.index("Configure Metal shader and presentation tests")
    assert 'HORO_UI_PAINT_METAL_FIXTURE=%s' in native
    assert 'HoroMetalGraphExecutionSmoke' in native
    assert "--no-tests=error" in native


def test_producer_rejects_unattested_executable_before_any_process_or_output(tmp_path, monkeypatch):
    executable = tmp_path / "unattested-tool"
    executable.write_bytes(b"not the canonical executable")
    output = tmp_path / "unpublished"
    metadata = {"tools": {role: {"binary": "sha256:" + "0" * 64}
                          for role in ("Dxc", "SpirvTools", "SpirvCross")}}
    monkeypatch.setattr(FIXTURE, "expected_manifest", lambda source: metadata)
    monkeypatch.setattr(FIXTURE.platform, "machine", lambda: "x86_64")

    class HostRelease:
        @staticmethod
        def exists():
            return True

        @staticmethod
        def read_text():
            return 'ID=ubuntu\nVERSION_ID="26.04"\n'

    path_type = FIXTURE.Path
    release = HostRelease()
    monkeypatch.setattr(FIXTURE, "Path", lambda path: release if str(path) == "/etc/os-release" else path_type(path))
    calls = []
    monkeypatch.setattr(FIXTURE.subprocess, "run", lambda *args, **kwargs: calls.append(args))
    args = type("Arguments", (), {"source": ROOT, "root": output, "tools_root": None,
                                  "dxc": executable, "validator": executable, "cross": executable})()
    with pytest.raises(ValueError, match="Tool executable differs from canonical lock"):
        FIXTURE.produce(args)
    assert calls == []
    assert not output.exists()
