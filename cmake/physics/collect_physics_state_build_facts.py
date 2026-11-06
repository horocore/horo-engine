"""Collect conservative Physics inputs from the actual configured compilation database.

The profile is emitted by the owning CMake integration. This collector never
invents compiler/ABI/build facts, imports another build's recipes, or qualifies
the loaded process image. It deliberately hashes a superset of owned headers.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

from generate_physics_state_build_facts import atomic_write, canonical, require

SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inl', '.ipp', '.tpp'}
PROFILE_KEYS = {'version', 'sourceRoot', 'buildRoot', 'joltRoot', 'solver', 'physics', 'platform'}
TARGETS = {'HoroPhysics', 'HoroPhysicsModel', 'Jolt'}
MODEL_SCENE_SOURCES = {'src/runtime/scene/PhysicsSceneComponents.cpp',
                       'src/runtime/scene/CharacterSceneComponents.cpp'}


def read_bounded(path, maximum=16 * 1024 * 1024):
    require(path.is_file() and path.stat().st_size <= maximum, 'Missing/oversized configured input: ' + str(path))
    return json.loads(path.read_text())


def actual_source(entry):
    path = Path(entry['file'])
    return (Path(entry['directory']) / path).resolve(strict=True)


def target_name(entry, arguments):
    candidate = entry.get('output', '') + ' ' + ' '.join(arguments)
    matches = [name for name in TARGETS if 'CMakeFiles/' + name + '.dir/' in candidate.replace('\\', '/')]
    require(len(matches) <= 1, 'Ambiguous actual target recipe')
    return matches[0] if matches else None


def normalize_recipe(arguments, roots):
    output = []
    skip = False
    for argument in arguments:
        if skip:
            skip = False
            continue
        if argument in {'-o', '-MF', '-MT', '-MQ'}:
            skip = True
            continue
        require(not argument.startswith('@'), 'Response-file recipe requires an explicit supported parser')
        if argument.startswith(('/Fo', '/Fd')):
            continue
        normalized = argument
        for path, label in roots:
            normalized = normalized.replace(str(path), label)
            normalized = normalized.replace(str(path).replace('\\', '/'), label)
        output.append(normalized)
    require(not skip and bool(output), 'Truncated actual compiler arguments')
    return canonical(output).decode()


def source_inventory(root, prefix):
    require(root.is_dir(), 'Missing genuine source root: ' + str(root))
    return [{'identity': prefix + '/' + str(path.relative_to(root)).replace('\\', '/'), 'path': str(path)}
            for path in sorted(root.rglob('*')) if path.is_file() and path.suffix.lower() in SUFFIXES]


def required_input(path, identity):
    require(path.is_file(), 'Missing owning build input: ' + str(path))
    return {'identity': identity, 'path': str(path)}


def effective_facts(profile, recipes):
    """Bind effective policies to real recipes rather than caller-supplied labels."""
    solver = dict(profile['solver'], effectiveOptions={'recipes': sorted(recipes['Jolt'])})
    physics = dict(profile['physics'], compilePolicy={
        'recipes': sorted(recipes['HoroPhysicsModel'] + recipes['HoroPhysics'])})
    platform = dict(profile['platform'], codegen={'recipes': sorted(recipes['HoroPhysics'])})
    return solver, physics, platform


def windows_arguments(command, exporter, directory):
    """Use the configured MSVC CRT, not CommandLineToArgvW or a POSIX approximation.

    subprocess passes this original string as lpCommandLine while executable
    selects only our inert wmain exporter. The compiler is never executed.
    """
    require(isinstance(command, str) and command and '\0' not in command, 'Invalid Windows compilation command')
    require(len(command.encode('utf-16-le')) <= 65_532, 'Oversized Windows compilation command')
    require(exporter is not None, 'Windows recipes require same-toolchain argument-array export')
    tool = Path(exporter).resolve(strict=True)
    require(tool.is_file(), 'Missing Windows CRT argument exporter')
    completed = subprocess.run(command, executable=str(tool), cwd=directory, shell=False,
                               stdin=subprocess.DEVNULL, capture_output=True, timeout=10, check=False)
    require(completed.returncode == 0 and len(completed.stdout) <= 1024 * 1024,
            'Windows CRT argument export failed or exceeded its bound')
    arguments = json.loads(completed.stdout)
    require(isinstance(arguments, list) and 0 < len(arguments) <= 32_767, 'Invalid Windows CRT argument array')
    require(all(isinstance(value, str) and '\0' not in value for value in arguments), 'Invalid Windows CRT argument value')
    return arguments


def recipe_arguments(entry, profile, exporter):
    arguments = entry.get('arguments')
    if arguments is not None:
        return arguments
    if os.name != 'nt':
        return shlex.split(entry['command'], posix=True)
    require(profile['platform']['compiler'].startswith('MSVC:'), 'Unsupported Windows compiler argument semantics')
    return windows_arguments(entry['command'], exporter, entry['directory'])


def validate_windows_compiler(arguments, profile):
    """Selected Physics recipes must name the exact configured MSVC compiler, never a guessed wrapper."""
    declared = profile['platform']['compiler'].split(':', 2)
    require(len(declared) == 3 and declared[0] == 'MSVC', 'Unsupported Windows compiler argument semantics')
    require(bool(arguments), 'Missing Windows compiler argument')
    require(Path(arguments[0]).resolve(strict=True) == Path(declared[2]).resolve(strict=True),
            'Windows recipe does not name the configured compiler')


def owned_recipe(entry, target, source, build, jolt):
    require(Path(entry['directory']).resolve(strict=True) == build, 'Recipe belongs to another build')
    path = actual_source(entry)
    owner = jolt if target == 'Jolt' else source / 'src/physics'
    model_scene = is_model_scene_source(target, path, source)
    pch = is_generated_pch_source(target, path, build)
    require(path.is_relative_to(owner) or model_scene or pch, 'Unexpected source owner in Physics target: ' + str(path))
    return path, model_scene, pch


def is_model_scene_source(target, path, source):
    return (target == 'HoroPhysicsModel' and path.is_relative_to(source) and
            path.relative_to(source).as_posix() in MODEL_SCENE_SOURCES)


def is_generated_pch_source(target, path, build):
    return (target == 'Jolt' and path.is_relative_to(build / 'third-party/horo_jolt/CMakeFiles/Jolt.dir') and
            path.name in {'cmake_pch.hxx.cxx', 'cmake_pch.cxx'})


def unrelated_output(entry):
    # Actual CMake output ownership can exclude unrelated rows without launching
    # one CRT process per unrelated translation unit. Missing output stays strict.
    return bool(entry.get('output')) and target_name(entry, []) is None


def collect_recipes(profile, database, exporter, source, build, jolt, roots):
    recipes = {name: [] for name in TARGETS}
    model_scene_files, generated_solver_files = {}, {}
    for entry in database:
        if unrelated_output(entry):
            continue
        arguments = recipe_arguments(entry, profile, exporter)
        require(isinstance(arguments, list) and all(isinstance(x, str) for x in arguments), 'Invalid compilation recipe')
        target = target_name(entry, arguments)
        if target is None:
            continue
        if os.name == 'nt':
            validate_windows_compiler(arguments, profile)
        path, model_scene, generated_pch = owned_recipe(entry, target, source, build, jolt)
        if generated_pch:
            generated_solver_files[path.name] = required_input(path, 'jolt-generated/' + path.name)
            header = path.with_name('cmake_pch.hxx')
            generated_solver_files[header.name] = required_input(header, 'jolt-generated/' + header.name)
        if model_scene:
            relative = path.relative_to(source).as_posix()
            model_scene_files[relative] = required_input(path, 'engine/' + relative)
        recipes[target].append(normalize_recipe(arguments, roots))
    return recipes, model_scene_files, generated_solver_files


def collect(profile, database, windows_argv_exporter=None):
    require(set(profile) == PROFILE_KEYS and profile['version'] == 1, 'Unsupported configured Physics profile')
    source = Path(profile['sourceRoot']).resolve(strict=True)
    build = Path(profile['buildRoot']).resolve(strict=True)
    jolt = Path(profile['joltRoot']).resolve(strict=True)
    # CMake recipes may retain a declared symlink spelling (macOS /var) while
    # file ownership resolves its canonical spelling (/private/var). Normalize
    # both exact declared roots; do not guess aliases from arbitrary arguments.
    aliases = {(Path(profile[key]).absolute(), label) for key, label in
               [('sourceRoot', '$SOURCE'), ('buildRoot', '$BUILD'), ('joltRoot', '$JOLT')]}
    aliases.update({(source, '$SOURCE'), (build, '$BUILD'), (jolt, '$JOLT')})
    roots = sorted(aliases, key=lambda item: (-len(str(item[0])), str(item[0])))
    recipes, model_scene_files, generated_solver_files = collect_recipes(
        profile, database, windows_argv_exporter, source, build, jolt, roots)
    require(all(recipes.values()), 'Missing actual native Physics/PhysicsModel/Jolt recipes; omitted profile is unavailable')
    solver_files = source_inventory(jolt / 'Jolt', 'jolt/Jolt')
    solver_files += list(generated_solver_files.values())
    for relative in ['Build/CMakeLists.txt', 'LICENSE']:
        solver_files.append(required_input(jolt / relative, 'jolt/' + relative))
    for relative in ['cmake/HoroPhysicsDependency.cmake', 'cmake/patches/HoroJoltConstraintReservation.cmake']:
        solver_files.append(required_input(source / relative, 'engine/' + relative))
    physics_files = source_inventory(source / 'src/physics', 'engine/src/physics')
    physics_files += list(model_scene_files.values())
    physics_files += source_inventory(source / 'src/foundation', 'engine/src/foundation')
    physics_files += source_inventory(source / 'include/Horo', 'engine/include/Horo')
    physics_files.append(required_input(source / 'src/CMakeLists.txt', 'engine/src/CMakeLists.txt'))
    required_input(build / 'CMakeCache.txt', 'configured/CMakeCache.txt')
    # Cache bytes are not hashed as compatibility identity: cache contains local
    # paths and unrelated options. The authentic platform facts/recipes are the
    # authority; hash only actual compiler ABI helper implementation below.
    platform_files = [required_input(source / 'src/physics/PhysicsStateBuildFacts.cpp',
                                     'engine/src/physics/PhysicsStateBuildFacts.cpp')]
    solver_facts, physics_facts, platform_facts = effective_facts(profile, recipes)
    return {
        'version': 1,
        'solver': {'facts': solver_facts, 'files': solver_files, 'recipes': recipes['Jolt']},
        'physics': {'facts': physics_facts, 'files': physics_files,
                    'recipes': recipes['HoroPhysicsModel'] + recipes['HoroPhysics']},
        'platform': {'facts': platform_facts, 'files': platform_files, 'recipes': recipes['HoroPhysics']},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--compile-commands', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--windows-argv-exporter', type=Path)
    args = parser.parse_args()
    profile = read_bounded(args.profile, 1024 * 1024)
    database = read_bounded(args.compile_commands, 128 * 1024 * 1024)
    expected = Path(profile['buildRoot']).resolve(strict=True) / 'compile_commands.json'
    require(args.compile_commands.resolve(strict=True) == expected, 'Compilation database is not from the configured build')
    atomic_write(args.manifest, canonical(collect(profile, database, args.windows_argv_exporter)) + b'\n')


if __name__ == '__main__':
    main()
