"""Private same-run shader qualification data; never installs tools or grants catalog trust."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import stat
import subprocess

SOURCE = "assets/shaders/runtime/ui_paint.hlsl"
CATALOG = "src/runtime/renderer/toolchain/ShaderCompilerToolchainCatalog.cpp"
SCRIPT = ".github/scripts/ui_paint_fixture.py"
LIMIT = 16 * 1024 * 1024
STAGES = (("PaintVertex", "vert", "vs_6_0"), ("PaintFragment", "frag", "ps_6_0"))
DXC_OPTIONS = ("-nologo", "-HV", "2021", "-Ges", "-Zpc", "-Gis", "-O3",
               "-spirv", "-fspv-target-env=vulkan1.3", "-fvk-use-gl-layout")
APPLE_OPTIONS = ("-std=macos-metal2.4", "-mmacosx-version-min=14.0")
PARAMETER_OFFSETS = {"Extent": 0, "Opacity": 8, "Sampling": 12, "Flags": 16,
                     "Reserved0": 20, "Reserved1": 24, "Reserved2": 28,
                     "MaskExtent": 32, "MaskReserved": 40, "CornerRadii": 48}


def absolute_data_path(path):
    """Translate only macOS's fixed system /var alias; never resolve caller-controlled descendants."""
    path = path.absolute()
    if ".." in path.parts:
        raise ValueError("Parent traversal forbidden")
    if platform.system() == "Darwin" and path.parts[:2] == ("/", "var"):
        try:
            target = os.readlink("/var")
        except OSError as error:
            raise ValueError("Unexpected macOS temporary-root alias") from error
        if target not in ("private/var", "/private/var"):
            raise ValueError("Unexpected macOS temporary-root alias")
        path = Path("/private/var").joinpath(*path.parts[2:])
    return path


def bounded(path, maximum=LIMIT):
    """Anchor every physical component with no-follow descriptors, including during concurrent replacement."""
    path = absolute_data_path(path)
    directory = os.open(path.anchor, os.O_RDONLY | os.O_DIRECTORY)
    try:
        for component in path.parts[1:-1]:
            next_directory = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=directory)
            os.close(directory)
            directory = next_directory
        descriptor = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=directory)
        with os.fdopen(descriptor, "rb") as stream:
            evidence = os.fstat(stream.fileno())
            if not stat.S_ISREG(evidence.st_mode) or not 0 < evidence.st_size <= maximum:
                raise ValueError(f"Missing/empty/oversized input: {path.name}")
            data = stream.read(maximum + 1)
    except OSError as error:
        raise ValueError("Input absent or not a confined regular file") from error
    finally:
        os.close(directory)
    if not 0 < len(data) <= maximum:
        raise ValueError("Input changed size while reading")
    return data


def digest(data):
    return hashlib.sha256(data).hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate manifest member")
        result[key] = value
    return result


def identity(source):
    expected = {key: os.environ[key] for key in
                ("GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_REPOSITORY")}
    if not re.fullmatch(r"[0-9a-f]{40}", expected["GITHUB_SHA"]):
        raise ValueError("Invalid revision")
    for key in ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT"):
        if not re.fullmatch(r"[1-9][0-9]{0,19}", expected[key]):
            raise ValueError("Invalid run identity")
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    if actual != expected["GITHUB_SHA"]:
        raise ValueError("Checkout does not match current run revision")
    return expected


def catalog(source):
    text = bounded(source / CATALOG, 128 * 1024).decode("utf-8")
    result = {}
    for role, body in re.findall(r"ApprovedShaderCompilerTool\{ShaderCompilerTool::(\w+),\s*(.*?)\}", text, re.S):
        if role not in ("Dxc", "SpirvTools", "SpirvCross"):
            continue
        fields = ["".join(re.findall(r'"([^"\n]*)"', part)) for part in body.split(",")]
        if len(fields) != 7 or fields[0] != "linux-x86_64-ubuntu-26.04" or role in result:
            raise ValueError("Unexpected canonical tool lock")
        result[role] = dict(zip(("host", "release", "url", "archive", "binary", "license", "path"), fields))
    if set(result) != {"Dxc", "SpirvTools", "SpirvCross"}:
        raise ValueError("Missing canonical tool role")
    return result


def source_evidence(source):
    data = bounded(source / SOURCE)
    # The current canonical shader is self-contained. Fail closed if it gains includes;
    # a future producer must explicitly bind that complete dependency closure.
    if re.search(rb"^\s*#\s*include\b", data, re.M):
        raise ValueError("Shader include closure requires an explicit qualification update")
    return {SOURCE: digest(data), CATALOG: digest(bounded(source / CATALOG, 128 * 1024)),
            SCRIPT: digest(bounded(source / SCRIPT, 128 * 1024))}


def expected_manifest(source):
    return {"schema": 1, "identity": identity(source), "source": source_evidence(source),
            "tools": catalog(source), "dxc_options": list(DXC_OPTIONS),
            "cross_options": ["--msl", "--msl-version", "20400"],
            "reflect_options": ["--reflect"],
            "apple_options": list(APPLE_OPTIONS),
            "target": "metal-library-2.4", "entries": [list(stage) for stage in STAGES]}


