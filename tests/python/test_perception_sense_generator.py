"""Native sense code generation, executable with the standard-library unittest runner."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("horo_perception_generator_tests", ROOT / "scripts/project.py")
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("gameplay descriptor generator is unavailable")
GENERATOR = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GENERATOR
SPEC.loader.exec_module(GENERATOR)


class PerceptionSenseGeneratorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="horo-sense-generator-")
        self.root = Path(self.directory.name)
        self.source = self.root / "Custom Senses Ş.cpp"
        self.output = self.root / "bundle.cpp"
        self.revision = self.root / "bundle.revision"

    def tearDown(self) -> None:
        self.assertEqual(self.root.resolve().parent, Path(tempfile.gettempdir()).resolve())
        self.assertTrue(self.root.name.startswith("horo-sense-generator-"))
        self.directory.cleanup()

    def generate(self, source: str) -> str:
        self.source.write_text(source, encoding="utf-8")
        GENERATOR.generate_bundle(self.output, self.revision, self.root, "game.sense.tests", "sense-fingerprint", [self.source])
        return self.output.read_text(encoding="utf-8")

    def test_native_senses_share_the_existing_bundle_and_stable_numeric_order(self) -> None:
        text = self.generate('HORO_AI_SENSE(Later, "99")\nHORO_AI_SENSE(First, "6")\nHORO_AI_NODE(Node, "2")\n')
        self.assertIn(".perceptionSenseCount = 2", text)
        self.assertIn(".perceptionSenses = PerceptionSenses().data()", text)
        self.assertIn(".decisionNodeCount = 1", text)
        self.assertIn("HoroGeneratedPerceptionSense_First() noexcept;", text)
        self.assertLess(text.index("HoroGeneratedPerceptionSense_First()"), text.index("HoroGeneratedPerceptionSense_Later()"))
        self.assertNotIn("new First", text)

    def test_invalid_or_reserved_sense_identities_publish_no_bundle(self) -> None:
        for identity in ("0", "1", "5", "06", "-7", "18446744073709551616", "game.sense"):
            with self.subTest(identity=identity):
                with self.assertRaisesRegex(ValueError, "perception sense identity"):
                    self.generate(f'HORO_AI_SENSE(Sense, "{identity}")\n')
                self.assertFalse(self.output.exists())
                self.assertFalse(self.revision.exists())

    def test_duplicate_and_oversized_sense_tables_publish_no_bundle(self) -> None:
        sources = (
            'HORO_AI_SENSE(First, "6")\nHORO_AI_SENSE(Second, "6")\n',
            'HORO_AI_SENSE(Sense, "6")\nHORO_AI_SENSE(Sense, "7")\n',
            "\n".join(f'HORO_AI_SENSE(Sense{index}, "{index + 6}")' for index in range(65)),
        )
        for source in sources:
            with self.subTest(source=source[:80]):
                with self.assertRaisesRegex(ValueError, "perception sense"):
                    self.generate(source)
                self.assertFalse(self.output.exists())

    def test_failed_regeneration_preserves_the_previous_complete_publication(self) -> None:
        self.generate('HORO_AI_SENSE(Sense, "6")\n')
        before = self.output.read_bytes(), self.revision.read_bytes()
        with self.assertRaisesRegex(ValueError, "perception sense identity"):
            self.generate('HORO_AI_SENSE(Sense, "0")\n')
        self.assertEqual(before, (self.output.read_bytes(), self.revision.read_bytes()))

    def test_modules_without_custom_senses_emit_an_empty_explicit_table(self) -> None:
        text = self.generate('HORO_AI_NODE(Node, "2")\n')
        self.assertIn(".perceptionSenseCount = 0", text)
        self.assertIn(".perceptionSenses = nullptr", text)
        self.assertIn(".decisionNodeCount = 1", text)


if __name__ == "__main__":
    unittest.main()
