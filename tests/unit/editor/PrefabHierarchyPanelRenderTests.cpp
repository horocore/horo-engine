#include "../../helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "editor/screens/workspace/panels/hierarchy/HierarchyPanel.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>

namespace Horo::Editor {
    namespace {
        /** @brief Supplies immutable presentation input only; source capture and resolver behavior have separate native coverage. */
        EditorWorkspaceViewModel Presentation() {
            const auto asset = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000001").Value();
            const auto instance = Prefab::PrefabInstanceId::Create(1).Value();
            const PrefabHierarchyTarget root{instance, std::nullopt};
            const PrefabHierarchyTarget member{instance, Prefab::ExpandedPrefabObjectKey{instance, {}}};
            auto sources = std::make_shared<SceneSource::ScenePrefabProjection>();
            sources->instances.push_back(
                {.authored = {.instanceId = instance, .sourcePrefab = Prefab::PrefabAssetReference::Create(asset).Value()}});
            auto projection = std::make_shared<PrefabHierarchyProjection>();
            projection->documentSession = asset;
            projection->publication = 1;
            projection->documentRevision = 1;
            projection->sources = sources;
            projection->entries = {{root, std::nullopt, asset, "Long localized Prefab instance ownership name", asset.ToString(), true},
                                   {member, root, asset, "A very long nested member name whose source remains protected", asset.ToString(),
                                    true}};
            EditorWorkspaceViewModel model;
            model.documentRevision = {1};
            model.prefabHierarchy = std::move(projection);
            model.prefabSelection = member;
            return model;
        }

        /** @brief Executes both actual panel renderers at a bounded narrow width without a GPU backend. */
        void DrawPanels(Tests::EditorGuiContextFixture &fixture, HierarchyPanel &hierarchy, InspectorPanel &inspector,
                        const EditorWorkspaceViewModel &model, const float width) {
            EditorWorkspaceViewCommandData command;
            fixture.imgui.BeginFrame();
            ImGui::SetNextWindowSize({width, 550});
            ImGui::Begin("Prefab ownership layout", nullptr, ImGuiWindowFlags_NoSavedSettings);
            hierarchy.DrawPanel(ImGui::GetCursorScreenPos(), {width - 20, 250}, model, command, fixture.context);
            inspector.DrawPanel(ImGui::GetCursorScreenPos(), {width - 20, 250}, model, command, fixture.context);
            ImGui::End();
            fixture.imgui.EndFrame();
            CHECK(command.command == EditorWorkspaceViewCommand::None);
            const auto *draw = ImGui::GetDrawData();
            REQUIRE(draw != nullptr);
            REQUIRE(draw->TotalVtxCount > 0);
            for (int index = 0; index < draw->CmdListsCount; ++index)
                for (const auto &vertex : draw->CmdLists[index]->VtxBuffer)
                    CHECK((std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y)));
        }
    }  // namespace

    TEST_CASE("Prefab ownership panels render localized narrow layouts and release cache pins on detach", "[editor][prefab][render]") {
        Tests::EditorGuiContextFixture fixture;
        const auto locale = GENERATE("en-US", "tr-TR");
        REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path(HORO_TEST_LOCALIZATION_DIR) / "en-US.json"));
        REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path(HORO_TEST_LOCALIZATION_DIR) / "tr-TR.json"));
        REQUIRE(fixture.localization.Prepare(LocaleTag{locale}));
        REQUIRE(fixture.localization.ActivatePrepared());
        auto model = Presentation();
        const std::weak_ptr<const PrefabHierarchyProjection> weak = model.prefabHierarchy;
        HierarchyPanel hierarchy;
        InspectorPanel inspector;
        for (const float width : {180.0F, 320.0F})
            DrawPanels(fixture, hierarchy, inspector, model, width);
        model.prefabHierarchy.reset();
        model.prefabSelection.reset();
        hierarchy.OnDetach();
        inspector.OnDetach();
        CHECK(weak.expired());
    }
}  // namespace Horo::Editor
