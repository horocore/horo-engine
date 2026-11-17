#!/usr/bin/env python3
"""Cook explicitly authored linear-AP1 look data with original pinned OCIO CPU references."""
from __future__ import annotations

import argparse
import importlib.util
import itertools
import json
import math
from pathlib import Path
import struct
import sys


def support_module():
    """Load the explicit sibling producer even under the isolated interpreter."""
    path = Path(__file__).with_name("color_pipeline_cook.py")
    spec = importlib.util.spec_from_file_location("color_output_support", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


COOK = support_module()
SOURCE_FIELDS = {"dimension", "domainMinimum", "domainMaximum", "interpolation", "rgb", "encoding", "layout"}


def vector(value: object) -> list[float]:
    """Preserve finite representable float32 RGB values, including signed HDR."""
    COOK.require(isinstance(value, list) and len(value) == 3, "Expected three RGB values")
    COOK.require(all(type(v) in (float, int) for v in value), "RGB values must be numbers")
    COOK.require(all(math.isfinite(v) for v in value), "RGB values must be finite")
    return [COOK.float32(v) for v in value]


def source_model(encoded: bytes) -> dict:
    """Admit only an explicit bounded AP1 cube; no file-extension or color-space guessing."""
    value = json.loads(encoded, object_pairs_hook=COOK.unique_object)
    COOK.require(isinstance(value, dict) and set(value) == SOURCE_FIELDS, "Unknown or missing look source fields")
    COOK.require(value["encoding"] == "LinearAcesCgV1" and value["layout"] == "RedFastest", "Explicit linear AP1/red-fastest input required")
    n = value["dimension"]
    COOK.require(type(n) is int and 2 <= n <= 65, "Cube dimension must be 2..65")
    COOK.require(value["interpolation"] in ("Trilinear", "Tetrahedral"), "Unsupported interpolation")
    low, high = vector(value["domainMinimum"]), vector(value["domainMaximum"])
    width = [COOK.float32(b - a) for a, b in zip(low, high)]
    COOK.require(all(v > 0 for v in width), "Cube domain must have positive finite float32 widths")
    rows = value["rgb"]
    COOK.require(isinstance(rows, list) and len(rows) == n ** 3, "Cube sample count differs from its dimension")
    return {"dimension": n, "domainMinimum": low, "domainMaximum": high, "width": width,
            "interpolation": value["interpolation"], "rgb": [vector(row) for row in rows],
            "sourceSHA256": COOK.sha256(encoded)}


def payload(model: dict) -> bytes:
    """Encode exact red-fastest samples as little-endian IEEE754 RGB without atlas resampling."""
    result = bytearray(len(model["rgb"]) * 12)
    for index, row in enumerate(model["rgb"]):
        struct.pack_into("<3f", result, index * 12, *row)
    return bytes(result)


def look_processor(ocio: object, model: dict) -> object:
    """Build the independent actual OCIO CPU LUT, setting every index explicitly."""
    n = model["dimension"]
    interpolation = ocio.INTERP_LINEAR if model["interpolation"] == "Trilinear" else ocio.INTERP_TETRAHEDRAL
    lut = ocio.Lut3DTransform(gridSize=n, interpolation=interpolation)
    for b, g, r in itertools.product(range(n), repeat=3):
        lut.setValue(r, g, b, *model["rgb"][r + n * g + n * n * b])
    return ocio.Config.CreateRaw().getProcessor(lut).getOptimizedCPUProcessor(ocio.OPTIMIZATION_NONE)


def probe_inputs(model: dict) -> list[list[float]]:
    """Keep exact endpoints and a bounded non-lattice/tie corpus inside the declared domain."""
    fractions = list(itertools.product((0.0, 1.0), repeat=3))
    fractions.extend(((.125, .375, .75), (.5, .5, .25), (.5, .25, .5), (.25, .5, .5), (.5, .5, .5)))
    result = []
    for row in fractions:
        result.append([low if fraction == 0 else high if fraction == 1 else COOK.float32(low + fraction * width)
                       for low, high, width, fraction in zip(model["domainMinimum"], model["domainMaximum"], model["width"], row)])
    return result


def references(ocio: object, model: dict, output_cpu: object, identity: dict, provenance: dict) -> dict:
    """Evaluate direct OCIO LUT then pinned output; never mirror the product shader's interpolation."""
    cpu = look_processor(ocio, model)
    cases = []
    for index, source in enumerate(probe_inputs(model)):
        normalized = [COOK.float32(COOK.float32(v - low) / width)
                      for v, low, width in zip(source, model["domainMinimum"], model["width"])]
        looked = vector(cpu.applyRGB(normalized))
        display = vector(output_cpu.applyRGB(list(looked)))
        cases.append({"name": f"probe-{index}", "input": source, "lookLinearAcesCg": looked, "displayLinearRgb": display})
    return {"schemaVersion": 1, "profile": COOK.PROFILE, "sourceSHA256": model["sourceSHA256"],
            "interpolation": model["interpolation"], "processor": "direct-OCIO-CPU-optimization-none-Lut3D-then-output",
            "scope": "reference-values-only-no-native-execution-or-conformance-claim", "cases": cases,
            "outputProcessor": identity, "tool": {"version": provenance["version"], "receiptSHA256": provenance["receiptSHA256"],
                                                    "wheelSHA256": provenance["wheel"]["sha256"]}}


def write_generation(root: Path, model: dict, reference: dict) -> dict:
    """Publish the one closed three-file data schema; integrity hashes are not publisher trust."""
    data = COOK.write_file(root, "look.rgb.f32le", payload(model))
    refs = COOK.write_file(root, "reference.json", COOK.json_bytes(reference))
    COOK.require(refs["bytes"] <= 128 * 1024, "Reference corpus exceeds the runtime package bound")
    descriptor = {"schemaVersion": 2, "role": "CreativeLook", "configurationRelease": 40000,
                  "configurationSHA256": COOK.CONFIG_SHA256, "dimension": model["dimension"],
                  "domainMinimum": model["domainMinimum"], "domainMaximum": model["domainMaximum"],
                  "interpolation": model["interpolation"], "domainPolicy": "RejectOutsideV1"}
    manifest = {"schemaVersion": 1, "kind": "horo-creative-look-package", "descriptor": descriptor, "payload": data, "reference": refs}
    COOK.write_file(root, "manifest.json", COOK.json_bytes(manifest))
    return manifest


def arguments(argv: list[str] | None = None) -> argparse.Namespace:
    """Require explicit source, independently pinned tools/configuration, and a new destination."""
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "config", "tool-receipt", "wheel", "numpy-wheel", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--numpy-wheel-sha256", required=True)
    return parser.parse_args(argv)


def run(args: argparse.Namespace) -> dict:
    """Validate before tool activation and exclusively publish only a complete original generation."""
    model = source_model(COOK.read_bytes(args.source, 16 * 1024 * 1024))
    ocio, provenance = COOK.load_tool(args.tool_receipt, args.wheel)
    COOK.load_array_tool(args.numpy_wheel, args.numpy_wheel_sha256, args.tool_receipt.parent.resolve())
    output_cpu, _, identity = COOK.processor(ocio, args.config)
    corpus = references(ocio, model, output_cpu, identity, provenance)
    return COOK.publish(args.output, lambda stage: write_generation(stage, model, corpus))


def main(argv: list[str] | None = None) -> int:
    """Report a bounded cold-input failure without publishing a partial package."""
    try:
        manifest = run(arguments(argv))
        print(json.dumps({"kind": manifest["kind"], "dimension": manifest["descriptor"]["dimension"],
                          "interpolation": manifest["descriptor"]["interpolation"],
                          "scope": "offline-look-data-and-actual-OCIO-CPU-reference-generation"}))
        return 0
    except (COOK.CookError, OSError, ValueError, OverflowError, RuntimeError) as error:
        print(str(error)[:2048], file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
