"""Cold serialization/publication regressions; these do not qualify ACES or GPU behavior."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "renderer" / "color_pipeline_cook.py"
SPEC = importlib.util.spec_from_file_location("horo_color_pipeline_cook", SCRIPT)
COOK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COOK)


class ColorCookPublicationTests(unittest.TestCase):
    """Exercise real exclusive directory publication and preservation on failure."""

    def test_complete_generation_is_published_once(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "generation"
            result = COOK.publish(destination, lambda stage: COOK.write_file(stage, "payload.bin", b"original"))
            self.assertEqual((destination / "payload.bin").read_bytes(), b"original")
            self.assertEqual(result["sha256"], COOK.sha256(b"original"))
            self.assertEqual(sorted(path.name for path in Path(directory).iterdir()), ["generation"])

    def test_existing_empty_destination_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "generation"
            destination.mkdir()
            identity = destination.stat().st_ino
            with self.assertRaises(COOK.CookError):
                COOK.publish(destination, lambda stage: self.fail("Writer must not run"))
            self.assertEqual(destination.stat().st_ino, identity)
            self.assertEqual(list(destination.iterdir()), [])

    def test_racing_empty_destination_is_never_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "generation"
            identities = []

            def writer(stage):
                COOK.write_file(stage, "payload.bin", b"staged")
                destination.mkdir()
                identities.append(destination.stat().st_ino)

            with self.assertRaises(OSError):
                COOK.publish(destination, writer)
            self.assertEqual(destination.stat().st_ino, identities[0])
            self.assertEqual(list(destination.iterdir()), [])
            self.assertEqual(sorted(path.name for path in Path(directory).iterdir()), ["generation"])

    def test_failed_writer_removes_only_its_owned_stage(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "generation"
            sibling = Path(directory) / "keep.bin"
            sibling.write_bytes(b"protected")

            def writer(stage):
                COOK.write_file(stage, "payload.bin", b"partial")
                raise COOK.CookError("injected cold producer failure")

            with self.assertRaises(COOK.CookError):
                COOK.publish(destination, writer)
            self.assertFalse(destination.exists())
            self.assertEqual(sibling.read_bytes(), b"protected")
            self.assertEqual(sorted(path.name for path in Path(directory).iterdir()), ["keep.bin"])

    def test_dangling_symlink_destination_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "generation"
            destination.symlink_to(Path(directory) / "absent", target_is_directory=True)
            with self.assertRaises(COOK.CookError):
                COOK.publish(destination, lambda stage: self.fail("Writer must not run"))
            self.assertTrue(destination.is_symlink())


class ColorCookIntegrityTests(unittest.TestCase):
    """Check independent byte/reference contracts without constructing a fake valid processor."""

    def test_json_is_canonical_finite_and_duplicate_free(self):
        self.assertEqual(COOK.json_bytes({"b": 2, "a": 1}), COOK.json_bytes({"a": 1, "b": 2}))
        with self.assertRaises(ValueError):
            COOK.json_bytes({"bad": float("nan")})
        with self.assertRaises(COOK.CookError):
            COOK.unique_object([("a", 1), ("a", 2)])

    def test_reference_corpus_includes_finite_negative_hdr_and_range_boundaries(self):
        values = [channel for _, rgb in COOK.reference_inputs() for channel in rgb]
        self.assertLess(min(values), 0)
        self.assertGreater(max(values), 65504)
        self.assertIn(1024, values)
        for value in values:
            rounded = COOK.float32(value)
            self.assertEqual(struct.pack("<f", rounded), struct.pack("<f", value))
        with self.assertRaises(COOK.CookError):
            COOK.float32(float("inf"))

    def test_native_mixed_reference_inputs_are_exact_half_floats(self):
        cases = [(name, rgb) for name, rgb in COOK.reference_inputs()
                 if name.startswith("half-mixed-")]
        self.assertEqual(len(cases), 4)
        for name, rgb in cases:
            with self.subTest(name=name):
                self.assertGreater(max(rgb), min(rgb))
                for value in rgb:
                    self.assertEqual(struct.unpack("<e", struct.pack("<e", value))[0], value)
        self.assertTrue(any(min(rgb) < 0 < max(rgb) for _, rgb in cases))
        self.assertTrue(any(max(rgb) > 1 for _, rgb in cases))

    def test_bad_config_is_rejected_before_ocio_processor_access(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / COOK.CONFIG_NAME
            config.write_bytes(b"not the pinned configuration")
            with self.assertRaises(COOK.CookError):
                COOK.processor(object(), config)

    def test_payload_creation_never_overwrites_an_owned_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            COOK.write_file(root, "payload.bin", b"first")
            with self.assertRaises(FileExistsError):
                COOK.write_file(root, "payload.bin", b"second")
            self.assertEqual((root / "payload.bin").read_bytes(), b"first")


if __name__ == "__main__":
    unittest.main()
