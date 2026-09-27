"""Release-notes source, selection, snapshot, and Welcome projection contracts."""

import json
import io
from contextlib import redirect_stderr
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from parse_changelog import (NotesError, main as parse_main, make_snapshot, parse_changelog, render_header,
                             select_range, select_version, snapshot_bytes, version_parts)
from verify_release_notes import release_body, verify
from verify_release_archive import verify_archive


SOURCE = """# Changelog

## [Unreleased]

### Added
- Future work.

## [0.2.0] — 2026-09-26

### Added
- Reviewed feature.

### Fixed
- Reviewed fix.

## [0.1.0] — 2026-08-01

### Added
- Earlier feature.
"""


def write_archive_fixture(root: Path, snapshot: bytes) -> tuple[Path, Path]:
    tar_path = root / "release.tar.gz"
    zip_path = root / "release.zip"
    with tarfile.open(tar_path, "w:gz") as package:
        directory = tarfile.TarInfo("HoroEngine/")
        directory.type = tarfile.DIRTYPE
        package.addfile(directory)
        member = tarfile.TarInfo("HoroEngine/release-notes.json")
        member.size = len(snapshot)
        package.addfile(member, io.BytesIO(snapshot))
    with zipfile.ZipFile(zip_path, "w") as package:
        package.writestr("release-notes.json", snapshot)
    return tar_path, zip_path


