#!/usr/bin/env python3
"""Transfer the real compile inputs and coverage between identical hosted runners."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import shutil
import subprocess
import tarfile


REPORTS = ("coverage.xml", "python-coverage.xml", "ctest.xml", "coverage-ctest-discovery.json")
BUILD_OUTPUTS = {".o", ".obj", ".a", ".lib", ".so", ".dll", ".pdb", ".gcda", ".gcno", ".d", ".ninja_deps", ".ninja_log"}
SOURCE_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp", ".tpp", ".def", ".c", ".cc", ".cpp", ".cxx", ".S"}


def absolute(path: Path | str) -> Path:
    """Keep the build's lexical absolute paths, including include-directory aliases."""
    return Path(os.path.abspath(path))


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def executable(name: str) -> str:
    """Resolve trusted runner tools before starting a process without a shell."""
    resolved = shutil.which(name)
    if resolved is None:
        raise ValueError(f"Required native tool is unavailable: {name}")
    return str(absolute(resolved))


def git(workspace: Path, *args: str) -> str:
    return subprocess.check_output([executable("git"), "-C", str(workspace), *args], text=True).strip()


def source_identity(workspace: Path, expected: str) -> str:
    if git(workspace, "rev-parse", "HEAD") != expected:
        raise ValueError("Analysis inputs do not belong to the requested source commit")
    git(workspace, "diff", "--quiet", "HEAD", "--")
    return git(workspace, "rev-parse", "HEAD^{tree}")


def command_inputs(entry: dict) -> tuple[list[str], list[Path]]:
    """Retain include roots for units that CMake describes but excludes from all."""
    args = entry.get("arguments") or shlex.split(entry["command"])
    directory = Path(entry["directory"])
    paths = [absolute(directory / entry["file"])]
    flags = ("-isystem", "-iquote", "-idirafter", "-include", "-imacros", "-I")
    index = 0
    while index < len(args):
        argument = args[index]
        if argument.startswith("@"):
            raise ValueError("Response-file compile commands need explicit input capture")
        for flag in flags:
            if argument == flag:
                index += 1
                paths.append(absolute(directory / args[index]))
                break
            if argument.startswith(flag) and len(argument) > len(flag):
                paths.append(absolute(directory / argument[len(flag):]))
                break
        index += 1
    return args, paths


def compile_outputs(entries: list[dict]) -> set[Path]:
    outputs = set()
    for entry in entries:
        args = entry.get("arguments") or shlex.split(entry["command"])
        output = entry.get("output") or args[args.index("-o") + 1]
        outputs.add(absolute(Path(entry["directory"]) / output))
    return outputs


def ninja_inputs(build: Path) -> set[Path]:
    entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    outputs = compile_outputs(entries)
    output = subprocess.check_output([executable("ninja"), "-C", str(build), "-t", "deps"], text=True)
    paths = set()
    current = False
    for line in output.splitlines():
        if ": #deps " in line and not line.startswith(" "):
            current = absolute(build / line.split(": #deps ", 1)[0]) in outputs
        elif current and line.startswith("    ") and line.strip():
            paths.add(absolute(build / line.strip()))
    if not paths:
        raise ValueError("The completed native build has no recorded header dependencies")
    return paths


def scope(path: Path, roots: dict[str, Path]) -> tuple[str, str]:
    for name, root in roots.items():
        if path.is_relative_to(root):
            relative = path.relative_to(root)
            if ".git" in relative.parts:
                raise ValueError("Git internals are never analysis inputs")
            return name, relative.as_posix()
    return "environment", str(path)


def is_header_candidate(path: Path) -> bool:
    if path.name in BUILD_OUTPUTS or not path.is_file():
        return False
    if path.suffix in SOURCE_SUFFIXES:
        return True
    if path.suffix:
        return False
    # Extensionless headers are common; ELF executables and object/cache data are not headers.
    with path.open("rb") as stream:
        return b"\0" not in stream.read(8192)


