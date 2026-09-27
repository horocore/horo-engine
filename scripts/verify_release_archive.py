#!/usr/bin/env python3
"""Verify that a distributable embeds the exact reviewed notes snapshot."""

import argparse
import sys
import tarfile
import zipfile
from pathlib import Path, PurePosixPath

from parse_changelog import MAX_SNAPSHOT_BYTES, NotesError


def notes_member(name: str) -> bool:
    """Accept only a root or single package-directory notes file."""
    if not name or "\\" in name or name.startswith("/"):
        raise NotesError(f"unsafe package member path: {name}")
    parts = PurePosixPath(name).parts
    if any(part in (".", "..") for part in name.split("/")) or any(not part for part in name.split("/")):
        raise NotesError(f"unsafe package member path: {name}")
    return parts[-1] == "release-notes.json" and len(parts) in (1, 2)


def verify_tar(snapshot: bytes, archive: Path) -> None:
    # Read-only tar stream: extractfile returns bytes and never materializes member paths.
    with tarfile.open(archive, "r:gz") as package:
        members = [member for member in package.getmembers()
                   if notes_member(member.name.rstrip("/"))]
        if len(members) != 1 or not members[0].isfile() or members[0].size != len(snapshot):
            raise NotesError("package must contain exactly one matching release-notes.json")
        content = package.extractfile(members[0])
        if content is None or content.read(MAX_SNAPSHOT_BYTES + 1) != snapshot:
            raise NotesError("packaged release notes differ from the reviewed snapshot")


def verify_zip(snapshot: bytes, archive: Path) -> None:
    with zipfile.ZipFile(archive) as package:
        members = [member for member in package.infolist()
                   if notes_member(member.filename.rstrip("/"))]
        if len(members) != 1 or members[0].is_dir() or members[0].file_size != len(snapshot):
            raise NotesError("package must contain exactly one matching release-notes.json")
        with package.open(members[0]) as content:
            if content.read(MAX_SNAPSHOT_BYTES + 1) != snapshot:
                raise NotesError("packaged release notes differ from the reviewed snapshot")


def verify_archive(snapshot: bytes, archive: Path) -> None:
    if not snapshot or len(snapshot) > MAX_SNAPSHOT_BYTES:
        raise NotesError("reviewed notes snapshot is missing or oversized")
    if archive.name.endswith(".tar.gz"):
        verify_tar(snapshot, archive)
    elif archive.suffix == ".zip":
        verify_zip(snapshot, archive)
    else:
        raise NotesError(f"unsupported release archive format: {archive.name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    try:
        verify_archive(args.snapshot.read_bytes(), args.archive)
    except (OSError, ValueError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"[release-notes] {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