class ReleaseNotesTests(unittest.TestCase):
    def test_exact_version_and_unreleased_exclusion(self):
        entries = parse_changelog(SOURCE)
        self.assertEqual([entry["version"] for entry in entries], ["0.2.0", "0.1.0"])
        self.assertEqual([entry["version"] for entry in select_range(entries, "0.1.0", "0.2.0")], ["0.2.0"])
        prerelease = parse_changelog("## [0.2.0] — 2026-09-26\n### Added\n- Stable.\n"
                                     "## [0.2.0-rc.1] — 2026-09-25\n### Added\n- Preview.\n")
        self.assertEqual([entry["version"] for entry in select_range(prerelease, "0.2.0-rc.1", "0.2.0")], ["0.2.0"])
        with self.assertRaisesRegex(NotesError, "exact candidate version"):
            select_version(entries, "0.2.0+build.1")
        with self.assertRaises(NotesError):
            select_version(entries, "Unreleased")

    def test_same_snapshot_drives_distribution_markdown_and_welcome(self):
        snapshot = make_snapshot(select_version(parse_changelog(SOURCE), "0.2.0"), "horo-editor")
        encoded = snapshot_bytes(snapshot)
        self.assertEqual(json.loads(encoded)["markdown"],
                         "## [0.2.0] — 2026-09-26\n\n### Added\n- Reviewed feature.\n\n### Fixed\n- Reviewed fix.\n")
        header = render_header(snapshot)
        self.assertIn("v0.2.0", header)
        self.assertIn("Reviewed feature.", header)
        self.assertIn("Reviewed fix.", header)
        self.assertNotIn("Future work.", encoded.decode())
        self.assertLessEqual(len(encoded), 32768)
        verify(encoded, "v0.2.0", 'set(HORO_ENGINE_VERSION "0.2.0")', snapshot["markdown"])
        with self.assertRaises(NotesError):
            verify(encoded, "v0.2.0", 'set(HORO_ENGINE_VERSION "0.2.0")', "Different GitHub Release body")
        with self.assertRaises(NotesError):
            verify(encoded, "v0.2.0", 'set(HORO_ENGINE_VERSION "0.2.0")', snapshot["markdown"] + "\n")
        with self.assertRaises(NotesError):
            verify(encoded, "v0.2.1", 'set(HORO_ENGINE_VERSION "0.2.0")', snapshot["markdown"])

    def test_rejects_missing_malformed_oversized_and_unsafe_content(self):
        for source in (
            SOURCE.replace("### Added\n- Reviewed feature.", "### Added"),
            SOURCE.replace("2026-09-26", "2026-02-30"),
            SOURCE.replace("- Reviewed feature.", "- <script>danger</script>"),
            SOURCE.replace("- Reviewed feature.", "- a < b"),
            SOURCE.replace("- Reviewed feature.", "- a > b"),
            SOURCE.replace("- Reviewed feature.", "- [bad](javascript:evil)"),
            SOURCE.replace("- Reviewed feature.", "- " + "x" * 2049),
            SOURCE + "\n## [0.2.0] — 2026-09-27\n### Fixed\n- Duplicate.\n",
            "## [0.3.0] — not-a-date\n### Added\n- Invalid.\n" + SOURCE,
            "## Not a version\n" + SOURCE,
            "x" * 262145,
        ):
            with self.subTest(source=source[:40]), self.assertRaises(NotesError):
                parse_changelog(source)
        entries = parse_changelog(SOURCE)
        with self.assertRaises(NotesError):
            select_version(entries, "0.3.0")
        for invalid in ("1." + "9" * 100000 + ".0", "1.0.0-01", "1.0.0+", "1.0.0-rc..1"):
            with self.subTest(version=invalid[:30]):
                with self.assertRaises(NotesError):
                    version_parts(invalid)

    def test_generated_files_stay_in_build_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "CHANGELOG.md"
            source.write_text(SOURCE, encoding="utf-8")
            arguments = ["parse_changelog.py", "--source", str(source),
                         "--version", "0.2.0", "--product", "horo-editor"]
            errors = io.StringIO()
            with patch("sys.argv", arguments), patch.object(Path, "cwd", return_value=root), redirect_stderr(errors):
                result = parse_main()
            self.assertEqual(result, 0, errors.getvalue())
            self.assertEqual((root / "generated" / "ReleaseNotesSnapshot.json").read_bytes(),
                             snapshot_bytes(make_snapshot(select_version(parse_changelog(SOURCE), "0.2.0"), "horo-editor")))
            self.assertTrue((root / "generated" / "GeneratedBuildInfo.h").is_file())
            outside = root / "outside"
            outside.mkdir()
            (root / "generated" / "ReleaseNotesSnapshot.json").unlink()
            (root / "generated" / "ReleaseNotesSnapshot.json").symlink_to(outside / "stolen.json")
            errors = io.StringIO()
            with patch("sys.argv", arguments), patch.object(Path, "cwd", return_value=root), redirect_stderr(errors):
                result = parse_main()
            self.assertNotEqual(result, 0)
            self.assertIn("must not be a symlink", errors.getvalue())
            self.assertFalse((outside / "stolen.json").exists())

    def test_archive_contains_exact_snapshot(self):
        snapshot = snapshot_bytes(make_snapshot(select_version(parse_changelog(SOURCE), "0.2.0"), "horo-editor"))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tar_path, zip_path = write_archive_fixture(root, snapshot)
            verify_archive(snapshot, tar_path)
            verify_archive(snapshot, zip_path)
            for archive in (tar_path, zip_path):
                with self.subTest(archive=archive.name), self.assertRaises(NotesError):
                    verify_archive(b"different reviewed bytes", archive)
            missing = root / "missing.zip"
            with zipfile.ZipFile(missing, "w") as package:
                package.writestr("HoroEditor.exe", b"binary fixture")
            with self.assertRaisesRegex(NotesError, "exactly one"):
                verify_archive(snapshot, missing)
            duplicate = root / "duplicate.zip"
            with zipfile.ZipFile(duplicate, "w") as package:
                package.writestr("one/release-notes.json", snapshot)
                package.writestr("two/release-notes.json", snapshot)
            with self.assertRaisesRegex(NotesError, "exactly one"):
                verify_archive(snapshot, duplicate)

            with self.assertRaisesRegex(NotesError, "unsupported release archive format"):
                verify_archive(snapshot, root / "release.7z")
            with self.assertRaisesRegex(NotesError, "missing or oversized"):
                verify_archive(b"", zip_path)
            with self.assertRaisesRegex(NotesError, "missing or oversized"):
                verify_archive(b"x" * 32769, zip_path)

    def test_archive_rejects_traversal_and_non_file_notes(self):
        snapshot = snapshot_bytes(make_snapshot(select_version(parse_changelog(SOURCE), "0.2.0"), "horo-editor"))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "work"
            root.mkdir()
            tar_path, zip_path = write_archive_fixture(root, snapshot)

            for unsafe in ("../release-notes.json", str(root.parent / "absolute-release-notes.json"),
                           "package/../release-notes.json", "package\\release-notes.json"):
                with self.subTest(unsafe=unsafe):
                    with tarfile.open(tar_path, "w:gz") as package:
                        member = tarfile.TarInfo(unsafe)
                        member.size = len(snapshot)
                        package.addfile(member, io.BytesIO(snapshot))
                    with zipfile.ZipFile(zip_path, "w") as package:
                        package.writestr(unsafe, snapshot)
                    for archive in (tar_path, zip_path):
                        with self.assertRaisesRegex(NotesError, "unsafe package member path"):
                            verify_archive(snapshot, archive)
                    self.assertFalse((root.parent / "outside.txt").exists())
                    self.assertFalse((root.parent / "absolute-release-notes.json").exists())

            # A valid notes entry cannot excuse a hostile sibling archive path.
            with tarfile.open(tar_path, "w:gz") as package:
                member = tarfile.TarInfo("HoroEngine/release-notes.json")
                member.size = len(snapshot)
                package.addfile(member, io.BytesIO(snapshot))
                member = tarfile.TarInfo("../outside.txt")
                member.size = 1
                package.addfile(member, io.BytesIO(b"x"))
            with zipfile.ZipFile(zip_path, "w") as package:
                package.writestr("release-notes.json", snapshot)
                package.writestr("../outside.txt", b"x")
            for archive in (tar_path, zip_path):
                with self.subTest(archive=archive.name):
                    with self.assertRaisesRegex(NotesError, "unsafe package member path"):
                        verify_archive(snapshot, archive)
            with tarfile.open(tar_path, "w:gz") as package:
                member = tarfile.TarInfo("release-notes.json")
                member.type = tarfile.SYMTYPE
                member.linkname = "../outside.txt"
                package.addfile(member)
            with self.assertRaisesRegex(NotesError, "exactly one"):
                verify_archive(snapshot, tar_path)
            self.assertFalse((root.parent / "outside.txt").exists())
            with zipfile.ZipFile(zip_path, "w") as package:
                package.writestr("release-notes.json/", b"")
            with self.assertRaisesRegex(NotesError, "exactly one"):
                verify_archive(snapshot, zip_path)

    def test_release_verifier_rejects_invalid_identity_and_snapshot(self):
        snapshot = snapshot_bytes(make_snapshot(select_version(parse_changelog(SOURCE), "0.2.0"), "horo-editor"))
        cmake = 'set(HORO_ENGINE_VERSION "0.2.0")'
        markdown = json.loads(snapshot)["markdown"]
        for data, tag, configured, body in (
            (snapshot, "0.2.0", cmake, markdown),
            (snapshot, "v0.2.0-01", cmake, markdown),
            (snapshot, "v0.2.0", cmake + "\n" + cmake, markdown),
            (b"", "v0.2.0", cmake, markdown),
            (b"{bad", "v0.2.0", cmake, markdown),
            (b"[]", "v0.2.0", cmake, markdown),
            (snapshot.replace(b'horo-editor', b'another-product'), "v0.2.0", cmake, markdown),
        ):
            with self.subTest(tag=tag, data=data[:20]):
                with self.assertRaises(NotesError):
                    verify(data, tag, configured, body)
        verify(snapshot, "v0.2.0", cmake, markdown.replace("\n", "\r\n"))

    def test_release_body_fetch_uses_bounded_authenticated_api_request(self):
        with patch.dict("os.environ", {"GITHUB_REPOSITORY": "horocore/horo-engine", "GH_TOKEN": str(id(self))}):
            with patch("verify_release_notes.http.client.HTTPSConnection") as constructor:
                connection = constructor.return_value
                connection.getresponse.return_value.status = 200
                connection.getresponse.return_value.read.return_value = b'{"body":"Reviewed"}'
                self.assertEqual(release_body("v0.2.0"), "Reviewed")
                constructor.assert_called_once_with("api.github.com", timeout=30)
                self.assertEqual(connection.request.call_args.args[:2],
                                 ("GET", "/repos/horocore/horo-engine/releases/tags/v0.2.0"))
                self.assertEqual(connection.request.call_args.kwargs["headers"]["Accept"],
                                 "application/vnd.github+json")
                connection.close.assert_called_once()

    def test_release_body_fetch_rejects_untrusted_responses(self):
        with patch.dict("os.environ", {"GITHUB_REPOSITORY": "../escape", "GH_TOKEN": str(id(self))}):
            with self.assertRaisesRegex(NotesError, "GITHUB_REPOSITORY"):
                release_body("v0.2.0")
        with patch.dict("os.environ", {"GITHUB_REPOSITORY": "horocore/horo-engine", "GH_TOKEN": str(id(self))}):
            with patch("verify_release_notes.http.client.HTTPSConnection") as constructor:
                connection = constructor.return_value
                connection.getresponse.return_value.status = 404
                with self.assertRaisesRegex(NotesError, "HTTP 404"):
                    release_body("v0.2.0")
                connection.getresponse.return_value.status = 200
                for response in (b"x" * 131073, b"{bad", b'{"body":null}'):
                    connection.getresponse.return_value.read.return_value = response
                    with self.subTest(response=response[:20]):
                        with self.assertRaises(NotesError):
                            release_body("v0.2.0")


if __name__ == "__main__":
    unittest.main()
