"""Real input transfer and rejection of stale/tampered native analysis evidence."""
from __future__ import annotations

import importlib.util
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

import pytest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("sonar_analysis_inputs", ROOT / ".github/scripts/sonar_analysis_inputs.py")
assert SPEC and SPEC.loader
INPUTS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INPUTS)
NINJA_INPUTS = INPUTS.ninja_inputs


def git(workspace: Path, *args: str) -> str:
    return subprocess.check_output([INPUTS.executable("git"), "-C", str(workspace), *args], text=True).strip()


@pytest.fixture
def source(tmp_path, monkeypatch):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    git(workspace, "init", "-q")
    git(workspace, "config", "core.autocrlf", "false")
    (workspace / "source.cpp").write_bytes(b'#include "generated.hpp"\nint main() { return VALUE - 37; }\n')
    git(workspace, "add", "source.cpp")
    git(workspace, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
        "-c", "commit.gpgsign=false", "commit", "-qm", "Native fixture source")
    build = workspace / "build"
    generated = build / "generated"
    generated.mkdir(parents=True)
    header = generated / "generated.hpp"
    header.write_bytes(b"#define VALUE 37\n")
    sdk = tmp_path / "sdk"
    (sdk / "include").mkdir(parents=True)
    sdk_header = sdk / "include/sdk.hpp"
    sdk_header.write_bytes(b"#define SDK_VALUE 19\n")
    environment = tmp_path / "system.hpp"
    environment.write_bytes(b"#define SYSTEM_VALUE 11\n")
    command = {"directory": str(build), "file": str(workspace / "source.cpp"),
               "arguments": [sys.executable, "-I", str(generated), "-I", str(sdk / "include"),
                             "-c", str(workspace / "source.cpp"), "-o", str(build / "source.o")]}
    (build / "compile_commands.json").write_text(json.dumps([command]), encoding="utf-8")
    for name in INPUTS.REPORTS:
        (build / name).write_bytes(b"<native-fixture-evidence/>\n")
    dependencies = {workspace / "source.cpp", header, sdk_header, environment}
    monkeypatch.setattr(INPUTS, "ninja_inputs", lambda unused: dependencies)
    return {"workspace": workspace, "build": build, "sdk": sdk, "bundle": tmp_path / "bundle",
            "commit": git(workspace, "rev-parse", "HEAD"), "header": header,
            "sdk_header": sdk_header, "environment": environment}


def pack(source):
    INPUTS.pack(source["workspace"], source["build"], source["sdk"], source["bundle"], source["commit"])


def restore(source, **changes):
    values = source | changes
    INPUTS.restore(values["workspace"], values["sdk"], values["bundle"], values["commit"])


def manifest(source):
    return json.loads((source["bundle"] / "manifest.json").read_text(encoding="utf-8"))


def save_manifest(source, value):
    (source["bundle"] / "manifest.json").write_text(json.dumps(value), encoding="utf-8")


def test_restores_actual_generated_sdk_headers_database_and_reports(source):
    pack(source)
    database = (source["build"] / "compile_commands.json").read_bytes()
    shutil.rmtree(source["build"])
    shutil.rmtree(source["sdk"])
    restore(source)
    assert source["header"].read_bytes() == b"#define VALUE 37\n"
    assert source["sdk_header"].read_bytes() == b"#define SDK_VALUE 19\n"
    assert (source["build"] / "compile_commands.json").read_bytes() == database
    for report in INPUTS.REPORTS:
        assert (source["build"] / report).read_bytes() == b"<native-fixture-evidence/>\n"


def test_requires_original_commit_and_absolute_roots(source):
    pack(source)
    with pytest.raises(ValueError, match="source commit"):
        restore(source, commit="0" * 40)
    with pytest.raises(ValueError, match="absolute build paths"):
        restore(source, sdk=source["sdk"].parent / "different-sdk")


