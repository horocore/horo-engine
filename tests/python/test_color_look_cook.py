"""Cold input/storage contracts only; real OCIO and GPU results have separate receipts."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "color_look_cook", Path(__file__).resolve().parents[2] / "scripts/renderer/color_look_cook.py")
LOOK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LOOK)


def source():
    return {"dimension": 2, "domainMinimum": [0, 0, 0], "domainMaximum": [1, 1, 1],
            "interpolation": "Trilinear", "encoding": "LinearAcesCgV1", "layout": "RedFastest",
            "rgb": [[r, g, b] for b in range(2) for g in range(2) for r in range(2)]}


def model(value):
    return LOOK.source_model(json.dumps(value).encode())


class ColorLookInputTests(unittest.TestCase):
    def test_layout_is_explicit_and_exact_red_fastest_little_endian(self):
        data = LOOK.payload(model(source()))
        self.assertEqual(len(data), 8 * 3 * 4)
        self.assertEqual(struct.unpack_from("<3f", data, 12), (1, 0, 0))
        self.assertEqual(struct.unpack_from("<3f", data, 24), (0, 1, 0))
        self.assertEqual(struct.unpack_from("<3f", data, 48), (0, 0, 1))

    def test_encoding_and_order_are_never_guessed(self):
        for field, value in (("encoding", "sRGB"), ("layout", "BlueFastest")):
            item = source()
            item[field] = value
            with self.subTest(field=field), self.assertRaises(LOOK.COOK.CookError):
                model(item)

    def test_signed_hdr_values_and_negative_zero_payload_are_preserved(self):
        item = source()
        item["rgb"][0] = [-0.0, -.125, 1024.0]
        data = LOOK.payload(model(item))
        self.assertEqual(data[:12], struct.pack("<3f", -0.0, -.125, 1024.0))

    def test_endpoint_probes_preserve_actual_declared_float32_bounds(self):
        item = source()
        item["domainMinimum"] = [-0.0, -16, -2]
        item["domainMaximum"] = [8, 16, 4]
        value = model(item)
        probes = LOOK.probe_inputs(value)
        self.assertEqual(len(probes), 13)
        self.assertEqual(struct.pack("<f", probes[0][0]), struct.pack("<f", -0.0))
        self.assertEqual(probes[7], value["domainMaximum"])
        for point in probes:
            self.assertTrue(all(low <= v <= high for v, low, high in
                                zip(point, value["domainMinimum"], value["domainMaximum"])))

    def test_unknown_missing_and_duplicate_fields_are_rejected(self):
        item = source()
        item["extra"] = 1
        with self.assertRaises(LOOK.COOK.CookError):
            model(item)
        del item["extra"]
        del item["layout"]
        with self.assertRaises(LOOK.COOK.CookError):
            model(item)
        with self.assertRaises(LOOK.COOK.CookError):
            LOOK.source_model(b'{"dimension":2,"dimension":3}')

    def test_dimension_count_and_interpolation_vocabulary_are_closed(self):
        for field, value in (("dimension", True), ("dimension", 1), ("dimension", 66),
                             ("rgb", []), ("interpolation", "Nearest")):
            item = source()
            item[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(LOOK.COOK.CookError):
                model(item)
        item = source()
        item["interpolation"] = "Tetrahedral"
        self.assertEqual(model(item)["interpolation"], "Tetrahedral")

    def test_nonfinite_samples_and_degenerate_domains_are_rejected(self):
        for bad in (float("nan"), float("inf")):
            item = source()
            item["rgb"][0][0] = bad
            with self.subTest(value=bad), self.assertRaises(LOOK.COOK.CookError):
                model(item)
        item = source()
        item["domainMaximum"][1] = 0
        with self.assertRaises(LOOK.COOK.CookError):
            model(item)

    def test_three_file_generation_binds_actual_bytes_without_replacing_destination(self):
        value = model(source())
        reference = {"schemaVersion": 1, "scope": "synthetic-codec-only-no-OCIO-or-native-claim"}
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "look"
            manifest = LOOK.COOK.publish(destination, lambda stage: LOOK.write_generation(stage, value, reference))
            before = {p.name: p.read_bytes() for p in destination.iterdir()}
            self.assertEqual(set(before), {"manifest.json", "look.rgb.f32le", "reference.json"})
            self.assertEqual(manifest["payload"]["sha256"], LOOK.COOK.sha256(before["look.rgb.f32le"]))
            self.assertEqual(manifest["reference"]["bytes"], len(before["reference.json"]))
            with self.assertRaises(LOOK.COOK.CookError):
                LOOK.COOK.publish(destination, lambda stage: LOOK.write_generation(stage, value, reference))
            self.assertEqual({p.name: p.read_bytes() for p in destination.iterdir()}, before)



class ColorLookPublicationBoundaryTests(unittest.TestCase):
    def arguments(self, root):
        values = []
        for name in ("source", "config", "tool-receipt", "wheel", "numpy-wheel", "output"):
            values.extend(("--" + name, str(root / name)))
        return values + ["--numpy-wheel-sha256", "1" * 64]

    def test_cli_requires_explicit_paths_and_preserves_unicode_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "look 空間 🧪"
            args = LOOK.arguments(self.arguments(root))
            for name in ("source", "config", "tool_receipt", "wheel", "numpy_wheel", "output"):
                self.assertEqual(getattr(args, name), root / name.replace("_", "-"))
            self.assertEqual(args.numpy_wheel_sha256, "1" * 64)
            self.assertFalse(root.exists())
        with patch("sys.stderr", new=io.StringIO()), self.assertRaises(SystemExit) as error:
            LOOK.arguments([])
        self.assertEqual(error.exception.code, 2)

    def test_invalid_source_is_rejected_before_tool_activation_and_publication(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            invalid = source()
            invalid["encoding"] = "sRGB"
            (root / "source").write_text(json.dumps(invalid), encoding="utf-8")
            with patch.object(LOOK.COOK, "load_tool") as tool:
                with self.assertRaisesRegex(LOOK.COOK.CookError, "Explicit linear AP1"):
                    LOOK.run(LOOK.arguments(self.arguments(root)))
                tool.assert_not_called()
            self.assertFalse((root / "output").exists())

    def test_missing_tool_receipt_reports_failure_without_constructing_processor(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "source").write_text(json.dumps(source()), encoding="utf-8")
            stdout = io.StringIO()
            stderr = io.StringIO()
            with patch.object(LOOK.COOK, "processor") as processor:
                with patch("sys.stdout", stdout), patch("sys.stderr", stderr):
                    result = LOOK.main(self.arguments(root))
                processor.assert_not_called()
            self.assertEqual(result, 1)
            self.assertEqual(stdout.getvalue(), "")
            self.assertIn("absolute regular file", stderr.getvalue())
            self.assertFalse((root / "output").exists())

    def test_cli_bounds_failure_text_without_emitting_a_success_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            message = "synthetic publication failure " * 200
            stdout = io.StringIO()
            stderr = io.StringIO()
            with patch.object(LOOK, "run", side_effect=LOOK.COOK.CookError(message)):
                with patch("sys.stdout", stdout), patch("sys.stderr", stderr):
                    result = LOOK.main(self.arguments(Path(directory)))
            self.assertEqual(result, 1)
            self.assertEqual(stdout.getvalue(), "")
            self.assertEqual(stderr.getvalue(), message[:2048] + "\n")

    def test_source_digest_binds_original_encoding_while_payload_binds_float_values(self):
        compact = json.dumps(source(), separators=(",", ":")).encode()
        spaced = json.dumps(source(), indent=2).encode()
        first = LOOK.source_model(compact)
        second = LOOK.source_model(spaced)
        self.assertEqual(first["sourceSHA256"], hashlib.sha256(compact).hexdigest())
        self.assertEqual(second["sourceSHA256"], hashlib.sha256(spaced).hexdigest())
        self.assertNotEqual(first["sourceSHA256"], second["sourceSHA256"])
        self.assertEqual(LOOK.payload(first), LOOK.payload(second))

    def test_manifest_binds_reference_hash_and_descriptor_to_exact_staged_bytes(self):
        value = model(source())
        reference = {"schemaVersion": 1, "scope": "synthetic-codec-integrity", "label": "空間", "sample": -0.0}
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "look"
            manifest = LOOK.COOK.publish(destination, lambda stage: LOOK.write_generation(stage, value, reference))
            encoded = (destination / "reference.json").read_bytes()
            self.assertEqual(json.loads(encoded), reference)
            self.assertEqual(manifest["reference"]["sha256"], hashlib.sha256(encoded).hexdigest())
            self.assertEqual(manifest["reference"]["bytes"], len(encoded))
            self.assertEqual(manifest["descriptor"]["configurationSHA256"], LOOK.COOK.CONFIG_SHA256)
            self.assertEqual(manifest["descriptor"]["domainMinimum"], value["domainMinimum"])
            self.assertEqual(manifest["descriptor"]["domainMaximum"], value["domainMaximum"])
            self.assertEqual(manifest["payload"]["bytes"], len(LOOK.payload(value)))
            self.assertEqual(json.loads((destination / "manifest.json").read_bytes()), manifest)

    def test_invalid_reference_never_publishes_or_leaves_a_private_stage(self):
        for reference in ({"oversized": "x" * (128 * 1024)}, {"nonfinite": float("inf")}):
            with self.subTest(reference=list(reference)), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                with self.assertRaises((LOOK.COOK.CookError, ValueError)):
                    LOOK.COOK.publish(root / "look", lambda stage: LOOK.write_generation(stage, model(source()), reference))
                self.assertEqual(list(root.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
