#!/usr/bin/env python3
"""Generate a portable native Horo extension project from public SDK contracts."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path


MAXIMUM_IDENTIFIER_BYTES = 256
MAXIMUM_SEMANTIC_VERSION_BYTES = 64
MAXIMUM_DISPLAY_NAME_BYTES = 4 * 1024
MAXIMUM_PORTABLE_FILENAME_BYTES = 255
LONGEST_SHARED_MODULE_SUFFIX = ".dylib"
LONGEST_EXECUTABLE_SUFFIX = ".exe"


@dataclass(frozen=True)
class Module:
    suffix: str
    roles: tuple[str, ...]
    exports_service: bool = False
    imports_backend: bool = False


SHAPES = {
    "gui": (Module("editor", ("editor-presentation",)),),
    "backend": (Module("backend", ("backend-capability", "headless-tooling"), True),),
    "script": (Module("script", ("backend-capability", "script-provider"), True),),
    "hybrid": (
        Module("backend", ("backend-capability", "headless-tooling"), True),
        Module("editor", ("editor-presentation",), imports_backend=True),
        Module("script", ("script-provider",), imports_backend=True),
    ),
}


MODULE_SOURCE = r'''#include "Horo/Extensions/ExtensionAbi.h"

#include <stddef.h>

static const char module_id[] = "@MODULE_ID@";
static const char module_version[] = "@VERSION@";

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements) {
    if (requirements == NULL || requirements->structSize < sizeof(*requirements))
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    *requirements = (HoroExtensionRequirements){
        .structSize = sizeof(*requirements),
        .abiMajorVersion = HORO_EXTENSION_ABI_VERSION,
        .minimumHostMinor = HORO_EXTENSION_ABI_MINOR_VERSION,
        .requiredHostApiSize = sizeof(HoroExtensionHostApi),
    };
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module) {
    if (host == NULL || module == NULL || module->structSize < sizeof(*module) ||
        host->structSize < sizeof(*host) || host->abiVersion != HORO_EXTENSION_ABI_VERSION)
        return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
    *module = (HoroExtensionModuleApi){
        .structSize = sizeof(*module),
        .moduleId = {module_id, sizeof(module_id) - 1},
        .moduleVersion = {module_version, sizeof(module_version) - 1},
    };
    return HORO_EXTENSION_SUCCESS;
}

HORO_EXTENSION_EXPORT void horo_extension_unload(HoroExtensionModuleApi *module) {
    if (module != NULL)
        *module = (HoroExtensionModuleApi){0};
}
'''


CONTRACT_TEST = r'''#include "Horo/Extensions/ExtensionAbi.h"

#include <stddef.h>
#include <string.h>

HoroExtensionStatus horo_extension_query(HoroExtensionRequirements *requirements);
HoroExtensionStatus horo_extension_load(const HoroExtensionHostApi *host, HoroExtensionModuleApi *module);
void horo_extension_unload(HoroExtensionModuleApi *module);

int main(void) {
    static const char expected_id[] = "@MODULE_ID@";
    HoroExtensionRequirements requirements = {.structSize = sizeof(requirements)};
    if (horo_extension_query(&requirements) != HORO_EXTENSION_SUCCESS ||
        requirements.abiMajorVersion != HORO_EXTENSION_ABI_VERSION ||
        requirements.minimumHostMinor > HORO_EXTENSION_ABI_MINOR_VERSION)
        return 1;
    const HoroExtensionHostApi host = {
        .structSize = sizeof(host),
        .abiVersion = HORO_EXTENSION_ABI_VERSION,
        .abiMinorVersion = HORO_EXTENSION_ABI_MINOR_VERSION,
    };
    HoroExtensionModuleApi module = {.structSize = sizeof(module)};
    if (horo_extension_load(&host, &module) != HORO_EXTENSION_SUCCESS ||
        module.moduleId.length != sizeof(expected_id) - 1 ||
        memcmp(module.moduleId.data, expected_id, sizeof(expected_id) - 1) != 0)
        return 2;
    horo_extension_unload(&module);
    return module.moduleContext == NULL ? 0 : 3;
}
'''


def module_id(package_id: str, module: Module) -> str:
    return f"{package_id}.{module.suffix}"


def target_name(package_id: str, module: Module) -> str:
    return f"{re.sub(r'[^A-Za-z0-9]', '_', package_id)}_{module.suffix}"


def derived_identifiers(package_id: str, modules: tuple[Module, ...]) -> tuple[str, ...]:
    service_id = f"{package_id}.service"
    backend_id = f"{package_id}.backend"
    identifiers = {package_id}
    for module in modules:
        current_module_id = module_id(package_id, module)
        identifiers.add(current_module_id)
        if module.exports_service:
            identifiers.update((service_id,))
        if module.imports_backend:
            identifiers.update((backend_id, f"{current_module_id}.backend", service_id))
    return tuple(sorted(identifiers))


def generated_filenames(package_id: str, version: str, modules: tuple[Module, ...]) -> tuple[str, ...]:
    filenames = [f"{package_id}-{version}.zip"]
    for module in modules:
        target = target_name(package_id, module)
        filenames.extend((target + LONGEST_SHARED_MODULE_SUFFIX, target + "_contract_test" + LONGEST_EXECUTABLE_SUFFIX))
    return tuple(filenames)


def is_ascii_lower(character: str) -> bool:
    return "a" <= character <= "z"


def is_ascii_digit(character: str) -> bool:
    return "0" <= character <= "9"


def is_canonical_identifier(value: str) -> bool:
    if not value or len(value.encode("utf-8")) > MAXIMUM_IDENTIFIER_BYTES:
        return False
    for segment in value.split("."):
        if not segment or not is_ascii_lower(segment[0]) or segment[-1] == "-":
            return False
        if not all(is_ascii_lower(character) or is_ascii_digit(character) or character == "-" for character in segment):
            return False
    return True


def is_canonical_numeric_component(value: str) -> bool:
    return bool(value) and not (len(value) > 1 and value[0] == "0") and all(is_ascii_digit(character) for character in value)


def is_canonical_semantic_version(value: str) -> bool:
    if not value or len(value.encode("utf-8")) > MAXIMUM_SEMANTIC_VERSION_BYTES or "+" in value:
        return False
    core, separator, prerelease = value.partition("-")
    if len(core.split(".")) != 3 or not all(is_canonical_numeric_component(part) for part in core.split(".")):
        return False
    if not separator:
        return True
    for identifier in prerelease.split("."):
        if not identifier or not all(character.isascii() and (character.isalnum() or character == "-") for character in identifier):
            return False
        if all(is_ascii_digit(character) for character in identifier) and len(identifier) > 1 and identifier[0] == "0":
            return False
    return True


def render(text: str, replacements: dict[str, str]) -> str:
    for key, value in replacements.items():
        text = text.replace(f"@{key}@", value)
    return text


def manifest(package_id: str, name: str, version: str, modules: tuple[Module, ...]) -> dict:
    service_id = f"{package_id}.service"
    backend_id = f"{package_id}.backend"
    module_values = []
    for module in modules:
        value = {
            "id": module_id(package_id, module),
            "version": version,
            "kind": "native",
            "entry": f"bin/{target_name(package_id, module)}@CMAKE_SHARED_MODULE_SUFFIX@",
            "roles": list(module.roles),
            "abi": {"major": 1, "minimumMinor": 1},
        }
        if module.exports_service:
            value["exports"] = [{"id": service_id, "contract": service_id, "version": version}]
        if module.imports_backend:
            value["dependencies"] = [backend_id]
            value["imports"] = [{
                "id": f"{module_id(package_id, module)}.backend",
                "service": service_id,
                "contract": service_id,
                "minimumVersion": version,
            }]
        module_values.append(value)
    return {
        "schemaVersion": 1,
        "id": package_id,
        "displayName": name,
        "version": version,
        "modules": module_values,
        "contributions": [],
    }


def cmake_project(package_id: str, version: str, modules: tuple[Module, ...]) -> str:
    lines = [
        "cmake_minimum_required(VERSION 3.25)",
        "project(HoroExtension LANGUAGES C)",
        "find_package(HoroEngineExtensionSdk CONFIG REQUIRED)",
        "include(CTest)",
        'configure_file(extension.json.in "${CMAKE_CURRENT_BINARY_DIR}/extension.json" @ONLY)',
        "",
    ]
    for module in modules:
        target = target_name(package_id, module)
        source = f"src/{module.suffix}.c"
        test_target = f"{target}_contract_test"
        lines.extend([
            f"add_library({target} MODULE {source})",
            f"target_compile_features({target} PRIVATE c_std_11)",
            f"target_link_libraries({target} PRIVATE HoroEngine::ExtensionSdk)",
            f"set_target_properties({target} PROPERTIES PREFIX \"\" C_EXTENSIONS OFF C_VISIBILITY_PRESET hidden)",
            f"add_executable({test_target} tests/{module.suffix}_contract.c {source})",
            f"target_compile_features({test_target} PRIVATE c_std_11)",
            f"target_link_libraries({test_target} PRIVATE HoroEngine::ExtensionSdk)",
            f"set_target_properties({test_target} PROPERTIES C_EXTENSIONS OFF)",
            f"add_test(NAME {test_target} COMMAND {test_target})",
            f"install(TARGETS {target} DESTINATION bin)",
            "",
        ])
    lines.extend([
        'install(FILES "${CMAKE_CURRENT_BINARY_DIR}/extension.json" DESTINATION .)',
        'install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/horo-package.toml" DESTINATION .)',
        f'set(CPACK_PACKAGE_NAME "{package_id}")',
        f'set(CPACK_PACKAGE_VERSION "{version}")',
        f'set(CPACK_PACKAGE_FILE_NAME "{package_id}-{version}")',
        'set(CPACK_GENERATOR "ZIP")',
        "set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)",
        "include(CPack)",
        "",
    ])
    return "\n".join(lines)


def ci_template_dir() -> Path:
    root = Path(__file__).resolve().parent.parent
    packaged = root / "share/horo/extension-sdk/ci"
    if packaged.is_dir():
        return packaged
    return root / "sdk/ci"


def write_project(root: Path, package_id: str, name: str, version: str, shape: str) -> None:
    modules = SHAPES[shape]
    (root / "src").mkdir(parents=True)
    (root / "tests").mkdir()
    (root / ".github/workflows").mkdir(parents=True)
    (root / ".horo/ci").mkdir(parents=True)
    # Every destination below is a fixed or closed-table child of the private staging root.
    (root / "CMakeLists.txt").write_text(  # NOSONAR
        cmake_project(package_id, version, modules), encoding="utf-8"
    )
    (root / "extension.json.in").write_text(  # NOSONAR
        json.dumps(manifest(package_id, name, version, modules), indent=2) + "\n", encoding="utf-8"
    )
    (root / "horo-package.toml").write_text(  # NOSONAR
        "schemaVersion = 1\n\n[package]\n"
        f"id = {json.dumps(package_id, ensure_ascii=False)}\n"
        f"version = {json.dumps(version, ensure_ascii=False)}\n"
        "kind = \"extension\"\n"
        f"displayName = {json.dumps(name, ensure_ascii=False)}\n",
        encoding="utf-8",
    )
    templates = ci_template_dir()
    lock_file = templates / "extension-ci.lock.json"
    if not lock_file.is_file():
        lock_file = templates / "extension-ci.lock.json.in"
    shutil.copyfile(templates / "extension-author-ci.yml", root / ".github/workflows/extension-ci.yml")
    shutil.copyfile(templates / "bootstrap.py" if (templates / "bootstrap.py").is_file()
                    else Path(__file__).with_name("bootstrap_extension_ci.py"), root / ".horo/ci/bootstrap.py")
    shutil.copyfile(lock_file, root / ".horo/extension-ci.lock.json")
    for module in modules:
        replacements = {"MODULE_ID": module_id(package_id, module), "VERSION": version}
        (root / "src" / f"{module.suffix}.c").write_text(  # NOSONAR
            render(MODULE_SOURCE, replacements), encoding="utf-8"
        )
        (root / "tests" / f"{module.suffix}_contract.c").write_text(  # NOSONAR
            render(CONTRACT_TEST, replacements), encoding="utf-8"
        )
    (root / "README.md").write_text(  # NOSONAR
        f"# {name}\n\nGenerated `{shape}` Horo extension scaffold.\n\n"
        "Configure with `HoroEngineExtensionSdk_DIR` pointing to the SDK's "
        "`lib/cmake/HoroEngineExtensionSdk` directory. Build, run CTest, then build "
        "the `package` target to create the ZIP artifact.\n\n"
        "For source-free author CI, edit `.horo/extension-ci.lock.json` with the "
        "version-matched HTTPS SDK ZIP URL and SHA-256 for each platform. The "
        "pinned `.github/workflows/extension-ci.yml` builds, validates, tests, "
        "packages and verifies the unsigned archive on Linux, macOS and Windows. "
        "It publishes an attributable `.horopkg` and provenance, but does not "
        "sign releases or establish publisher trust.\n",
        encoding="utf-8",
    )


def validate(args: argparse.Namespace) -> None:
    if not is_canonical_identifier(args.id):
        raise ValueError("--id must be a lowercase reverse-domain identifier")
    if not is_canonical_semantic_version(args.version):
        raise ValueError("--version must be a canonical semantic version")
    if len(args.name.strip().encode("utf-8")) > MAXIMUM_DISPLAY_NAME_BYTES:
        raise ValueError("--name exceeds the manifest display-name limit")
    modules = SHAPES[args.shape]
    if not all(is_canonical_identifier(identifier) for identifier in derived_identifiers(args.id, modules)):
        raise ValueError("--id is too long for identifiers derived by this scaffold shape")
    if not all(len(filename.encode("utf-8")) <= MAXIMUM_PORTABLE_FILENAME_BYTES
               for filename in generated_filenames(args.id, args.version, modules)):
        raise ValueError("--id and --version produce a filename longer than the portable component limit")
    if os.path.lexists(args.output):
        if args.output.is_symlink() or not args.output.is_dir() or any(args.output.iterdir()):
            raise ValueError("--output must be an absent path or empty directory")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shape", choices=tuple(SHAPES), required=True)
    parser.add_argument("--id", required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--version", default="1.0.0")
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        validate(args)
        output = args.output.absolute()
        output.parent.mkdir(parents=True, exist_ok=True)
        output_parent = output.parent.resolve(strict=True)
        if output.name in {"", ".", ".."}:
            raise ValueError("--output must name a project directory")
        output = output_parent / output.name
        staging: Path | None = Path(tempfile.mkdtemp(prefix=".horo-extension.", dir=output_parent))
        try:
            write_project(staging, args.id, args.name.strip(), args.version, args.shape)
            if output.exists():
                output.rmdir()
            staging.replace(output)
            staging = None
        finally:
            if staging is not None:
                shutil.rmtree(staging, ignore_errors=True)
    except (OSError, ValueError) as error:
        print(f"error: {error}")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
