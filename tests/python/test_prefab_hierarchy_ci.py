"""Keep the Prefab hierarchy foundation covered in real headless and GUI compositions."""
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PrefabHierarchyCiTests(unittest.TestCase):
    def test_prefab_test_translation_units_have_exact_existing_target_membership(self):
        cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        expected = {
            "HoroSceneDocumentTests": (
                "PrefabDocumentSessionTests.cpp", "PrefabDocumentSessionByteBoundaryTests.cpp",
            ),
            "HoroEditorWorkspaceControllerTests": (
                "EditorWorkspaceControllerPrefabTests.cpp",
                "EditorWorkspaceControllerPrefabAssetDropTests.cpp",
                "EditorWorkspaceControllerPrefabAuthoringTests.cpp",
            ),
        }
        for target, names in expected.items():
            sources = cmake.split(f"add_executable({target}", 1)[1].split(")", 1)[0].split()
            for name in names:
                relative = f"unit/editor/{name}"
                self.assertEqual(sources.count(relative), 1, (target, name))
                self.assertEqual(cmake.split().count(relative), 1, name)
                self.assertTrue((ROOT / "tests" / relative).is_file(), name)

    def test_document_session_dirty_close_assertion_includes_declaring_header(self):
        path = ROOT / "tests/unit/editor/PrefabDocumentSessionByteBoundaryTests.cpp"
        self.assertTrue(path.is_file(), "byte-boundary tests must have a registered owning translation unit")
        source = path.read_text(encoding="utf-8")
        self.assertIn('#include "Horo/Editor/WorkspacePanelHost.h"', source)
        self.assertIn(
            "CHECK(dirty.ErrorValue().code.Value() == WorkspaceDocumentErrors::DirtyDocument.code.Value());",
            source,
        )

    def test_headless_sources_belong_to_real_owner_targets(self):
        source = (ROOT / "src/CMakeLists.txt").read_text(encoding="utf-8")
        model = source.split("add_library(HoroEditorModel STATIC", 1)[1].split(")", 1)[0]
        services = source.split("add_library(HoroEditorServices STATIC", 1)[1].split(")", 1)[0]
        self.assertIn("PrefabHierarchyProjection.cpp", model)
        self.assertIn("PrefabHierarchyCommand.cpp", model)
        self.assertIn("PrefabHierarchySources.cpp", services)
        tests = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        document = tests.split("add_executable(HoroSceneDocumentTests", 1)[1].split(")", 1)[0]
        for name in ("PrefabHierarchyProjectionTests.cpp", "PrefabHierarchySourcesTests.cpp"):
            self.assertIn(name, document)
        windows = (ROOT / "tests/cmake/HoroCiSuites.cmake").read_text(encoding="utf-8")
        windows = windows.split("set(HORO_CI_WINDOWS_TARGETS", 1)[1].split(")", 1)[0]
        for target in ("HoroSceneDocumentTests", "HoroAssetCookServiceTests", "HoroAssetsPublicHeaderConsumer"):
            self.assertIn(target, windows)

    def test_gui_routes_and_exported_sdk_keep_actual_implementation_coverage(self):
        presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        configure = {item["name"]: item for item in presets["configurePresets"]}
        self.assertEqual(configure["ci-native-debug"]["cacheVariables"]["HORO_BUILD_EDITOR_GUI"], "ON")
        for platform in ("ci-linux-debug", "ci-macos-debug"):
            self.assertEqual(configure[platform]["inherits"], "ci-native-debug")
        source = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        for name in ("EditorWorkspaceControllerPrefabTests.cpp", "PrefabHierarchyEditSessionTests.cpp",
                     "PrefabHierarchyPanelRenderTests.cpp", "AssetSelectedCapturePublicContract.cpp"):
            self.assertIn(name, source)
        module = (ROOT / "tests/fixtures/gameplay_physics_sdk/PhysicsModule.cpp").read_text(encoding="utf-8")
        self.assertIn("Assets::AssetCookInputSnapshot::CaptureSelected", module)

    def test_prefab_actions_have_both_localized_catalogs(self):
        keys = ("instance", "member", "open_source", "locate_instance", "protected", "overrides", "capacity")
        for language in ("en-US", "tr-TR"):
            catalog = json.loads((ROOT / f"assets/localization/editor/{language}.json").read_text(encoding="utf-8"))
            for key in keys:
                self.assertTrue(catalog["messages"][f"workspace.hierarchy.prefab.{key}"]["text"])

    def test_job_budgets_preserve_case_timeout(self):
        for name in ("ci.yml", "sonar.yml"):
            source = (ROOT / ".github/workflows" / name).read_text(encoding="utf-8")
            self.assertIn("timeout-minutes: 180", source)
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertIn("--no-tests=error --timeout 90", workflow)


if __name__ == "__main__":
    unittest.main()
