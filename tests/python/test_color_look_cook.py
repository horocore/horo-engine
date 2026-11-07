"""Cold input/storage contracts only; real OCIO and GPU results have separate receipts."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

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


if __name__ == "__main__":
    unittest.main()