def inventory(root):
    if root.is_symlink() or not root.is_dir():
        raise ValueError("Absent or symlink handoff root")
    paths = []
    for path in root.iterdir():
        paths.append(path)
        if len(paths) > 8:
            raise ValueError("Too many handoff files")
    if len(paths) > 8 or any(path.is_symlink() or not path.is_file() for path in paths):
        raise ValueError("Handoff must contain only a finite flat data-file set")
    return {path.name for path in paths}


def verify(root, source):
    expected_files = {"manifest.json"} | {entry + suffix for entry, _, _ in STAGES
                                          for suffix in (".spv", ".metal", ".reflection.json")}
    if inventory(root) != expected_files:
        raise ValueError("Unexpected handoff entries")
    manifest = json.loads(bounded(root / "manifest.json", 64 * 1024), object_pairs_hook=unique_object)
    expected = expected_manifest(source)
    hashes = {name: digest(bounded(root / name)) for name in sorted(expected_files - {"manifest.json"})}
    if manifest != {**expected, "files": hashes}:
        raise ValueError("Stale/tampered source, run, options, tools or data")
    for entry, _, _ in STAGES:
        if bounded(root / (entry + ".spv"))[:4] != b"\x03\x02\x23\x07":
            raise ValueError("Invalid SPIR-V magic")
        if b"#include <metal_stdlib>" not in bounded(root / (entry + ".metal")):
            raise ValueError("Invalid MSL translation")
        validate_reflection(root / (entry + ".reflection.json"))
    return manifest


def validate_reflection(path):
    """Require actual compiler-produced offsets to match the public fixed64-byte raster ABI."""
    reflection = json.loads(bounded(path, 64 * 1024), object_pairs_hook=unique_object)
    uniforms = reflection.get("ubos", [])
    if len(uniforms) != 1:
        raise ValueError("Expected one exact raster uniform block")
    block = uniforms[0]
    if (block.get("block_size"), block.get("set"), block.get("binding")) != (64, 0, 0):
        raise ValueError("Compiler uniform size/binding differs from raster ABI")
    members = reflection["types"][block["type"]]["members"]
    offsets = {member["name"]: member["offset"] for member in members}
    if len(members) != len(PARAMETER_OFFSETS) or offsets != PARAMETER_OFFSETS:
        raise ValueError("Compiler member offsets differ from raster ABI")


def run(command, env=None):
    # Audited private boundary: the producer checks every Linux executable against
    # its canonical lock digest before this call; Apple admits only /usr/bin/xcrun.
    # Callers construct fixed options and owned absolute data paths. The manifest
    # never supplies commands, executable paths or options. Preserve actual argv0
    # for the pinned native loader; no shell interprets the data arguments.
    subprocess.run(command, check=True, timeout=60, stdin=subprocess.DEVNULL, env=env, shell=False)


def native_bindings(root):
    """Derive the fixture map from generated MSL; actual Metal reflection checks it again."""
    stages = {entry: bounded(root / (entry + ".metal")).decode("utf-8") for entry, _, _ in STAGES}
    # SPIRV-Cross sanitizes the compiler's struct type name. The preceding exact
    # reflection check establishes its one uniform block; do not infer a name.
    patterns = ((r"constant\s+\w+\s*&\s*\w+\s*\[\[buffer\((\d+)\)\]\]", "buffer"),
                (r"\bPaintTexture\s*\[\[texture\((\d+)\)\]\]", "texture"),
                (r"\bCoverageTexture\s*\[\[texture\((\d+)\)\]\]", "texture"),
                (r"\bPaintSampler\s*\[\[sampler\((\d+)\)\]\]", "sampler"))
    indices = []
    for pattern, _ in patterns:
        matches = re.findall(pattern, stages["PaintFragment"])
        if len(matches) != 1 or int(matches[0]) > 30:
            raise ValueError("Generated MSL resource mapping is absent or ambiguous")
        indices.append(int(matches[0]))
    if re.findall(patterns[0][0], stages["PaintVertex"]) != [str(indices[0])] or indices[1] == indices[2]:
        raise ValueError("MSL stage bindings disagree")
    return indices