def include_files(root: Path):
    """Materialize header aliases as ordinary bytes; never archive Git credentials."""
    for directory, children, names in os.walk(root, followlinks=True):
        children[:] = [name for name in children if name not in {".git", ".sonar", "__pycache__"}]
        ancestors = {parent.resolve() for parent in (Path(directory), *Path(directory).parents) if parent.is_relative_to(root)}
        if any((Path(directory) / child).resolve() in ancestors for child in children):
            raise ValueError("Cyclic include-directory alias")
        for name in names:
            path = Path(directory) / name
            if is_header_candidate(path):
                yield absolute(path)


def compilation_inputs(entries: list[dict], roots: dict[str, Path]) -> tuple[set[Path], set[Path]]:
    inputs = set()
    directories = set()
    for entry in entries:
        args, paths = command_inputs(entry)
        for path in paths:
            if path.is_file():
                inputs.add(path)
            elif path.is_dir() and scope(path, roots)[0] != "environment":
                directories.add(path)
        inputs.add(Path(executable(args[0])))
    return inputs, directories


def fallback_headers(directories: set[Path], roots: dict[str, Path], tracked: set[str]):
    for directory in sorted(directories, key=lambda item: len(item.parts)):
        if any(directory != parent and directory.is_relative_to(parent) for parent in directories):
            continue
        for path in include_files(directory):
            owner, relative = scope(path, roots)
            # The exact checkout already supplies every tracked file, including unused fixtures.
            if owner != "workspace" or relative not in tracked:
                yield path


def collect(build: Path, roots: dict[str, Path], tracked: set[str]) -> set[Path]:
    database = build / "compile_commands.json"
    entries = json.loads(database.read_text(encoding="utf-8"))
    if not isinstance(entries, list) or not entries:
        raise ValueError("The real compilation database is empty")
    inputs, directories = compilation_inputs(entries, roots)
    inputs.update(ninja_inputs(build))
    inputs.update(fallback_headers(directories, roots, tracked))
    inputs.add(database)
    for report in REPORTS:
        path = build / report
        if not path.is_file():
            raise ValueError(f"Missing completed coverage evidence: {report}")
        inputs.add(path)
    return inputs


def pack(workspace: Path, build: Path, sdk: Path, bundle: Path, commit: str) -> None:
    tree = source_identity(workspace, commit)
    roots = {"workspace": workspace, "sdk": sdk}
    tracked = set(git(workspace, "ls-files", "-z").split("\0"))
    bundle.mkdir(parents=True, exist_ok=False)
    files = []
    with tarfile.open(bundle / "inputs.tar.gz", "w:gz", compresslevel=1) as archive:
        for path in sorted(collect(build, roots, tracked)):
            if not path.is_file():
                raise ValueError(f"Recorded native input disappeared: {path}")
            owner, relative = scope(path, roots)
            item = {"scope": owner, "path": relative, "sha256": digest(path), "size": path.stat().st_size}
            if owner == "workspace" and relative in tracked:
                item["scope"] = "checkout"
            elif owner != "environment":
                member = tarfile.TarInfo(f"{owner}/{relative}")
                member.size = item["size"]
                member.mode = 0o644
                with path.open("rb") as stream:
                    archive.addfile(member, stream)
            files.append(item)
    manifest = {"schema": 1, "commit": commit, "tree": tree, "roots": {key: str(value) for key, value in roots.items()},
                "archiveSha256": digest(bundle / "inputs.tar.gz"), "files": files}
    source_identity(workspace, commit)
    (bundle / "manifest.json").write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
    print(f"Captured {len(files)} actual compiler/header/report inputs for {commit}")


def target(root: Path, relative: str) -> Path:
    value = PurePosixPath(relative)
    if relative != value.as_posix() or "\\" in relative:
        raise ValueError("Analysis input paths must use their canonical POSIX spelling")
    if value.is_absolute() or not value.parts or any(part in {"..", ".git"} for part in value.parts):
        raise ValueError("Unsafe analysis input path")
    result = root / value
    if not result.resolve().is_relative_to(root.resolve()):
        raise ValueError("Analysis input escapes its declared root")
    return result


def verify_file(path: Path, item: dict) -> None:
    if not path.is_file() or path.stat().st_size != item["size"] or digest(path) != item["sha256"]:
        raise ValueError(f"Analysis environment/input differs from the completed build: {path}")


