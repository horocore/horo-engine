"""Catalog-selected shader executables retain confinement and immutable byte admission."""
import hashlib
import os

import pytest
import project


def executable_fixture(tmp_path):
    root = tmp_path / "verified tools ü"
    executable = root / "release/bin/dxc"
    executable.parent.mkdir(parents=True)
    executable.write_bytes(b"verified compiler fixture")
    executable.chmod(0o755)
    digest = "sha256:" + hashlib.sha256(executable.read_bytes()).hexdigest()
    return root, executable, digest


def test_tool_cli_returns_only_the_verified_nested_catalog_path(tmp_path, capsys):
    root, executable, digest = executable_fixture(tmp_path)
    project.verify_shader_tool_main(["--root", str(root), "--relative-path", "release/bin/dxc", "--sha256", digest])
    assert capsys.readouterr().out == str(executable.resolve()) + "\n"
    assert executable.read_bytes() == b"verified compiler fixture"


@pytest.mark.parametrize("relative", ["", "/bin/dxc", "../dxc", "release/../dxc", "release//dxc", "./dxc", r"C:\bin\dxc"])
def test_tool_rejects_unconfined_catalog_paths(tmp_path, relative):
    root, executable, digest = executable_fixture(tmp_path)
    with pytest.raises(ValueError, match="relative POSIX"):
        project.verify_shader_tool_executable(root, relative, digest)
    assert executable.read_bytes() == b"verified compiler fixture"


def test_tool_rejects_a_resolved_symlink_escape_even_with_matching_bytes(tmp_path):
    root, executable, digest = executable_fixture(tmp_path)
    escaped = tmp_path / "outside"
    escaped.write_bytes(executable.read_bytes())
    escaped.chmod(0o755)
    executable.unlink()
    executable.symlink_to(escaped)
    with pytest.raises(ValueError, match="escapes"):
        project.verify_shader_tool_executable(root, "release/bin/dxc", digest)
    assert escaped.read_bytes() == b"verified compiler fixture"


@pytest.mark.parametrize("digest", ["not-a-digest", "sha256:" + "0" * 64])
def test_tool_rejects_invalid_or_mismatched_identity_without_mutation(tmp_path, digest):
    root, executable, _ = executable_fixture(tmp_path)
    with pytest.raises(ValueError):
        project.verify_shader_tool_executable(root, "release/bin/dxc", digest)
    assert executable.read_bytes() == b"verified compiler fixture"


@pytest.mark.skipif(os.name == "nt", reason="POSIX executable mode admission")
def test_tool_rejects_nonexecutable_bytes_before_invocation(tmp_path):
    root, executable, digest = executable_fixture(tmp_path)
    executable.chmod(0o644)
    with pytest.raises(ValueError, match="not executable"):
        project.verify_shader_tool_executable(root, "release/bin/dxc", digest)


def test_tool_hashing_obeys_the_existing_file_size_bound(tmp_path, monkeypatch):
    root, executable, digest = executable_fixture(tmp_path)
    monkeypatch.setattr(project, "MAXIMUM_FILE", executable.stat().st_size - 1)
    with pytest.raises(ValueError, match="size bound"):
        project.verify_shader_tool_executable(root, "release/bin/dxc", digest)
