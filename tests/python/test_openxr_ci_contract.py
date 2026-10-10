"""Static CI qualification contracts; never configure or compile a native target."""

import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


class OpenXRCIContractTests(unittest.TestCase):
    def setUp(self):
        self.presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        self.workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")

    def preset(self, kind, name):
        return next(value for value in self.presets[kind] if value["name"] == name)

    def variables(self, name):
        preset = self.preset("configurePresets", name)
        parent = preset.get("inherits")
        values = self.variables(parent) if parent else {}
        return values | preset.get("cacheVariables", {})

    def test_enabled_qualification_builds_all_native_and_boundary_targets(self):
        values = self.variables("ci-xr-openxr")
        self.assertEqual(values["HORO_BUILD_XR_OPENXR"], "ON")
        self.assertEqual(values["BUILD_TESTING"], "ON")
        for option in ("HORO_BUILD_EDITOR_GUI", "HORO_BUILD_INPUT_SDL3", "HORO_BUILD_RENDER_OPENGL",
                       "HORO_BUILD_RENDER_METAL", "HORO_BUILD_PHYSICS_NATIVE",
                       "HORO_BUILD_NAVIGATION_RECAST_DETOUR", "HORO_BUILD_NETWORK_GNS"):
            self.assertEqual(values[option], "OFF")
        build = self.preset("buildPresets", "ci-xr-openxr")
        self.assertEqual(set(build["targets"]), {
            "HoroXROpenXRTests", "HoroXROpenXRHostInterfaceConsumer", "HoroXRApiTests",
            "HoroXRRuntimeTests", "HoroXRApiPublicHeaderConsumer", "HoroXRRuntimePublicHeaderConsumer",
            "HoroXRActionBindingTests", "HoroXRInputBindingsPublicHeaderConsumer",
        })
        self.assertEqual(build["jobs"], 2)

    def test_case_inventory_is_selected_serially_without_empty_success(self):
        test = self.preset("testPresets", "ci-xr-openxr")
        selection = test["filter"]["include"]["name"]
        for target in ("HoroXROpenXRTests", "HoroXRApiTests", "HoroXRRuntimeTests", "HoroXRActionBindingTests"):
            self.assertRegex(f"{target}::case", selection)
        self.assertEqual(test["execution"]["jobs"], 1)
        self.assertEqual(self.preset("testPresets", "ci-test-base")["execution"]["noTestsAction"], "error")
        self.assertEqual(self.variables("sonar")["HORO_BUILD_XR_OPENXR"], "ON")

    def test_off_composition_stays_off_and_checks_real_target_population(self):
        values = self.variables("ci-xr-disabled")
        self.assertEqual(values["HORO_BUILD_XR_OPENXR"], "OFF")
        self.assertEqual(values["HORO_VERIFY_XR_DISABLED"], "ON")
        self.assertEqual(values["BUILD_TESTING"], "OFF")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(cmake, r'option\(HORO_BUILD_XR_OPENXR\s+"[^"]+"\s+OFF\)')
        policy = (ROOT / "cmake/HoroDependencyPolicy.cmake").read_text(encoding="utf-8")
        self.assertIn("foreach(target HoroXROpenXR HoroXROpenXRHostInterface HoroOpenXRHeaders)", policy)
        self.assertIn("FetchContent_GetProperties(horo_openxr_sdk POPULATED", policy)
        for name in ("ci-linux-debug", "ci-macos-debug", "ci-windows-debug"):
            self.assertNotIn("HORO_BUILD_XR_OPENXR", self.variables(name))

    def test_existing_three_platform_matrix_runs_blocking_qualification_and_reports(self):
        for platform in ("Linux / GCC", "macOS / Clang", "Windows / MSVC"):
            self.assertIn(f"name: {platform}", self.workflow)
        for name, command in (
            ("Configure OpenXR native lifecycle qualification", "cmake --preset ci-xr-openxr"),
            ("Build OpenXR lifecycle and header boundaries", "cmake --build --preset ci-xr-openxr"),
            ("Verify OpenXR omitted target graph", "cmake --preset ci-xr-disabled"),
            ("Test OpenXR rollback replacement and shutdown", "ctest --preset ci-xr-openxr"),
        ):
            step = re.search(rf"      - name: {re.escape(name)}\n(.*?)(?=\n      - name:|\Z)", self.workflow, re.S)
            self.assertIsNotNone(step)
            body = step.group(1)
            self.assertIn(command, body)
            self.assertIn("!cancelled()", body)
            self.assertNotIn("continue-on-error", body)
        self.assertIn("steps.xr_build.outcome == 'success'", self.workflow)
        self.assertIn("build/ci*/ctest.xml", self.workflow)


if __name__ == "__main__":
    unittest.main()
