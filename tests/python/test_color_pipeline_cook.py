"""Cold serialization/publication regressions; these do not qualify ACES or GPU behavior."""
from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "renderer" / "color_pipeline_cook.py"
SPEC = importlib.util.spec_from_file_location("horo_color_pipeline_cook", SCRIPT)
COOK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COOK)


class TextureSamplesFixture:
    """Expose only the array operations used by the cold binary serializer."""

    def __init__(self, values, *, kind="f", itemsize=4):
        self.values = values
        self.dtype = types.SimpleNamespace(kind=kind, itemsize=itemsize)

    def reshape(self, _shape):
        return self

    def tolist(self):
        return list(self.values)


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
            try:
                destination.symlink_to(Path(directory) / "absent", target_is_directory=True)
            except OSError as error:
                if getattr(error, "winerror", None) == 1314:
                    self.skipTest("Creating symlinks requires a Windows privilege")
                raise
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

    def test_explicit_input_must_be_absolute_regular_and_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "input.bin"
            source.write_bytes(b"bounded")
            self.assertEqual(COOK.read_bytes(source, 7), b"bounded")
            for path, maximum in [(Path("relative.bin"), 7), (root, 7), (source, 6)]:
                with self.subTest(path=path, maximum=maximum), self.assertRaises(COOK.CookError):
                    COOK.read_bytes(path, maximum)

    def test_installed_code_receipt_rejects_escape_and_content_tampering(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "tool"
            root.mkdir()
            module = root / "module.py"
            module.write_bytes(b"original installed code")
            expected = COOK.sha256(module.read_bytes())
            receipt = {"installedCodeHashes": {module.name: expected}}
            self.assertEqual(COOK.receipt_code(root, receipt), {module.name: expected})
            module.write_bytes(b"tampered installed code")
            with self.assertRaisesRegex(COOK.CookError, "digest mismatch"):
                COOK.receipt_code(root, receipt)
            foreign = root.with_name("tool-foreign")
            foreign.mkdir()
            for relative in ["../tool-foreign/module.py", str(foreign / "module.py")]:
                with self.subTest(relative=relative), self.assertRaisesRegex(COOK.CookError, "escapes tool root"):
                    COOK.receipt_code(root, {"installedCodeHashes": {relative: expected}})
            with self.assertRaisesRegex(COOK.CookError, "must pin installed code"):
                COOK.receipt_code(root, {})

    def test_selected_wheel_requires_unique_matching_hash_and_byte_count(self):
        with tempfile.TemporaryDirectory() as directory:
            wheel = Path(directory) / "synthetic-unit-fixture.whl"
            wheel.write_bytes(b"synthetic wheel identity fixture")
            record = {"file": wheel.name, "sha256": COOK.sha256(wheel.read_bytes()), "bytes": wheel.stat().st_size}
            self.assertEqual(COOK.receipt_wheel({"inputs": [record]}, wheel), record)
            for records in [[], [record, record], [{**record, "sha256": "wrong"}], [{**record, "bytes": 1}]]:
                with self.subTest(records=records), self.assertRaises(COOK.CookError):
                    COOK.receipt_wheel({"inputs": records}, wheel)
            with self.assertRaisesRegex(COOK.CookError, "NumPy wheel digest mismatch"):
                COOK.load_array_tool(wheel, "wrong", Path(directory))

    def test_tool_receipt_rejects_foreign_interpreter_before_import(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt_path = root / "receipt.json"
            receipt = {"version": COOK.OCIO_VERSION, "interpreter": sys.executable,
                       "interpreterSHA256": COOK.hash_file(Path(sys.executable))}
            cases = [({**receipt, "version": "foreign"}, "Expected pinned OCIO"),
                     ({**receipt, "interpreter": "relative-python"}, "absolute interpreter"),
                     ({**receipt, "interpreter": str(root / "foreign-python")}, "exact receipt interpreter"),
                     ({**receipt, "interpreterSHA256": "wrong"}, "Interpreter digest mismatch"),
                     (receipt, "must pin installed code")]
            for value, reason in cases:
                receipt_path.write_bytes(COOK.json_bytes(value))
                with self.subTest(reason=reason), self.assertRaisesRegex(COOK.CookError, reason):
                    COOK.load_tool(receipt_path, root / "absent.whl")

    def test_ocio_module_origin_must_be_inside_tool_root_and_pinned_before_import(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "tool"
            root.mkdir()
            pinned = root / "pinned.py"
            pinned.write_bytes(b"synthetic pinned code fixture")
            unpinned = root / "unpinned.py"
            unpinned.write_bytes(b"synthetic unpinned code fixture")
            receipt_path = root / "receipt.json"
            receipt_path.write_bytes(COOK.json_bytes({
                "version": COOK.OCIO_VERSION, "interpreter": sys.executable,
                "interpreterSHA256": COOK.hash_file(Path(sys.executable)),
                "installedCodeHashes": {pinned.name: COOK.hash_file(pinned)}}))
            for origin, reason in [(str(root.parent / "foreign.py"), "escapes pinned tool root"),
                                   (str(unpinned), "absent from pinned code receipt")]:
                spec = importlib.util.spec_from_file_location("PyOpenColorIO", origin)
                with (
                    self.subTest(origin=origin),
                    patch.object(COOK.importlib.util, "find_spec", return_value=spec),
                    patch.object(COOK.importlib, "import_module") as load,
                ):
                    with self.assertRaisesRegex(COOK.CookError, reason):
                        COOK.load_tool(receipt_path, root / "absent.whl")
                    load.assert_not_called()

    def test_failed_cli_input_never_publishes_a_success_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt = root / "receipt.json"
            receipt.write_bytes(b"not JSON")
            destination = root / "output"
            arguments = ["--config", str(root / COOK.CONFIG_NAME), "--tool-receipt", str(receipt),
                         "--wheel", str(root / "ocio.whl"), "--numpy-wheel", str(root / "numpy.whl"),
                         "--numpy-wheel-sha256", "wrong", "--output", str(destination)]
            output, errors = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
                self.assertEqual(COOK.main(arguments), 1)
            self.assertEqual(output.getvalue(), "")
            self.assertIn("color_pipeline_cook.failed", errors.getvalue())
            self.assertFalse(destination.exists())

    def test_payload_creation_never_overwrites_an_owned_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            COOK.write_file(root, "payload.bin", b"first")
            with self.assertRaises(FileExistsError):
                COOK.write_file(root, "payload.bin", b"second")
            self.assertEqual((root / "payload.bin").read_bytes(), b"first")


class ColorCookTextureContractTests(unittest.TestCase):
    """Verify serialized artifacts and rejected descriptors without running an OCIO processor."""

    def setUp(self):
        self.ocio = types.SimpleNamespace(
            GpuShaderDesc=types.SimpleNamespace(TEXTURE_2D=2, TEXTURE_RED_CHANNEL=1, TEXTURE_RGB_CHANNEL=3),
            INTERP_NEAREST=0)

    def texture(self, index):
        return types.SimpleNamespace(textureName=COOK.TABLE_NAMES[index],
                                     samplerName=COOK.TABLE_NAMES[index] + "Sampler", dimensions=2,
                                     width=362, height=1, channel=1 if index == 0 else 3, interpolation=0)

    def test_auxiliary_descriptor_rejects_foreign_layout_and_sampling(self):
        for index, channels in [(0, 1), (1, 3)]:
            self.assertEqual(COOK.texture_shape(self.ocio, self.texture(index), index), channels)
        for field, value in [("textureName", "foreign"), ("samplerName", "foreign"), ("dimensions", 3),
                             ("width", 361), ("height", 2), ("channel", 4), ("interpolation", 1)]:
            texture = self.texture(0)
            setattr(texture, field, value)
            with self.subTest(field=field), self.assertRaises(COOK.CookError):
                COOK.texture_shape(self.ocio, texture, 0)

    def test_float_payload_preserves_signed_hdr_channels_and_pads_only_alpha(self):
        texture = types.SimpleNamespace(width=2, height=1)
        texture.getValues = lambda: TextureSamplesFixture([-0.0, .5, -1.0, 4.0, 1024.0, 65504.0])
        source, upload = COOK.texture_payload(texture, 3)
        self.assertEqual(source, struct.pack("<6f", -0.0, .5, -1.0, 4.0, 1024.0, 65504.0))
        self.assertEqual(upload, struct.pack("<8f", -0.0, .5, -1.0, 1.0, 4.0, 1024.0, 65504.0, 1.0))
        texture.getValues = lambda: TextureSamplesFixture([-0.0, 1024.0])
        source, upload = COOK.texture_payload(texture, 1)
        self.assertEqual(source, struct.pack("<2f", -0.0, 1024.0))
        self.assertEqual(upload, source)

    def test_payload_rejects_wrong_precision_shape_and_nonfinite_samples(self):
        invalid = [TextureSamplesFixture([1.0], itemsize=8), TextureSamplesFixture([1], kind="i"),
                   TextureSamplesFixture([]), TextureSamplesFixture([float("nan")]),
                   TextureSamplesFixture([float("inf")])]
        texture = types.SimpleNamespace(width=1, height=1)
        for samples in invalid:
            texture.getValues = lambda: samples
            with self.subTest(samples=samples.values), self.assertRaises(COOK.CookError):
                COOK.texture_payload(texture, 1)

    def test_emitted_rgb_table_digest_matches_exact_padded_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            texture = self.texture(1)
            texture.getValues = lambda: TextureSamplesFixture([.25, .5, .75] * 362)
            record = COOK.emit_texture(self.ocio, root, texture, 1)
            source = struct.pack("<3f", .25, .5, .75) * 362
            upload = struct.pack("<4f", .25, .5, .75, 1.0) * 362
            self.assertEqual((root / record["source"]["file"]).read_bytes(), source)
            self.assertEqual((root / record["upload"]["file"]).read_bytes(), upload)
            self.assertEqual(record["source"]["sha256"], COOK.sha256(source))
            self.assertEqual(record["upload"]["sha256"], COOK.sha256(upload))
            self.assertEqual(record["format"], "Rgba32Float")
            self.assertEqual(record["padding"], "RGB-unchanged-alpha-one")

    def test_foreign_shader_contract_is_rejected_before_any_artifact_is_written(self):
        cases = [([], [], [], "code"), ([object(), object()], [object()], [], "code"),
                 ([object(), object()], [], [object()], "code"), ([object(), object()], [], [], ""),
                 ([object(), object()], [], [], "x" * (128 * 1024 + 1))]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for textures, volumes, uniforms, code in cases:
                shader = types.SimpleNamespace(getTextures=lambda: textures, get3DTextures=lambda: volumes,
                                               getUniforms=lambda: uniforms, getShaderText=lambda: code)
                with self.subTest(textures=len(textures), bytes=len(code)), self.assertRaises(COOK.CookError):
                    COOK.stage_artifacts(root, self.ocio, object(), shader, {})
                self.assertEqual(list(root.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
