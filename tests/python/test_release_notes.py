"""Release-notes source, selection, snapshot, and Welcome projection contracts."""

import json
import io
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from parse_changelog import (NotesError, make_snapshot, parse_changelog, render_header,
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
            "x" * 262145,
        ):
            with self.subTest(source=source[:40]), self.assertRaises(NotesError):
                parse_changelog(source)
        with self.assertRaises(NotesError):
            select_version(parse_changelog(SOURCE), "0.3.0")
        for invalid in ("1." + "9" * 100000 + ".0", "1.0.0-01", "1.0.0+", "1.0.0-rc..1"):
            with self.subTest(version=invalid[:30]), self.assertRaises(NotesError):
                version_parts(invalid)

    def test_archive_contains_exact_snapshot(self):
        snapshot = snapshot_bytes(make_snapshot(select_version(parse_changelog(SOURCE), "0.2.0"), "horo-editor"))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tar_path = root / "release.tar.gz"
            zip_path = root / "release.zip"
            with tarfile.open(tar_path, "w:gz") as package:
                member = tarfile.TarInfo("HoroEngine/release-notes.json")
                member.size = len(snapshot)
                package.addfile(member, io.BytesIO(snapshot))
            with zipfile.ZipFile(zip_path, "w") as package:
                package.writestr("release-notes.json", snapshot)
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

    def test_release_body_fetch_uses_bounded_authenticated_api_request(self):
        class Response(io.BytesIO):
            def __enter__(self):
                return self

            def __exit__(self, *_):
                self.close()

        with patch.dict("os.environ", {"GITHUB_REPOSITORY": "horocore/horo-engine", "GH_TOKEN": "test-token"}):
            with patch("verify_release_notes.urlopen", return_value=Response(b'{"body":"Reviewed"}')) as fetch:
                self.assertEqual(release_body("v0.2.0"), "Reviewed")
                request = fetch.call_args.args[0]
                self.assertEqual(request.full_url,
                                 "https://api.github.com/repos/horocore/horo-engine/releases/tags/v0.2.0")
                self.assertEqual(fetch.call_args.kwargs["timeout"], 30)


if __name__ == "__main__":
    unittest.main()
