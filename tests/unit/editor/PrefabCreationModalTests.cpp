#include "../../helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "editor/modals/prefab_creation/PrefabCreationModal.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <imgui_internal.h>

namespace Horo::Editor {
    TEST_CASE("Prefab creation modal renders localized narrow drafts and cancels without invoking its transaction",
              "[editor][prefab][creation][render]") {
        const auto locale = GENERATE("en-US", "tr-TR");
        const auto width = GENERATE(320.0F, 760.0F);
        Tests::EditorGuiContextFixture fixture{{width, 620}};
        for (const auto *catalog : {"en-US", "tr-TR"})
            REQUIRE(
                fixture.localization.LoadCatalogFile(std::filesystem::path{HORO_TEST_LOCALIZATION_DIR} / (std::string{catalog} + ".json")));
        REQUIRE(fixture.localization.Prepare(LocaleTag{locale}));
        REQUIRE(fixture.localization.ActivatePrepared());
        for (const auto *key :
             {"action", "title", "help", "path", "cancel", "confirm", "failed", "post_commit", "more_diagnostics", "expired"})
            CHECK_FALSE(fixture.localization.Get("editor", std::string{"workspace.prefab.create."} + key).starts_with("[missing:"));
        EditorModalHost host{fixture.editorEvents, fixture.input};
        std::size_t attempts{};
        auto opened = host.OpenRoot(std::make_unique<PrefabCreationModal>(fixture.context, PrefabCreationIntent{{11}, 1, 1}, "/project",
                                                                          [&](PrefabCreationIntent) -> std::optional<std::string> {
            ++attempts;
            return std::string{"Rejected"};
        }));
        REQUIRE(opened.HasValue());
        for (int frame = 0; frame != 2; ++frame) {
            fixture.imgui.BeginFrame();
            host.Draw();
            fixture.imgui.EndFrame();
        }
        const auto *draw = ImGui::GetDrawData();
        REQUIRE(draw);
        CHECK(draw->TotalVtxCount > 0);
        for (int index = 0; index != draw->CmdListsCount; ++index)
            for (const auto &vertex : draw->CmdLists[index]->VtxBuffer)
                CHECK((std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y)));
        CHECK(host.RequestCloseAllForShutdown().HasValue());
        CHECK(attempts == 0);
    }

    TEST_CASE("Prefab filename editing never submits before the explicit Create action", "[editor][prefab][creation][render]") {
        Tests::EditorGuiContextFixture fixture{{760, 620}};
        REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path{HORO_TEST_LOCALIZATION_DIR} / "en-US.json"));
        EditorModalHost host{fixture.editorEvents, fixture.input};
        std::size_t attempts{};
        REQUIRE(host.OpenRoot(std::make_unique<PrefabCreationModal>(fixture.context, PrefabCreationIntent{{11}, 1, 1}, "/project",
                                                                    [&](PrefabCreationIntent) -> std::optional<std::string> {
            ++attempts;
            return "Rejected confirmation";
        })).HasValue());
        const auto frame = [&] {
            fixture.imgui.BeginFrame();
            host.Draw();
            fixture.imgui.EndFrame();
        };
        frame();
        frame();
        const auto child = [](const std::string_view marker) -> ImGuiWindow * {
            const auto found = std::ranges::find_if(GImGui->Windows, [marker](const ImGuiWindow *window) {
                return window->ParentWindow && std::string_view{window->Name}.find(marker) != std::string_view::npos;
            });
            return found == GImGui->Windows.end() ? nullptr : *found;
        };
        auto *body = child("##CreatePrefabBody");
        REQUIRE(body);
        ImGui::ActivateItemByID(body->GetID("##PrefabDestination"));
        frame();
        REQUIRE(ImGui::GetInputTextState(GImGui->ActiveId));
        ImGui::GetIO().AddInputCharactersUTF8("x");
        frame();
        REQUIRE(ImGui::GetInputTextState(GImGui->ActiveId));
        CHECK(std::string_view{ImGui::GetInputTextState(GImGui->ActiveId)->TextA.Data}.find('x') != std::string_view::npos);
        CHECK(attempts == 0);
        auto *footer = child("##ModalFooter");
        REQUIRE(footer);
        ImGui::ActivateItemByID(footer->GetID(fixture.localization.Get("editor", "workspace.prefab.create.confirm").c_str()));
        frame();
        CHECK(attempts == 1);
        REQUIRE(host.RequestCloseAllForShutdown().HasValue());
    }
}  // namespace Horo::Editor