def test_refuses_source_modified_after_native_build(source):
    pack(source)
    (source["workspace"] / "source.cpp").write_bytes(b"int main() { return 1; }\n")
    with pytest.raises(subprocess.CalledProcessError):
        restore(source)


def test_refuses_different_system_header_environment(source):
    pack(source)
    source["environment"].write_bytes(b"#define SYSTEM_VALUE 12\n")
    with pytest.raises(ValueError, match="differs from the completed build"):
        restore(source)


def test_refuses_archive_tampering(source):
    pack(source)
    with (source["bundle"] / "inputs.tar.gz").open("ab") as stream:
        stream.write(b"changed")
    with pytest.raises(ValueError, match="archive digest mismatch"):
        restore(source)


def test_missing_native_dependency_is_not_silently_omitted(source):
    source["header"].unlink()
    with pytest.raises(ValueError, match="disappeared"):
        pack(source)


def test_refuses_compiler_response_files_instead_of_losing_include_options(source):
    path = source["build"] / "compile_commands.json"
    value = json.loads(path.read_text(encoding="utf-8"))
    value[0]["arguments"].append("@flags.rsp")
    path.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(ValueError, match="Response-file"):
        pack(source)


def test_git_internals_cannot_enter_native_payload(source, monkeypatch):
    monkeypatch.setattr(INPUTS, "ninja_inputs", lambda unused: {source["workspace"] / ".git/config"})
    with pytest.raises(ValueError, match="Git internals"):
        pack(source)


def test_fallback_headers_skip_linked_worktree_markers_without_losing_headers(source):
    include_root = source["header"].parent
    linked = include_root / "linked-dependency"
    git(source["workspace"], "worktree", "add", "--quiet", "--detach", str(linked))
    assert (linked / ".git").is_file()
    (linked / "FallbackHeader").write_text("#define FALLBACK_VALUE 29\n", encoding="utf-8")

    pack(source)

    records = manifest(source)["files"]
    assert any(item["path"].endswith("linked-dependency/FallbackHeader") for item in records)
    assert not any(".git" in Path(item["path"]).parts for item in records)
    with tarfile.open(source["bundle"] / "inputs.tar.gz", "r:gz") as archive:
        assert not any(".git" in Path(member.name).parts for member in archive.getmembers())


def create_git_alias(alias, target, *, directory=False):
    try:
        alias.symlink_to(target, target_is_directory=directory)
    except OSError as error:
        if getattr(error, "winerror", None) == 1314:
            pytest.skip("creating symlinks requires a Windows privilege")
        raise


@pytest.mark.parametrize("directory", [False, True])
def test_fallback_headers_do_not_follow_git_metadata_aliases(source, directory):
    git_root = source["workspace"] / ".git"
    alias = source["header"].parent / "GitMetadataAlias"
    create_git_alias(alias, git_root if directory else git_root / "config", directory=directory)
    fallback_header = alias.with_name("FallbackHeader")
    fallback_header.write_text("#define FALLBACK_VALUE 29\n", encoding="utf-8")
    valid_alias = alias.with_name("AliasHeader")
    create_git_alias(valid_alias, fallback_header)

    pack(source)

    paths = [Path(item["path"]) for item in manifest(source)["files"]]
    assert {fallback_header.name, valid_alias.name}.issubset(path.name for path in paths)
    assert not any(alias.name in path.parts for path in paths)


def test_explicit_git_metadata_alias_is_rejected(source, monkeypatch):
    alias = source["header"].parent / "GitConfig.hpp"
    create_git_alias(alias, source["workspace"] / ".git/config")
    monkeypatch.setattr(INPUTS, "ninja_inputs", lambda unused: {alias})
    with pytest.raises(ValueError, match="Git internals"):
        pack(source)


def test_old_ninja_records_are_not_mistaken_for_current_compiler_inputs(source, monkeypatch):
    monkeypatch.setattr(INPUTS, "executable", lambda name: name)
    output = "obsolete.o: #deps 1, deps mtime 1 (VALID)\n    /removed/obsolete.hpp\n\n"
    output += "source.o: #deps 1, deps mtime 2 (VALID)\n    generated/generated.hpp\n\n"
    monkeypatch.setattr(INPUTS.subprocess, "check_output", lambda *args, **kwargs: output)
    assert NINJA_INPUTS(source["build"]) == {source["header"]}


