"""Build-facts generator regressions. All metadata is explicitly synthetic test data."""
import copy
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("physics_facts", Path(__file__).resolve().parents[2] / "cmake/physics/generate_physics_state_build_facts.py")
facts = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(facts)
import sys
sys.modules["generate_physics_state_build_facts"] = facts
COLLECTOR_SPEC = importlib.util.spec_from_file_location(
    "physics_collector", Path(__file__).resolve().parents[2] / "cmake/physics/collect_physics_state_build_facts.py")
collector = importlib.util.module_from_spec(COLLECTOR_SPEC)
COLLECTOR_SPEC.loader.exec_module(collector)
generator = facts

CHECKER_SPEC = importlib.util.spec_from_file_location(
    "physics_windows_argv_check", Path(__file__).resolve().parents[2] / "cmake/physics/check_physics_windows_argv.py")
checker = importlib.util.module_from_spec(CHECKER_SPEC)
with patch.dict(sys.modules, {"collect_physics_state_build_facts": collector,
                              "generate_physics_state_build_facts": facts}):
    CHECKER_SPEC.loader.exec_module(checker)




class BuildFactsTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="horo-physics-facts-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.a = self.root / "a.cpp"
        self.b = self.root / "b.cpp"
        self.a.write_bytes(b"source A")
        self.b.write_bytes(b"source B")
        self.header = self.root / "facts.h"
        self.report = self.root / "facts.json"
        self.manifest = {"version": 1}
        for name, required in facts.REQUIRED.items():
            self.manifest[name] = {
                "facts": {key: "SYNTHETIC_TEST_ONLY" for key in required},
                "files": [{"identity": name + "/a", "path": str(self.a)},
                          {"identity": name + "/b", "path": str(self.b)}],
                "recipes": ["test command B", "test command A"],
            }

    def generate(self, manifest=None):
        facts.generate(manifest or self.manifest, self.header, self.report)
        return json.loads(self.report.read_text())["digests"]

    def assert_rejected(self, manifest):
        before = self.header.read_bytes()
        with self.assertRaises((ValueError, FileNotFoundError)):
            self.generate(manifest)
        self.assertEqual(before, self.header.read_bytes())

    def test_input_order_is_irrelevant_and_unchanged_header_keeps_mtime(self):
        expected = self.generate()
        stamp = self.header.stat().st_mtime_ns
        for section in facts.REQUIRED:
            self.manifest[section]["files"].reverse()
            self.manifest[section]["recipes"].reverse()
        self.assertEqual(expected, self.generate())
        self.assertEqual(stamp, self.header.stat().st_mtime_ns)

    def test_effective_solver_policy_change_invalidates_only_solver(self):
        previous = self.generate()
        self.manifest["solver"]["facts"]["effectiveOptions"] = "DIFFERENT_TEST_POLICY"
        current = self.generate()
        self.assertNotEqual(previous["solver"], current["solver"])
        self.assertEqual(previous["physics"], current["physics"])
        self.assertEqual(previous["platform"], current["platform"])

    def test_real_source_bytes_change_invalidates_every_consumer(self):
        previous = self.generate()
        self.a.write_bytes(b"changed actual source bytes")
        current = self.generate()
        for section in facts.REQUIRED:
            self.assertNotEqual(previous[section], current[section])

    def test_invalid_build_evidence_rejects_without_replacing_header(self):
        self.generate()
        cases = []
        case = copy.deepcopy(self.manifest)
        del case["platform"]["facts"]["compiler"]
        cases.append(case)
        case = copy.deepcopy(self.manifest)
        case["version"] = 2
        cases.append(case)
        case = copy.deepcopy(self.manifest)
        case["solver"]["recipes"] = []
        cases.append(case)
        case = copy.deepcopy(self.manifest)
        case["physics"]["files"][1]["identity"] = case["physics"]["files"][0]["identity"]
        cases.append(case)
        case = copy.deepcopy(self.manifest)
        case["physics"]["files"][0]["path"] = str(self.root / "missing")
        cases.append(case)
        for case in cases:
            with self.subTest(case=case):
                self.assert_rejected(case)

    def test_aggregate_budget_rejects_before_reading_the_next_file(self):
        self.generate()
        original = Path.open
        opened = []
        def observe(path, *args, **kwargs):
            opened.append(path)
            return original(path, *args, **kwargs)
        with patch.object(facts, "MAX_TOTAL_BYTES", self.a.stat().st_size), patch.object(Path, "open", observe):
            with self.assertRaises(ValueError):
                self.generate()
        self.assertNotIn(self.b, opened)



