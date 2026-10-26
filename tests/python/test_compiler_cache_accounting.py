"""Confine local ccache maintenance and verify imported-cache budget accounting."""
import os
from pathlib import Path
import shutil
import sys
from types import SimpleNamespace
import uuid

import pytest

from test_ci_test_isolation import run
from test_compiler_registry import KEY, NAMESPACE, compiler


ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def local_cache(tmp_path, monkeypatch):
    workspace = (tmp_path / "workspace").resolve()
    workspace.mkdir()
    monkeypatch.chdir(workspace)
    directory = workspace / ".ci-cache/compiler"
    executable = tmp_path / "qualified ccache"
    executable.touch()
    calls = []
    monkeypatch.setenv("HORO_COMPILER_LAUNCHER", "ccache")
    monkeypatch.setenv("CCACHE_DIR", str(directory))
    monkeypatch.setenv("CCACHE_MAXSIZE", "2G")
    monkeypatch.setattr(compiler, "shutil", SimpleNamespace(which=lambda _name: str(executable)))
    process = compiler.subprocess
    monkeypatch.setattr(compiler, "subprocess", SimpleNamespace(
        run=lambda args, **options: calls.append((args, options)),
        CalledProcessError=process.CalledProcessError, TimeoutExpired=process.TimeoutExpired,
        SubprocessError=process.SubprocessError))
    return workspace, directory, executable.resolve(), calls


@pytest.mark.parametrize("relative", sorted(compiler.CI_CCACHE_DIRECTORIES))
@pytest.mark.parametrize("budget", ["1K", "2G", "3G", "4GiB"])
def test_maintenance_binds_the_selected_local_store_and_budget(local_cache, monkeypatch, relative, budget):
    workspace, _directory, executable, calls = local_cache
    selected = workspace / relative
    monkeypatch.setenv("CCACHE_DIR", str(selected))
    monkeypatch.setenv("CCACHE_MAXSIZE", budget)
    compiler.reconcile()
    assert len(calls) == 2
    assert calls[0] == calls[1]
    arguments, options = calls[0]
    assert arguments == ["ccache", "--dir", str(selected), "--cleanup"]
    assert options["executable"] == str(executable)
    assert options["shell"] is False
    assert options["check"] is True
    assert options["timeout"] == 30
    assert options["env"]["CCACHE_DIR"] == str(selected)
    assert options["env"]["CCACHE_CONFIGPATH"] == str(selected / "ccache.conf")
    assert options["env"]["CCACHE_MAXSIZE"] == budget


@pytest.mark.parametrize("budget", ["", "0", "0G", "-1G", "2", "1.5G", "5G", "--clear", "2G; echo unsafe", "١G", "1٢G"])
def test_invalid_budget_never_opens_the_store(local_cache, monkeypatch, budget):
    _workspace, directory, _executable, calls = local_cache
    monkeypatch.setenv("CCACHE_MAXSIZE", budget)
    with pytest.raises(ValueError, match="budget"):
        compiler.reconcile()
    assert not calls
    assert not directory.exists()


@pytest.mark.parametrize("selection", ["relative", "unowned", "outside", "parent"])
def test_unowned_or_non_normalized_directory_never_runs_ccache(local_cache, monkeypatch, selection):
    workspace, directory, _executable, calls = local_cache
    paths = {"relative": Path(".ci-cache/compiler"), "unowned": workspace / "build/cache",
             "outside": workspace.parent / "outside", "parent": directory / ".." / "compiler"}
    monkeypatch.setenv("CCACHE_DIR", str(paths[selection]))
    with pytest.raises(ValueError):
        compiler.reconcile()
    assert not calls
    assert not directory.exists()


@pytest.mark.parametrize("selection", ["store", "parent", "configuration"])
def test_cache_and_configuration_symlinks_are_rejected(local_cache, monkeypatch, selection):
    workspace, directory, _executable, calls = local_cache
    target = workspace.parent / "outside"
    target.mkdir()
    marker = target / "preserved"
    marker.write_text("retain")
    directory.parent.mkdir()
    if selection == "configuration":
        directory.mkdir()
        link, destination = directory / "ccache.conf", marker
    elif selection == "parent":
        link, destination = workspace / "cache-alias", directory.parent
        monkeypatch.setenv("CCACHE_DIR", str(link / "compiler"))
    else:
        link, destination = directory, target
    try:
        link.symlink_to(destination, target_is_directory=destination.is_dir())
    except OSError:
        pytest.skip("Host does not permit fixture symlinks")
    with pytest.raises(ValueError):
        compiler.reconcile()
    assert not calls
    assert marker.read_text() == "retain"


@pytest.mark.parametrize("launcher", ["sccache", "", "ccache; echo unsafe"])
def test_only_the_selected_ccache_backend_can_reconcile(local_cache, monkeypatch, launcher):
    _workspace, directory, _executable, calls = local_cache
    monkeypatch.setenv("HORO_COMPILER_LAUNCHER", launcher)
    with pytest.raises(ValueError, match="launcher"):
        compiler.reconcile()
    assert not calls
    assert not directory.exists()


def test_missing_ccache_never_creates_an_operational_store(local_cache, monkeypatch):
    _workspace, directory, _executable, calls = local_cache
    monkeypatch.setattr(compiler.shutil, "which", lambda _name: None)
    with pytest.raises(ValueError, match="unavailable"):
        compiler.reconcile()
    assert not calls
    assert not directory.exists()


