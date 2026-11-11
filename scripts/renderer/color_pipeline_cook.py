#!/usr/bin/env python3
"""Produce pinned analytic ACES 2 display-linear HLSL and its original 2D tables.

Run with the explicitly provisioned interpreter. This offline tool performs no
network access, tool discovery, trust activation, shader compilation, or LUT bake.
Its hashes describe provenance/integrity; they do not establish cook trust or GPU
qualification. The initial closed profile is ACEScg -> ACES 2 SDR100 Rec709 ->
linear Rec709, with no creative look selected.
"""
from __future__ import annotations

import argparse
from collections.abc import Callable
import ctypes
import hashlib
import importlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import zipfile

OCIO_VERSION = "2.5.0"
CONFIG_NAME = "cg-config-v4.0.0_aces-v2.0_ocio-v2.5.ocio"
CONFIG_SHA256 = "9e3ec773a7fbc0bb6666e428dedd8ccec4f59b8b6d40506c825cc7a8c02ff2ff"
PROFILE = "aces2-sdr100-rec709-linear-v1"
FUNCTION_NAME = "HoroColorOutput"
RESOURCE_PREFIX = "horo_color_"
TABLE_NAMES = ("horo_color_reach_m_table_0", "horo_color_gamut_cusp_table_0")
MAX_FILE_BYTES = 32 * 1024 * 1024


class CookError(RuntimeError):
    """A typed producer failure; no output generation is published on failure."""


def require(condition: bool, message: str) -> None:
    """Reject an unsupported or contradictory cold input without a fallback."""
    if not condition:
        raise CookError(message)


def read_bytes(path: Path, maximum: int = MAX_FILE_BYTES) -> bytes:
    """Read one explicit bounded regular file without following a final symlink."""
    require(path.is_absolute() and path.is_file() and not path.is_symlink(),
            f"Expected an absolute regular file: {path}")
    with path.open("rb") as stream:
        data = stream.read(maximum + 1)
    require(len(data) <= maximum, f"Input exceeds its byte limit: {path.name}")
    return data