def produce(args):
    if os.name != "posix" or platform.machine() != "x86_64" or not Path("/etc/os-release").exists():
        raise ValueError("Linux qualification host required")
    release = Path("/etc/os-release").read_text()
    if not re.search(r'^ID=ubuntu$', release, re.M) or not re.search(r'^VERSION_ID="26\.04"$', release, re.M):
        raise ValueError("Exact approved Ubuntu26.04 host required")
    metadata = expected_manifest(args.source)
    tools = {"Dxc": args.dxc, "SpirvTools": args.validator, "SpirvCross": args.cross}
    if args.tools_root:
        for index, role in enumerate(tools):
            relative = Path(metadata["tools"][role]["path"])
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError("Canonical executable path is not confined")
            tools[role] = args.tools_root / str(index) / relative
    for role, path in tools.items():
        if path is None or "sha256:" + digest(bounded(path, 256 * 1024 * 1024)) != metadata["tools"][role]["binary"]:
            raise ValueError("Tool executable differs from canonical lock")
    tool_environment = os.environ.copy()
    tool_environment["LD_LIBRARY_PATH"] = str(tools["Dxc"].parent.parent / "lib") + ":" + tool_environment.get("LD_LIBRARY_PATH", "")
    args.root.mkdir(parents=True, exist_ok=False)
    for entry, stage, profile in STAGES:
        spirv = args.root / (entry + ".spv")
        msl = args.root / (entry + ".metal")
        run([str(tools["Dxc"]), *DXC_OPTIONS, "-T", profile, "-E", entry, "-Fo", str(spirv), str(args.source / SOURCE)], tool_environment)
        bounded(spirv)
        run([str(tools["SpirvTools"]), "--target-env", "vulkan1.3", str(spirv)], tool_environment)
        reflection = args.root / (entry + ".reflection.json")
        run([str(tools["SpirvCross"]), str(spirv), "--reflect", "--output", str(reflection)], tool_environment)
        validate_reflection(reflection)
        run([str(tools["SpirvCross"]), str(spirv), "--output", str(msl), "--entry", entry,
             "--stage", stage, "--msl", "--msl-version", "20400"], tool_environment)
        bounded(msl)
    metadata["files"] = {name: digest(bounded(args.root / name)) for name in sorted(inventory(args.root))}
    (args.root / "manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
    verify(args.root, args.source)


def apple(args):
    manifest = verify(args.root, args.source)  # Complete validation before tooling or output publication.
    bindings = native_bindings(args.root)
    args.output = absolute_data_path(args.output)
    if args.xcrun != Path("/usr/bin/xcrun"):
        raise ValueError("Only the selected installed Apple tool dispatcher is admitted")
    bounded(args.xcrun)
    installed = {}
    for tool in ("metal", "metallib"):
        path = Path(subprocess.check_output(["/usr/bin/xcrun", "-sdk", "macosx", "--find", tool],
                                           text=True, timeout=10).strip())
        installed[tool] = {"path": str(path), "sha256": digest(bounded(path, 256 * 1024 * 1024))}
    manifest["apple_tools"] = installed
    manifest["apple_sdk"] = subprocess.check_output(["/usr/bin/xcrun", "-sdk", "macosx", "--show-sdk-path"],
                                                    text=True, timeout=10).strip()
    if any(parent.is_symlink() for parent in (args.output, *args.output.absolute().parents)):
        raise ValueError("Symlink output directory forbidden")
    args.output.mkdir(parents=True, exist_ok=False)
    for entry, _, _ in STAGES:
        air = args.output / (entry + ".air")
        library = args.output / (entry + ".metallib")
        run([str(args.xcrun), "-sdk", "macosx", "metal", *APPLE_OPTIONS,
             "-c", str(args.root / (entry + ".metal")), "-o", str(air)])
        bounded(air)
        run([str(args.xcrun), "-sdk", "macosx", "metallib", str(air), "-o", str(library)])
        if bounded(library)[:4] != b"MTLB":
            raise ValueError("Apple linker did not produce a native Metal library")
    for evidence in installed.values():
        if digest(bounded(Path(evidence["path"]), 256 * 1024 * 1024)) != evidence["sha256"]:
            raise ValueError("Apple executable changed during qualification")
    manifest["apple_libraries"] = {entry: digest(bounded(args.output / (entry + ".metallib"))) for entry, _, _ in STAGES}
    for entry, _, _ in STAGES:
        (args.output / (entry + ".sha256")).write_text("sha256:" + manifest["apple_libraries"][entry] + "\n")
    for entry, _, _ in STAGES:
        for suffix in (".metal", ".reflection.json"):
            (args.output / (entry + suffix)).write_bytes(bounded(args.root / (entry + suffix)))
    cross = manifest["tools"]["SpirvCross"]
    (args.output / "reflection-tool.txt").write_text(cross["release"] + "\n" + cross["archive"] + "\n")
    (args.output / "bindings.txt").write_text(" ".join(map(str, bindings)) + "\n")
    (args.output / "qualification.json").write_text(json.dumps(manifest, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("produce", "verify", "apple"))
    parser.add_argument("--source", type=Path, default=Path.cwd())
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--dxc", type=Path)
    parser.add_argument("--validator", type=Path)
    parser.add_argument("--cross", type=Path)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--xcrun", type=Path, default=Path("/usr/bin/xcrun"))
    args = parser.parse_args()
    args.source = absolute_data_path(args.source)
    args.root = absolute_data_path(args.root)
    if args.operation == "apple" and args.output is None:
        parser.error("apple requires --output")
    {"produce": produce, "verify": lambda value: verify(value.root, value.source), "apple": apple}[args.operation](args)


if __name__ == "__main__":
    main()