@pytest.mark.parametrize("failure", ["exit", "timeout"])
@pytest.mark.parametrize("failed_pass", [1, 2])
def test_maintenance_cli_reports_failure_without_client_output(local_cache, monkeypatch, capsys, failure, failed_pass):
    secret = uuid.uuid4().hex
    attempts = []

    def fail(_arguments, **_options):
        attempts.append(_arguments)
        if len(attempts) < failed_pass:
            return None
        if failure == "exit":
            raise compiler.subprocess.CalledProcessError(7, "ccache", output=secret, stderr=secret)
        raise compiler.subprocess.TimeoutExpired("ccache", 30, output=secret, stderr=secret)

    monkeypatch.setattr(compiler.subprocess, "run", fail)
    monkeypatch.setattr(sys, "argv", ["compiler_registry.py", "reconcile"])
    assert compiler.main() == 1
    assert len(attempts) == failed_pass
    output = capsys.readouterr().out
    assert "Local compiler cache maintenance unavailable" in output
    assert secret not in output


def test_workflow_fallback_keeps_compilation_and_analysis_selected():
    native = (ROOT / ".github/actions/restore-native-cache/action.yml").read_text(encoding="utf-8")
    sonar = (ROOT / ".github/workflows/sonar.yml").read_text(encoding="utf-8")
    assert native.index("sccache --zero-stats") < native.index("compiler_registry.py reconcile")
    assert 'CCACHE_DISABLE=1\' >> "$GITHUB_ENV"' in native
    configure = sonar.split("- name: Configure instrumented Debug build", 1)[1].split("- name:", 1)[0]
    assert configure.index("compiler_registry.py reconcile") < configure.index("cmake --preset sonar")
    assert "export CCACHE_DISABLE=1" in configure
    assert "cmake --build --preset sonar --target all HoroEditorVulkanShaderCook" in sonar
    assert "xvfb-run -a ctest --preset sonar" in sonar
    assert "-Dsonar.qualitygate.wait=true" in sonar


def cache_statistics(build):
    """Read ccache's supported counters through the same tiny CMake fixture."""
    output = run("cmake", "--build", str(build), "--target", "cache_statistics", "--parallel", "1").stdout
    result = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0] in {"files_in_cache", "cache_size_kibibyte"}:
            result[fields[0]] = int(fields[1])
    assert set(result) == {"files_in_cache", "cache_size_kibibyte"}
    return result


def assert_payload_unchanged(directory, payload):
    """Verify every recorded cache entry keeps its original bytes."""
    for name, digest in payload.items():
        assert compiler.package.digest_file(directory / name) == digest


@pytest.mark.skipif(not shutil.which("ccache") or not shutil.which("cmake"), reason="Native ccache and CMake required")
@pytest.mark.parametrize("budget,retain", [("1M", True), ("1K", False)])
def test_real_import_recounts_entries_and_enforces_the_selected_budget(tmp_path, monkeypatch, budget, retain):
    workspace = tmp_path.resolve()
    monkeypatch.chdir(workspace)
    for name in tuple(os.environ):
        if name.startswith("CCACHE_"):
            monkeypatch.delenv(name)
    directory = workspace / ".ci-cache/compiler"
    configuration = workspace / "local-configuration"
    configuration.write_text("max_size = 1M\nstats = true\n")
    monkeypatch.setenv("HORO_COMPILER_LAUNCHER", "ccache")
    monkeypatch.setenv("CCACHE_DIR", str(directory))
    monkeypatch.setenv("CCACHE_CONFIGPATH", str(configuration))
    monkeypatch.setenv("CCACHE_MAXSIZE", "1M")
    executable = Path(shutil.which("ccache")).resolve().as_posix()
    source, build = workspace / "source ü", workspace / "build ü"
    source.mkdir()
    (source / "fixture.c").write_text("int cache_fixture(void) { return 7; }\n")
    (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.25)
project(CompilerCacheAccounting C)
set(CMAKE_C_COMPILER_LAUNCHER "{executable}")
add_library(cache_fixture OBJECT fixture.c)
add_custom_target(cache_statistics COMMAND "{executable}" --print-stats)
''', encoding="utf-8")
    run("cmake", "-S", str(source), "-B", str(build))
    run("cmake", "--build", str(build), "--target", "cache_fixture", "--parallel", "1")
    original = cache_statistics(build)
    assert original["files_in_cache"] > 0
    assert original["cache_size_kibibyte"] > 0
    archive = workspace / "checkpoint.tar.gz"
    compiler.pack(directory, KEY, archive)
    directory.rename(workspace / "original-cache")
    directory.mkdir()
    staging = workspace / "verified"
    staging.mkdir()
    assert compiler.unpack(archive, NAMESPACE, staging) == KEY
    compiler.merge_entries(staging, directory)
    payload = {p.relative_to(directory): compiler.package.digest_file(p)
               for p in directory.rglob("*") if p.is_file()}
    assert not any(p.name.casefold() in compiler.LOCAL_FILES for p in payload)
    assert cache_statistics(build) == {"files_in_cache": 0, "cache_size_kibibyte": 0}
    monkeypatch.setenv("CCACHE_MAXSIZE", budget)
    compiler.reconcile()
    actual = cache_statistics(build)
    assert actual["cache_size_kibibyte"] * 1024 <= (1_000_000 if retain else 1_000)
    if retain:
        assert actual == original
        assert_payload_unchanged(directory, payload)
    else:
        assert actual["files_in_cache"] == 0
    assert_payload_unchanged(workspace / "original-cache", payload)
    assert configuration.read_text() == "max_size = 1M\nstats = true\n"