def sha256(data: bytes) -> str:
    """Hash exact portable payload bytes."""
    return hashlib.sha256(data).hexdigest()


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    """Reject duplicate JSON keys in explicit tool provenance."""
    result = {}
    for key, value in pairs:
        require(key not in result, f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def json_bytes(value: object) -> bytes:
    """Serialize canonical finite JSON independently of locale and machine paths."""
    return (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")


def hash_file(path: Path) -> str:
    """Hash one bounded tool artifact; interpreter symlinks resolve explicitly."""
    return sha256(read_bytes(path.resolve()))


def receipt_code(root: Path, receipt: dict) -> dict[str, str]:
    """Check every explicitly pinned installed OCIO code file inside the tool root."""
    entries = receipt.get("installedCodeHashes", {})
    require(isinstance(entries, dict) and 1 <= len(entries) <= 1024,
            "Tool receipt must pin installed code files")
    result = {}
    for relative, expected in entries.items():
        candidate = (root / relative).resolve()
        require(candidate.is_relative_to(root), "Installed code path escapes tool root")
        actual = hash_file(candidate)
        require(actual == expected, f"Installed tool code digest mismatch: {relative}")
        result[relative] = actual
    return result


def receipt_wheel(receipt: dict, wheel: Path) -> dict:
    """Require the exact selected OCIO wheel digest from the explicit setup receipt."""
    matches = [entry for entry in receipt.get("inputs", [])
               if Path(entry.get("file", "")).name == wheel.name]
    require(len(matches) == 1, "Selected OCIO wheel must appear exactly once in tool receipt")
    data = read_bytes(wheel)
    require(sha256(data) == matches[0].get("sha256"), "OCIO wheel digest mismatch")
    require(len(data) == matches[0].get("bytes"), "OCIO wheel byte count mismatch")
    return {"file": wheel.name, "sha256": sha256(data), "bytes": len(data)}


def load_tool(receipt_path: Path, wheel: Path) -> tuple[object, dict]:
    """Verify the explicit interpreter/module installation before constructing an OCIO processor."""
    encoded = read_bytes(receipt_path, 128 * 1024)
    receipt = json.loads(encoded, object_pairs_hook=unique_object)
    require(receipt.get("version") == OCIO_VERSION, "Expected pinned OCIO 2.5.0 tool receipt")
    interpreter = Path(receipt.get("interpreter", ""))
    require(interpreter.is_absolute(), "Tool receipt must pin an absolute interpreter")
    require(Path(sys.executable).absolute() == interpreter.absolute(), "Invoke the exact receipt interpreter")
    require(hash_file(interpreter) == receipt.get("interpreterSHA256"), "Interpreter digest mismatch")
    root = receipt_path.parent.resolve()
    code = receipt_code(root, receipt)
    spec = importlib.util.find_spec("PyOpenColorIO")
    require(spec is not None and spec.origin is not None, "Pinned PyOpenColorIO is unavailable")
    origin = Path(spec.origin).resolve()
    require(origin.is_relative_to(root), "PyOpenColorIO import escapes pinned tool root")
    require(origin.relative_to(root).as_posix() in code, "Imported OCIO module is absent from pinned code receipt")
    module = importlib.import_module("PyOpenColorIO")
    require(module.__version__ == OCIO_VERSION, "Loaded OCIO version differs from pinned tool")
    provenance = {"version": module.__version__, "receiptSHA256": sha256(encoded),
                  "interpreterSHA256": receipt["interpreterSHA256"], "installedCodeHashes": code,
                  "wheel": receipt_wheel(receipt, wheel)}
    return module, provenance


def wheel_code(wheel: Path, module: object, extension: object) -> dict[str, str]:
    """Match the actual imported array bridge code against the explicit pinned wheel contents."""
    root = Path(module.__file__).parent
    result = {}
    with zipfile.ZipFile(wheel) as archive:
        for path in [Path(module.__file__), Path(extension.__file__)]:
            relative = "numpy/" + path.relative_to(root).as_posix()
            member = archive.getinfo(relative)
            require(member.file_size <= MAX_FILE_BYTES, "Array bridge code member exceeds limit")
            expected = sha256(archive.read(member))
            require(hash_file(path) == expected, "Imported NumPy code differs from pinned wheel")
            result[relative] = expected
    return result


def load_array_tool(wheel: Path, expected_sha256: str, tool_root: Path) -> tuple[object, dict]:
    """Load the explicitly provisioned NumPy array bridge and pin its wheel and actual code origin."""
    data = read_bytes(wheel)
    require(sha256(data) == expected_sha256, "NumPy wheel digest mismatch")
    spec = importlib.util.find_spec("numpy")
    require(spec is not None and spec.origin is not None, "Provision pinned NumPy in the isolated tool environment")
    origin = Path(spec.origin).resolve()
    require(origin.is_relative_to(tool_root), "NumPy import escapes the explicit tool root")
    with zipfile.ZipFile(wheel) as archive:
        require(sha256(archive.read("numpy/__init__.py")) == hash_file(origin),
                "Imported NumPy module differs from selected wheel")
    module = importlib.import_module("numpy")
    extension = importlib.import_module("numpy._core._multiarray_umath")
    provenance = {"version": module.__version__, "wheel": {"file": wheel.name,
                  "sha256": sha256(data), "bytes": len(data)},
                  "installedCodeHashes": wheel_code(wheel, module, extension)}
    return module, provenance


def options() -> dict:
    """Describe the complete closed color route, including the exact display-transfer removal."""
    return {"profile": PROFILE, "input": "ACEScg", "display": "sRGB - Display",
            "view": "ACES 2.0 - SDR 100 nits (Rec.709)", "creativeLook": "none-selected",
            "output": {"primaries": "Rec709", "whitePoint": "D65", "encoding": "linear",
                       "referenceWhiteNits": 100, "peakNits": 100, "blackNits": 0},
            "displayTransferRemoval": {"type": "ExponentWithLinearTransform", "direction": "forward",
                       "gamma": [2.4, 2.4, 2.4, 1.0], "offset": [0.055, 0.055, 0.055, 0.0],
                       "negativeStyle": "mirror"},
            "cpuOptimization": "none", "gpuOptimization": "lossless", "language": "HLSL_DX11",
            "functionName": FUNCTION_NAME, "resourcePrefix": RESOURCE_PREFIX,
            "allowTexture1D": False, "maximumTextureWidth": 4096,
            "domainPolicy": "original-official-analytic-operations-no-added-range-or-shaper"}


def processor(ocio: object, config_path: Path) -> tuple[object, object, dict]:
    """Compose the exact official display/view with only its pinned mirror-sRGB inverse transfer."""
    data = read_bytes(config_path, 128 * 1024)
    require(config_path.name == CONFIG_NAME and sha256(data) == CONFIG_SHA256,
            "Expected exact CG4.0.0 ACES2.0/OCIO2.5 configuration bytes")
    config = ocio.Config.CreateFromFile(str(config_path))
    config.validate()
    view = ocio.DisplayViewTransform(src="ACEScg", display="sRGB - Display",
                                    view="ACES 2.0 - SDR 100 nits (Rec.709)")
    decode = ocio.ExponentWithLinearTransform(gamma=[2.4, 2.4, 2.4, 1], offset=[.055, .055, .055, 0],
                                            negativeStyle=ocio.NEGATIVE_MIRROR)
    full = config.getProcessor(ocio.GroupTransform([view, decode]))
    cpu = full.getOptimizedCPUProcessor(ocio.OPTIMIZATION_NONE)
    shader = ocio.GpuShaderDesc.CreateShaderDesc()
    shader.setLanguage(ocio.GPU_LANGUAGE_HLSL_DX11)
    shader.setFunctionName(FUNCTION_NAME)
    shader.setResourcePrefix(RESOURCE_PREFIX)
    shader.setAllowTexture1D(False)
    shader.setTextureMaxWidth(4096)
    full.getOptimizedGPUProcessor(ocio.OPTIMIZATION_LOSSLESS).extractGpuShaderInfo(shader)
    identity = {"config": {"file": config_path.name, "sha256": sha256(data), "bytes": len(data)},
                "processorCacheID": full.getCacheID(), "cpuCacheID": cpu.getCacheID(),
                "gpuCacheID": shader.getCacheID()}
    return cpu, shader, identity


def float32(value: float) -> float:
    """Round each reference channel to the actual finite float32 input contract."""
    result = struct.unpack("<f", struct.pack("<f", value))[0]
    require(math.isfinite(result), "Reference input or result is nonfinite")
    return result


def reference_inputs() -> list[tuple[str, list[float]]]:
    """Generate independent non-lattice black, negative, HDR, gamut and official-range boundary cases."""
    cases = [("black", [0.0, 0.0, 0.0]), ("negative-zero", [-0.0, 0.0, -0.0])]
    levels = [-65504.0, -1.0, -0.001, 1e-8, 1e-5, 0.0078125, .18, .5, 1.0,
              16.0, 128.0, 1024.0, 65504.0, 65504.0 * 2 ** 32, -65504.0 * 2 ** 32]
    cases.extend((f"gray-{i}", [v, v, v]) for i, v in enumerate(levels))
    colors = [[.18, -.05, .4], [4, .01, -.1], [-.2, 12, .003], [65504, 0, 0],
              [0, 65504, 0], [0, 0, 65504], [.0031, .0167, 3.14159]]
    cases.extend((f"mixed-{i}", value) for i, value in enumerate(colors))
    # These exact half-float inputs let native FP16 tests use the same CPU inputs,
    # without comparing rounded GPU sources with unrounded reference colors.
    half_colors = [[4.0, .0078125, -.125], [-.25, 12.0, .00390625],
                   [.03125, .125, 2.0], [1.0, .25, .0625]]
    cases.extend((f"half-mixed-{i}", value) for i, value in enumerate(half_colors))
    for boundary in [0.0, .0078125, 1.0, 1024.0]:
        for delta in [-2 ** -16, 0.0, 2 ** -16]:
            cases.append((f"boundary-{boundary}-{delta}", [boundary + delta, .18, .071]))
    return cases


def reference_corpus(cpu: object) -> dict:
    """Evaluate independent direct CPU references rather than sampling generated GPU tables."""
    cases = []
    for name, rgb in reference_inputs():
        source = [float32(value) for value in rgb]
        result = [float32(value) for value in cpu.applyRGB(list(source))]
        require(len(result) == 3, "OCIO CPU reference returned a foreign channel shape")
        cases.append({"name": name, "input": source, "displayLinearRgb": result})
    return {"schemaVersion": 1, "profile": PROFILE, "processor": "direct-OCIO-CPU-optimization-none",
            "scope": "reference-values-only-no-GPU-execution-or-conformance-claim", "cases": cases}


def texture_shape(ocio: object, texture: object, index: int) -> int:
    """Require exact current analytic ACES2 auxiliary textures, without admitting a baked 3D alternative."""
    require(texture.textureName == TABLE_NAMES[index], "Unexpected generated texture name")
    require(texture.samplerName == texture.textureName + "Sampler", "Unexpected generated sampler name")
    require(texture.dimensions == ocio.GpuShaderDesc.TEXTURE_2D, "Generated texture is not 2D")
    require(texture.width == 362 and texture.height == 1, "Unexpected ACES2 auxiliary table dimensions")
    channels = 1 if index == 0 else 3
    expected = ocio.GpuShaderDesc.TEXTURE_RED_CHANNEL if channels == 1 else ocio.GpuShaderDesc.TEXTURE_RGB_CHANNEL
    require(texture.channel == expected, "Unexpected generated texture channel layout")
    require(texture.interpolation == ocio.INTERP_NEAREST, "Unexpected generated auxiliary sampler interpolation")
    return channels


def texture_payload(texture: object, channels: int) -> tuple[bytes, bytes]:
    """Preserve exact source float32 values and expand only RGB to lossless RGBA upload storage."""
    array = texture.getValues()
    require(array.dtype.kind == "f" and array.dtype.itemsize == 4, "OCIO auxiliary samples must be float32")
    values = [float32(value) for value in array.reshape(-1).tolist()]
    require(len(values) == texture.width * texture.height * channels, "Auxiliary texture value count differs from descriptor")
    original = struct.pack(f"<{len(values)}f", *values)
    if channels == 1:
        return original, original
    rgba = []
    for offset in range(0, len(values), 3):
        rgba.extend([*values[offset:offset + 3], 1.0])
    return original, struct.pack(f"<{len(rgba)}f", *rgba)


def write_file(root: Path, name: str, data: bytes) -> dict:
    """Create and flush one owned staging artifact, returning its portable exact digest."""
    require(0 < len(data) <= MAX_FILE_BYTES, f"Artifact size is invalid: {name}")
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    return {"file": name, "bytes": len(data), "sha256": sha256(data)}


def emit_texture(ocio: object, root: Path, texture: object, index: int) -> dict:
    """Publish original samples and exact runtime upload data with an explicit padding policy."""
    channels = texture_shape(ocio, texture, index)
    source, upload = texture_payload(texture, channels)
    original = write_file(root, f"textures/{index:03}.source.f32le", source)
    packed = write_file(root, f"textures/{index:03}.upload.f32le", upload)
    return {"ordinal": index, "textureName": texture.textureName, "samplerName": texture.samplerName,
            "dimension": "2D", "width": texture.width, "height": texture.height, "sourceChannels": channels,
            "format": "R32Float" if channels == 1 else "Rgba32Float", "source": original, "upload": packed,
            "padding": "none" if channels == 1 else "RGB-unchanged-alpha-one",
            "sampler": {"filter": "nearest", "addressU": "clamp-to-edge", "addressV": "clamp-to-edge",
                        "mipFilter": "none"}, "registerAssignment": "wrapper-owned-no-register-rewrite"}


def stage_artifacts(root: Path, ocio: object, cpu: object, shader: object, provenance: dict) -> dict:
    """Create a complete bounded generation before any destination publication."""
    textures = list(shader.getTextures())
    require(len(textures) == 2, "Expected exactly two analytic ACES2 auxiliary textures")
    require(len(shader.get3DTextures()) == 0, "3D LUT extraction is outside the closed analytic profile")
    require(len(shader.getUniforms()) == 0, "Dynamic OCIO uniforms are outside the closed profile")
    code = shader.getShaderText().encode("utf-8")
    require(0 < len(code) <= 128 * 1024, "Generated analytic shader exceeds its bound")
    shader_artifact = write_file(root, "processor.hlsl", code)
    tables = [emit_texture(ocio, root, texture, i) for i, texture in enumerate(textures)]
    references = write_file(root, "reference.json", json_bytes(reference_corpus(cpu)))
    route = options()
    manifest = {"schemaVersion": 1, "kind": "horo-analytic-color-processor", "profile": PROFILE,
                "options": route, "optionsSHA256": sha256(json_bytes(route)), "provenance": provenance,
                "shader": shader_artifact, "textures": tables, "referenceCorpus": references,
                "qualification": "provenance-and-integrity-only-no-cook-trust-native-or-ACES-conformance"}
    write_file(root, "manifest.json", json_bytes(manifest))
    return manifest


def rename_exclusive(source: Path, destination: Path) -> None:
    """Atomically publish a new directory without replacing even an empty racing destination."""
    if sys.platform == "win32":
        os.rename(source, destination)
        return
    library = ctypes.CDLL(None, use_errno=True)
    if sys.platform == "darwin":
        function = library.renamex_np
        function.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
        result = function(os.fsencode(source), os.fsencode(destination), 4)  # RENAME_EXCL
    elif sys.platform.startswith("linux"):
        function = library.renameat2
        function.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        result = function(-100, os.fsencode(source), -100, os.fsencode(destination), 1)  # RENAME_NOREPLACE
    else:
        raise CookError("Exclusive atomic directory publication is unsupported on this host")
    if result != 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), str(destination))