import types

class CollectorTests(unittest.TestCase):
    def setUp(self):
        # These recipes model GCC even on Windows. MSVC cases select their own adapter.
        self.enterContext(patch.object(collector, "os", types.SimpleNamespace(name="posix")))
        self.directory = tempfile.TemporaryDirectory(prefix="horo-physics-collector-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "source"
        self.build = self.source / "build"
        self.jolt = self.source / "vendor/jolt"
        files = {
            self.source / "src/physics/World.cpp": "synthetic physics",
            self.source / "src/physics/Model.cpp": "synthetic model",
            self.source / "src/physics/PhysicsStateBuildFacts.cpp": "synthetic ABI helper",
            self.source / "src/foundation/Invariant.h": "synthetic foundation",
            self.source / "include/Horo/Physics/World.h": "synthetic public contract",
            self.source / "src/CMakeLists.txt": "synthetic target composition",
            self.source / "cmake/HoroPhysicsDependency.cmake": "synthetic pin",
            self.source / "cmake/patches/HoroJoltConstraintReservation.cmake": "synthetic patch",
            self.jolt / "Jolt/Solver.cpp": "synthetic solver",
            self.jolt / "Build/CMakeLists.txt": "synthetic solver recipe",
            self.jolt / "LICENSE": "synthetic license fixture",
            self.build / "CMakeCache.txt": "synthetic local cache",
        }
        for path, content in files.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        self.profile = {"version": 1, "sourceRoot": str(self.source), "buildRoot": str(self.build), "joltRoot": str(self.jolt)}
        for section, keys in generator.REQUIRED.items():
            self.profile[section] = {key: "SYNTHETIC_UNIT_FIXTURE" for key in keys}
        self.database = [self.recipe("HoroPhysics", self.source / "src/physics/World.cpp"),
                         self.recipe("HoroPhysicsModel", self.source / "src/physics/Model.cpp"),
                         self.recipe("Jolt", self.jolt / "Jolt/Solver.cpp")]

    def recipe(self, target, source):
        output = self.build / ("CMakeFiles/" + target + ".dir/unit.o")
        return {"directory": str(self.build), "file": str(source), "output": str(output),
                "arguments": ["/synthetic/compiler", "-I" + str(self.source / "include"), "-DREAL_RECIPE_FIXTURE=1",
                              "-o", str(output), "-MF", str(output) + ".d", "-c", str(source)]}

    def manifest(self):
        return collector.collect(self.profile, self.database)

    def digests(self):
        generator.generate(self.manifest(), self.root / "facts.h", self.root / "facts.json")
        return json.loads((self.root / "facts.json").read_text())["digests"]

    def test_collects_all_three_actual_target_owners_and_excludes_output_paths(self):
        manifest = self.manifest()
        self.assertEqual(len(manifest["solver"]["recipes"]), 1)
        self.assertEqual(len(manifest["physics"]["recipes"]), 2)
        recipe = manifest["physics"]["recipes"][0]
        self.assertIn("$SOURCE", recipe)
        self.assertNotIn(str(self.source), recipe)
        self.assertNotIn("unit.o", recipe)
        self.assertNotIn('"-MF"', recipe)
        self.assertEqual(manifest["physics"]["facts"]["snapshotSchema"], self.profile["physics"]["snapshotSchema"])
        self.assertEqual(manifest["physics"]["facts"]["compilePolicy"]["recipes"],
                         sorted(manifest["physics"]["recipes"]))

    def add_model_scene_recipe(self, name):
        path = self.source / "src/runtime/scene" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("synthetic Scene component implementation")
        self.database.append(self.recipe("HoroPhysicsModel", path))
        return path

    def test_real_model_scene_sources_are_owned_and_content_invalidates_physics(self):
        paths = [self.add_model_scene_recipe(name) for name in
                 ("PhysicsSceneComponents.cpp", "CharacterSceneComponents.cpp")]
        manifest = self.manifest()
        identities = {item["identity"] for item in manifest["physics"]["files"]}
        for path in paths:
            self.assertIn("engine/" + path.relative_to(self.source).as_posix(), identities)
        before = self.digests()
        paths[0].write_text("changed Scene component implementation")
        after = self.digests()
        self.assertNotEqual(before["physics"], after["physics"])
        self.assertEqual(before["solver"], after["solver"])
        self.assertEqual(before["platform"], after["platform"])

    def test_other_scene_source_cannot_be_admitted_by_directory_alone(self):
        self.add_model_scene_recipe("UnexpectedOwner.cpp")
        with self.assertRaisesRegex(ValueError, "source owner"):
            self.manifest()

    def test_scene_component_exception_is_not_available_to_native_target(self):
        path = self.add_model_scene_recipe("PhysicsSceneComponents.cpp")
        self.database[-1] = self.recipe("HoroPhysics", path)
        with self.assertRaisesRegex(ValueError, "source owner"):
            self.manifest()

    def test_configured_jolt_pch_content_is_accounted_as_solver_input(self):
        folder = self.build / "third-party/horo_jolt/CMakeFiles/Jolt.dir"
        folder.mkdir(parents=True)
        source = folder / "cmake_pch.hxx.cxx"
        header = folder / "cmake_pch.hxx"
        source.write_text('#include "cmake_pch.hxx"')
        header.write_text("synthetic configured PCH includes")
        self.database.append(self.recipe("Jolt", source))
        before = self.digests()
        header.write_text("changed configured PCH includes")
        after = self.digests()
        self.assertNotEqual(before["solver"], after["solver"])
        self.assertEqual(before["physics"], after["physics"])
        self.assertEqual(before["platform"], after["platform"])

    def test_pch_name_cannot_admit_another_build_or_target(self):
        source = self.source / "foreign-build/CMakeFiles/Jolt.dir/cmake_pch.hxx.cxx"
        source.parent.mkdir(parents=True)
        source.write_text("foreign generated PCH")
        self.database.append(self.recipe("Jolt", source))
        with self.assertRaisesRegex(ValueError, "source owner"):
            self.manifest()

    def test_output_path_and_local_cache_changes_do_not_change_compatibility(self):
        before = self.digests()
        for entry in self.database:
            entry["arguments"][entry["arguments"].index("-o") + 1] += ".other"
            entry["arguments"][entry["arguments"].index("-MF") + 1] += ".other"
        (self.build / "CMakeCache.txt").write_text("different local cache path/options")
        self.assertEqual(before, self.digests())

    def test_solver_compiler_policy_invalidates_only_solver(self):
        before = self.digests()
        self.database[2]["arguments"].append("-fno-fast-math")
        after = self.digests()
        self.assertNotEqual(before["solver"], after["solver"])
        self.assertEqual(before["physics"], after["physics"])
        self.assertEqual(before["platform"], after["platform"])

    def test_physics_compiler_policy_invalidates_physics_and_platform(self):
        before = self.digests()
        self.database[0]["arguments"].append("-fno-fast-math")
        after = self.digests()
        self.assertEqual(before["solver"], after["solver"])
        self.assertNotEqual(before["physics"], after["physics"])
        self.assertNotEqual(before["platform"], after["platform"])

    def test_owned_public_header_bytes_invalidate_physics_only(self):
        before = self.digests()
        (self.source / "include/Horo/Physics/World.h").write_text("different real bytes in fixture")
        after = self.digests()
        self.assertNotEqual(before["physics"], after["physics"])
        self.assertEqual(before["solver"], after["solver"])
        self.assertEqual(before["platform"], after["platform"])

    def test_foreign_build_source_and_missing_native_target_are_rejected(self):
        original = copy.deepcopy(self.database)
        self.database[0]["directory"] = str(self.source)
        with self.assertRaisesRegex(ValueError, "another build"):
            self.manifest()
        self.database = copy.deepcopy(original)
        self.database[0]["file"] = str(self.jolt / "Jolt/Solver.cpp")
        with self.assertRaisesRegex(ValueError, "source owner"):
            self.manifest()
        self.database = original[:2]
        with self.assertRaisesRegex(ValueError, "Missing actual native"):
            self.manifest()

    def test_response_and_truncated_argument_recipes_are_rejected(self):
        self.database[0]["arguments"].append("@unknown.rsp")
        with self.assertRaisesRegex(ValueError, "Response-file"):
            self.manifest()
        self.database[0]["arguments"].pop()
        self.database[0]["arguments"].append("-MF")
        with self.assertRaisesRegex(ValueError, "Truncated"):
            self.manifest()

    def test_windows_command_string_cannot_be_posix_reinterpreted(self):
        self.database[0]["command"] = "untrusted Windows quoting"
        del self.database[0]["arguments"]
        self.profile["platform"]["compiler"] = "MSVC:19.44:C:/configured/cl.exe"
        with patch.object(collector, "os", types.SimpleNamespace(name="nt")):
            with self.assertRaisesRegex(ValueError, "argument-array export"):
                self.manifest()

    def exporter(self):
        path = self.root / "synthetic-argv-exporter.exe"
        path.write_bytes(b"NOT_EXECUTABLE_UNIT_FIXTURE")
        return path

    def test_windows_export_uses_original_string_and_fixed_executable_without_shell(self):
        command = r'"C:\compiler path\cl.exe" /DVALUE="a b" /c source.cpp'
        arguments = [r'C:\compiler path\cl.exe', '/DVALUE=a b', '/c', 'source.cpp']
        completed = types.SimpleNamespace(returncode=0, stdout=json.dumps(arguments).encode())
        exporter = self.exporter()
        with patch.object(collector.subprocess, "run", return_value=completed) as run:
            self.assertEqual(collector.windows_arguments(command, exporter, self.build), arguments)
        self.assertEqual(run.call_args.args, (command,))
        self.assertEqual(run.call_args.kwargs["executable"], str(exporter.resolve()))
        self.assertIs(run.call_args.kwargs["shell"], False)
        self.assertEqual(run.call_args.kwargs["timeout"], 10)

    def test_windows_export_failure_or_invalid_output_cannot_become_arguments(self):
        cases = [(1, b'[]'), (0, b'{}'), (0, b'[]'), (0, b'[1]'), (0, b'["bad\\u0000value"]')]
        exporter = self.exporter()
        for status, output in cases:
            with self.subTest(status=status, output=output):
                completed = types.SimpleNamespace(returncode=status, stdout=output)
                with patch.object(collector.subprocess, "run", return_value=completed):
                    with self.assertRaises(ValueError):
                        collector.windows_arguments('cl.exe /c unit.cpp', exporter, self.build)

    def test_windows_invalid_or_oversized_command_is_rejected_before_process(self):
        exporter = self.exporter()
        for command in ['', 'cl.exe\0foreign', 'a' * 32_767]:
            with patch.object(collector.subprocess, "run") as run:
                with self.assertRaises(ValueError):
                    collector.windows_arguments(command, exporter, self.build)
                run.assert_not_called()

    def test_windows_selected_rows_require_exact_configured_msvc_compiler(self):
        compiler = self.root / 'configured-cl.exe'
        compiler.write_bytes(b'SYNTHETIC_COMPILER_FILE')
        foreign = self.root / 'foreign-cl.exe'
        foreign.write_bytes(b'FOREIGN_SYNTHETIC_COMPILER_FILE')
        self.profile['platform']['compiler'] = 'MSVC:19.44:' + str(compiler)
        collector.validate_windows_compiler([str(compiler), '/c', 'unit.cpp'], self.profile)
        with self.assertRaisesRegex(ValueError, 'configured compiler'):
            collector.validate_windows_compiler([str(foreign)], self.profile)
        self.profile['platform']['compiler'] = 'Unknown:1:' + str(compiler)
        with self.assertRaisesRegex(ValueError, 'Unsupported Windows compiler'):
            collector.validate_windows_compiler([str(compiler)], self.profile)

    def test_windows_command_recipes_preserve_provenance_and_response_denial(self):
        compiler = self.root / 'configured-cl.exe'
        compiler.write_bytes(b'SYNTHETIC_COMPILER_FILE')
        self.profile['platform']['compiler'] = 'MSVC:19.44:' + str(compiler)
        decoded = []
        for entry in self.database:
            args = entry.pop('arguments')
            args[0] = str(compiler)
            entry['command'] = 'SYNTHETIC_RAW_NOT_EXECUTED'
            decoded.append(args)
        with patch.object(collector, 'os', types.SimpleNamespace(name='nt')):
            with patch.object(collector, 'windows_arguments', side_effect=decoded):
                self.assertEqual(len(collector.collect(self.profile, self.database, self.exporter())['solver']['recipes']), 1)
            decoded[0].append('@unadmitted.rsp')
            with patch.object(collector, 'windows_arguments', side_effect=decoded):
                with self.assertRaisesRegex(ValueError, 'Response-file'):
                    collector.collect(self.profile, self.database, self.exporter())

    def test_unrelated_cmake_output_avoids_unnecessary_command_decoding(self):
        self.database.append({'output': 'CMakeFiles/Unrelated.dir/unit.obj', 'command': 'NOT_EXECUTED'})
        self.manifest()

    def test_missing_required_patch_or_license_is_rejected(self):
        (self.source / "cmake/patches/HoroJoltConstraintReservation.cmake").unlink()
        with self.assertRaisesRegex(ValueError, "Missing owning build input"):
            self.manifest()

    def test_ambiguous_native_target_recipe_is_rejected(self):
        self.database[0]["arguments"].append("CMakeFiles/Jolt.dir/collision.o")
        with self.assertRaisesRegex(ValueError, "Ambiguous"):
            self.manifest()



class WindowsArgvSelfcheckAdapterTests(unittest.TestCase):
    """Synthetic adapter contracts exercise the wrapper independently of native CRT qualification."""

    def test_non_windows_host_is_rejected_before_the_adapter_is_called(self):
        with patch.object(checker, "os", types.SimpleNamespace(name="posix")):
            with patch.object(checker, "windows_arguments") as adapter:
                with self.assertRaisesRegex(ValueError, "requires Windows"):
                    checker.check(Path("unused-exporter"))
                adapter.assert_not_called()

    def test_quote_and_unicode_oracle_vectors_reach_the_exact_selected_exporter(self):
        compiler = r"C:\nonexistent compiler\not-executed.exe"
        vectors = [
            (r'"a b c" d e', ['a b c', 'd', 'e']),
            (r'"ab\"c" "\\" d', ['ab"c', '\\', 'd']),
            (r'a\\\b d"e f"g h', [r'a\\\b', 'de fg', 'h']),
            (r'a\\\"b c d', ['a\\"b', 'c', 'd']),
            (r'a\\\\"b c" d e', [r'a\\b c', 'd', 'e']),
            (r'a"b"" c d', ['ab" c d']),
            ('"" "quoted  空間 🧪"', ['', 'quoted  空間 🧪']),
        ]
        expected = dict(vectors)
        calls = []
        with tempfile.TemporaryDirectory() as directory:
            exporter = Path(directory) / "selected exporter 空間 🧪.exe"
            exporter.write_bytes(b"SYNTHETIC_ADAPTER_INPUT")

            def decode(command, selected, cwd):
                self.assertEqual(selected, exporter)
                self.assertEqual(cwd, str(exporter.parent))
                prefix = '"' + compiler + '" '
                self.assertTrue(command.startswith(prefix))
                tail = command[len(prefix):]
                self.assertIn(tail, expected)
                calls.append(tail)
                return [compiler, *expected[tail]]

            with patch.object(checker, "os", types.SimpleNamespace(name="nt")):
                with patch.object(checker, "windows_arguments", side_effect=decode):
                    checker.check(exporter)
            self.assertEqual(calls, [tail for tail, _ in vectors])
            self.assertEqual(exporter.read_bytes(), b"SYNTHETIC_ADAPTER_INPUT")

    def test_first_oracle_mismatch_stops_the_remaining_vectors(self):
        with patch.object(checker, "os", types.SimpleNamespace(name="nt")):
            with patch.object(checker, "windows_arguments", return_value=[]) as adapter:
                with self.assertRaisesRegex(ValueError, "CRT argv selfcheck mismatch"):
                    checker.check(Path("selected-exporter"))
                self.assertEqual(adapter.call_count, 1)

    def test_adapter_failure_propagates_without_attempting_other_vectors(self):
        with patch.object(checker, "os", types.SimpleNamespace(name="nt")):
            with patch.object(checker, "windows_arguments", side_effect=OSError("synthetic exporter failure")) as adapter:
                with self.assertRaisesRegex(OSError, "synthetic exporter failure"):
                    checker.check(Path("selected-exporter"))
                self.assertEqual(adapter.call_count, 1)

    def test_cli_forwards_one_explicit_unicode_exporter_path(self):
        with tempfile.TemporaryDirectory() as directory:
            exporter = Path(directory) / "argv oracle 空間 🧪.exe"
            with patch.object(sys, "argv", ["check_physics_windows_argv.py", "--exporter", str(exporter)]):
                with patch.object(checker, "check") as check:
                    checker.main()
                    check.assert_called_once_with(exporter)

    def test_cli_requires_exporter_before_invoking_the_selfcheck(self):
        with patch.object(sys, "argv", ["check_physics_windows_argv.py"]):
            with patch.object(checker, "check") as check, patch("sys.stderr", new=io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    checker.main()
                self.assertEqual(error.exception.code, 2)
                check.assert_not_called()


if __name__ == "__main__":
    unittest.main()
