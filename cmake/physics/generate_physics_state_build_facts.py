"""Generate private Physics compatibility digests from explicit, authentic build inputs.

The caller supplies effective facts/recipes from the actual configured target.
This tool hashes those inputs; it does not qualify determinism or identify a loaded image.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

REQUIRED = {
    "solver": {"revision", "archiveSha256", "patchSha256", "solverSchema", "effectiveOptions"},
    "physics": {"snapshotSchema", "orderingSchema", "compilePolicy"},
    "platform": {"compiler", "target", "abi", "buildMode", "codegen"},
}
SYMBOLS = {"solver": "SolverInputs", "physics": "PhysicsInputs", "platform": "ConfiguredPlatformInputs"}
MAX_FILE_BYTES = 64 * 1024 * 1024
MAX_TOTAL_BYTES = 512 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode()


def validate_fact(value):
    require(value is not None, "Missing build fact")
    if isinstance(value, str):
        require(bool(value.strip()), "Empty build fact")
    elif isinstance(value, dict):
        require(bool(value), "Empty build fact object")
        for key, child in value.items():
            require(isinstance(key, str) and bool(key), "Invalid build fact key")
            validate_fact(child)
    elif isinstance(value, list):
        require(bool(value), "Empty build fact list")
        for child in value:
            validate_fact(child)
    else:
        require(type(value) in (int, bool), "Unsupported build fact type")


def file_record(entry, remaining_bytes):
    require(set(entry) == {"identity", "path"}, "Unexpected source input fields")
    require(isinstance(entry["identity"], str) and entry["identity"], "Missing source identity")
    path = Path(entry["path"]).resolve(strict=True)
    before = path.stat()
    require(path.is_file() and before.st_size <= min(MAX_FILE_BYTES, remaining_bytes), "Invalid or oversized source input")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(65536), b""):
            digest.update(block)
    after = path.stat()
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), "Source changed while hashing")
    return {"identity": entry["identity"], "sha256": digest.hexdigest(), "bytes": before.st_size}


def section_record(name, section):
    require(set(section) == {"facts", "files", "recipes"}, "Unexpected section fields")
    facts = section["facts"]
    require(isinstance(facts, dict) and REQUIRED[name] <= set(facts), "Missing required target facts")
    validate_fact(facts)
    recipes = section["recipes"]
    require(isinstance(recipes, list) and bool(recipes), "Missing actual target recipes")
    require(all(isinstance(x, str) and x.strip() for x in recipes), "Invalid target recipe")
    require(isinstance(section["files"], list) and bool(section["files"]), "Missing actual source inputs")
    require(len(section["files"]) <= 8192, "Source input count exceeded")
    files = []
    remaining_bytes = MAX_TOTAL_BYTES
    for entry in section["files"]:
        record = file_record(entry, remaining_bytes)
        remaining_bytes -= record["bytes"]
        files.append(record)
    files.sort(key=lambda x: x["identity"])
    require(len({x["identity"] for x in files}) == len(files), "Duplicate source identity")
    require(sum(x["bytes"] for x in files) <= MAX_TOTAL_BYTES, "Source input budget exceeded")
    return {"facts": facts, "files": files, "recipes": sorted(recipes)}


def atomic_write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() == content:
        return
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def generate(manifest, header, report):
    require(set(manifest) == {"version", *REQUIRED}, "Unexpected manifest fields")
    require(type(manifest["version"]) is int and manifest["version"] == 1, "Unsupported build-facts version")
    records = {name: section_record(name, manifest[name]) for name in REQUIRED}
    digests = {name: hashlib.sha256(canonical({"domain": "horo.physics.state-build.v1/" + name,
                                             "inputs": record})).hexdigest() for name, record in records.items()}
    lines = ['#pragma once', '#include "Horo/Foundation/Sha256.h"', '#include <cstdint>',
             'namespace Horo::Physics::Detail::StateBuildFacts {', 'inline constexpr std::uint32_t ContractVersion = 1;']
    for name, digest in digests.items():
        values = ", ".join("0x" + digest[index:index + 2] for index in range(0, 64, 2))
        lines.append("inline constexpr Sha256Digest " + SYMBOLS[name] + "{.bytes = {" + values + "}};")
    lines.append('extern const Sha256Digest PlatformAbi; // Defined once from the actual native compiler/STL/layout.')
    lines.append('} // namespace Horo::Physics::Detail::StateBuildFacts')
    atomic_write(header, ("\n".join(lines) + "\n").encode())
    atomic_write(report, json.dumps({"version": 1, "digests": digests, "inputs": records,
                                    "scope": "Configured inputs only; compiled native TU supplies final PlatformAbi; no loaded-image or determinism qualification"},
                                   indent=2, sort_keys=True).encode() + b"\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    require(args.manifest.stat().st_size <= 4 * 1024 * 1024, "Oversized build manifest")
    generate(json.loads(args.manifest.read_text()), args.header, args.report)


if __name__ == "__main__":
    main()