def publish(destination: Path, writer: Callable[[Path], object]) -> object:
    """Stage in the exact destination parent and clean only this invocation's unpublished directory."""
    require(destination.is_absolute(), "Output directory must be absolute")
    require(destination.parent.is_dir() and not destination.parent.is_symlink(), "Output parent must already exist")
    require(not os.path.lexists(destination), "Output destination already exists")
    stage = Path(tempfile.mkdtemp(prefix=f".{destination.name}.stage-", dir=destination.parent))
    try:
        result = writer(stage)
        rename_exclusive(stage, destination)
        return result
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def arguments(argv: list[str] | None = None) -> argparse.Namespace:
    """Require explicit pinned files and a fresh absolute output generation."""
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["config", "tool-receipt", "wheel", "numpy-wheel", "output"]:
        parser.add_argument(f"--{name}", required=True, type=Path)
    parser.add_argument("--numpy-wheel-sha256", required=True)
    return parser.parse_args(argv)


def run(args: argparse.Namespace) -> dict:
    """Validate explicit tool inputs, then publish a complete immutable analytic processor generation."""
    ocio, tool = load_tool(args.tool_receipt, args.wheel)
    _, array_tool = load_array_tool(args.numpy_wheel, args.numpy_wheel_sha256, args.tool_receipt.parent.resolve())
    cpu, shader, identity = processor(ocio, args.config)
    provenance = {"ocio": tool, "arrayBridge": array_tool, **identity}
    return publish(args.output, lambda stage: stage_artifacts(stage, ocio, cpu, shader, provenance))


def main(argv: list[str] | None = None) -> int:
    """Report publication only after exclusive rename succeeds; never label metadata as qualification."""
    try:
        result = run(arguments(argv))
    except Exception as error:  # CLI boundary: a failed generation never becomes a success receipt.
        print(f"color_pipeline_cook.failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps({"published": True, "profile": result["profile"],
                      "scope": "offline-analytic-source-and-reference-generation"}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