def restore_inventory(workspace: Path, roots: dict[str, Path], files: list[dict]) -> dict:
    """Verify immutable inputs before accepting any generated-file destinations."""
    tracked = set(git(workspace, "ls-files", "-z").split("\0"))
    expected = {}
    for item in files:
        owner = item["scope"]
        if owner in {"checkout", "environment"}:
            path = target(workspace, item["path"]) if owner == "checkout" else Path(item["path"])
            if not path.is_absolute():
                raise ValueError("Environment inputs must retain their absolute paths")
            verify_file(path, item)
            continue
        if owner not in roots:
            raise ValueError("Unknown analysis input scope")
        if owner == "workspace" and item["path"] in tracked:
            raise ValueError("Generated inputs cannot replace checked-out source")
        name = f"{owner}/{item['path']}"
        if name in expected:
            raise ValueError("Duplicate analysis input")
        expected[name] = (target(roots[owner], item["path"]), item)
    return expected


def restore_payload(archive_path: Path, roots: dict[str, Path], expected: dict) -> None:
    """Write only inventoried ordinary files after checking each payload digest."""
    with tarfile.open(archive_path, "r:gz") as archive:
        members = archive.getmembers()
        if len(members) != len(expected) or {member.name for member in members} != expected.keys():
            raise ValueError("Analysis archive inventory differs from its manifest")
        for member in members:
            path, item = expected[member.name]
            root_name, relative = member.name.split("/", 1)
            # Validate again after previous members have been written.
            path = target(roots[root_name], relative)
            if not member.isfile() or member.size != item["size"]:
                raise ValueError("Only exact ordinary compiler/header/report files may be restored")
            stream = archive.extractfile(member)
            if stream is None:
                raise ValueError("Missing analysis payload")
            data = stream.read()
            if hashlib.sha256(data).hexdigest() != item["sha256"]:
                raise ValueError("Analysis payload digest mismatch")
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("wb") as output:
                output.write(data)
    for path, item in expected.values():
        verify_file(path, item)


def restore_working_directories(roots: dict[str, Path], expected: dict) -> None:
    """Recreate the original compiler directories without altering its database."""
    databases = [path for path, unused in expected.values() if path.name == "compile_commands.json"]
    if len(databases) != 1:
        raise ValueError("Exactly one original compilation database is required")
    database = databases[0]
    for entry in json.loads(database.read_text(encoding="utf-8")):
        directory = absolute(entry["directory"])
        owner, relative = scope(directory, roots)
        if owner in roots:
            destination = roots[owner] if directory == roots[owner] else target(roots[owner], relative)
            destination.mkdir(parents=True, exist_ok=True)
        elif not directory.is_dir():
            raise ValueError("Original external compiler working directory is unavailable")


def restore(workspace: Path, sdk: Path, bundle: Path, commit: str) -> None:
    manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
    roots = {"workspace": workspace, "sdk": sdk}
    tree = source_identity(workspace, commit)
    if (manifest.get("schema") != 1 or manifest.get("commit") != commit or manifest.get("tree") != tree
            or manifest.get("roots") != {key: str(value) for key, value in roots.items()}):
        raise ValueError("Source commit/tree and original absolute build paths must match")
    archive_path = bundle / "inputs.tar.gz"
    if digest(archive_path) != manifest["archiveSha256"]:
        raise ValueError("Analysis input archive digest mismatch")
    expected = restore_inventory(workspace, roots, manifest["files"])
    restore_payload(archive_path, roots, expected)
    restore_working_directories(roots, expected)
    source_identity(workspace, commit)
    print(f"Verified/restored {len(manifest['files'])} exact build inputs; compilation database and coverage are unchanged")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("pack", "restore"))
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--sdk-root", type=Path, required=True)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--build", type=Path)
    args = parser.parse_args()
    workspace, sdk, bundle = map(absolute, (args.workspace, args.sdk_root, args.bundle))
    if args.mode == "pack":
        if args.build is None:
            parser.error("pack requires --build")
        pack(workspace, absolute(args.build), sdk, bundle, args.source_sha)
    else:
        restore(workspace, sdk, bundle, args.source_sha)


if __name__ == "__main__":
    main()