def test_unused_generated_binaries_are_not_added_as_fallback_headers(source):
    binary = source["header"].parent / "native-executable"
    binary.write_bytes(b"\x7fELF\x00native executable bytes")
    pack(source)
    assert not any(item["path"].endswith("native-executable") for item in manifest(source)["files"])


@pytest.mark.parametrize("kind", ["traversal", "symlink", "tracked-source", "tracked-dot-alias", "tracked-separator-alias", "git-metadata-alias"])
def test_rejects_unsafe_members_before_they_can_replace_files(source, kind):
    pack(source)
    value = manifest(source)
    archive = source["bundle"] / "inputs.tar.gz"
    payload = b"unexpected compiler input"
    git_config = source["workspace"] / ".git/config"
    original_git_config = git_config.read_bytes()
    if kind == "git-metadata-alias":
        alias = source["build"] / "GitConfig.hpp"
        create_git_alias(alias, git_config)
        item = {"scope": "workspace", "path": "build/GitConfig.hpp"}
    elif kind == "tracked-source":
        item = {"scope": "workspace", "path": "source.cpp"}
    elif kind == "tracked-dot-alias":
        item = {"scope": "workspace", "path": "./source.cpp"}
    elif kind == "tracked-separator-alias":
        item = {"scope": "workspace", "path": ".//source.cpp"}
    else:
        item = {"scope": "workspace", "path": "../outside" if kind == "traversal" else "build/link.hpp"}
    item.update(size=len(payload), sha256=hashlib.sha256(payload).hexdigest())
    value["files"] = [item]
    with tarfile.open(archive, "w:gz") as output:
        member = tarfile.TarInfo(f"workspace/{item['path']}")
        member.size = len(payload)
        if kind == "symlink":
            member.type = tarfile.SYMTYPE
            member.linkname = "../../outside"
        output.addfile(member, io.BytesIO(payload))
    value["archiveSha256"] = INPUTS.digest(archive)
    save_manifest(source, value)
    before = (source["workspace"] / "source.cpp").read_bytes()
    with pytest.raises(ValueError):
        restore(source)
    assert (source["workspace"] / "source.cpp").read_bytes() == before
    assert git_config.read_bytes() == original_git_config
    assert not (source["workspace"].parent / "outside").exists()


@pytest.mark.skipif(os.name != "posix", reason="Hosted Sonar uses the Linux GCC/Ninja toolchain")
def test_real_ninja_dependency_graph_survives_transfer_and_recompiles(source, monkeypatch):
    if not shutil.which("g++") or not shutil.which("ninja"):
        pytest.skip("The native dependency-transfer proof requires GCC and Ninja")
    monkeypatch.undo()
    workspace, build = source["workspace"], source["build"]
    command = [INPUTS.executable("g++"), "-I", str(build / "generated"), "-MMD", "-MF", str(build / "source.d"),
               "-c", str(workspace / "source.cpp"), "-o", str(build / "source.o")]
    (build / "build.ninja").write_text(
        "rule compile\n  command = " + " ".join(command) + "\n  depfile = source.d\n  deps = gcc\n"
        "build source.o: compile ../source.cpp\n", encoding="utf-8")
    (build / "compile_commands.json").write_text(json.dumps([
        {"directory": str(build), "file": str(workspace / "source.cpp"), "arguments": command}
    ]), encoding="utf-8")
    subprocess.run([INPUTS.executable("ninja"), "-C", str(build)], check=True)
    database = (build / "compile_commands.json").read_bytes()
    pack(source)
    shutil.rmtree(build)
    restore(source)
    assert (build / "compile_commands.json").read_bytes() == database
    subprocess.run(command, check=True, cwd=build)
