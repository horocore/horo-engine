#include "EditorUiTestHarness.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <imgui_internal.h>
#include <imgui_test_engine/imgui_te_context.h>
#include <optional>
#include <string>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Drives the actual Inspector and records its typed commands without bypassing its reducer. */
    struct CharacterInspectorUi final {
        Tests::EditorUiTestHarness harness{Tests::CreateHeadlessEditorUiTestSurface()};
        EngineDataBus engineEvents;
        EditorDataBus events;
        LocalizationService localization{LocaleTag{"tr-TR"}};
        Theme::Fonts fonts{ImGui::GetIO().FontDefault, ImGui::GetIO().FontDefault, ImGui::GetIO().FontDefault, ImGui::GetIO().FontDefault};
        ThemeContext theme{fonts};
        EditorSettingsSnapshot settings;
        EditorGuiContext context{engineEvents, events, localization, theme, settings};
        EditorWorkspaceViewModel model;
        InspectorPanel panel;
        std::optional<EditorWorkspaceViewCommandData> command;
        int characterVertices{};
        float capturedWidth{};

        CharacterInspectorUi() {
            LocalizationError error;
            REQUIRE(localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/tr-TR.json",
                                                 &error));
            REQUIRE(localization.Prepare(LocaleTag{"tr-TR"}, &error));
            REQUIRE(localization.ActivatePrepared(&error));
            SceneObject object{.id = SceneObjectId{17}, .name = "Karakter denetleyici yapılandırması"};
            object.components.characterController =
                Runtime::CharacterControllerComponent{.id = {19},
                                                      .enabled = false,
                                                      .movement = {.skinWidthMeters = 0.1F, .maximumContacts = 8}};
            model.objects.push_back(object);
            model.primarySelection = object.id;
            model.selectedObjects = {object.id};
            model.documentRevision = DocumentRevision{1};
            ImGui::GetIO().DisplaySize = {420, 900};
        }

        void Frame() {
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({420, 900});
            ImGui::Begin("Character inspector", nullptr,
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
            EditorWorkspaceViewCommandData current;
            panel.DrawPanel(ImGui::GetCursorScreenPos(), {400, 860}, model, current, context);
            if (current.command != EditorWorkspaceViewCommand::None)
                command = std::move(current);
            ImGui::End();
            // Capture the Character card itself while its frame is active, not drained post-run draw data.
            for (const ImGuiWindow *window : ImGui::GetCurrentContext()->Windows) {
                if (std::string_view{window->Name}.find("##CharacterControllerCard") == std::string_view::npos || !window->Active ||
                    window->Hidden)
                    continue;
                characterVertices = window->DrawList->VtxBuffer.Size;
                capturedWidth = ImGui::GetMainViewport()->Size.x;
            }
        }
    };

    TEST_CASE("Narrow Turkish Character Inspector reset preserves disabled ownership and emits one typed replacement",
              "[ui][imgui][editor][character]") {
        CharacterInspectorUi fixture;
        const auto original = fixture.model.objects.front().components.characterController;
        const auto result = fixture.harness.Run("horo_character", "localized_reset", [&](ImGuiTestContext *) {
            fixture.Frame();
        }, [&](ImGuiTestContext *ui) {
            ui->Yield(3);
            IM_CHECK(!fixture.command.has_value());
            ui->ItemClick("//**/##CharacterControllerCard/**/reset/##card_action");
            ui->Yield(2);
            IM_CHECK(fixture.command.has_value());
        });
        INFO(result.testEngineLog);
        REQUIRE(result.Succeeded());
        CHECK_FALSE(result.frameBudgetExceeded);
        CHECK(fixture.characterVertices > 0);
        CHECK(fixture.capturedWidth == 420.0F);
        REQUIRE(fixture.command.has_value());
        CHECK(fixture.command->command == EditorWorkspaceViewCommand::UpdateCharacterComponent);
        CHECK(fixture.command->objectPayload == SceneObjectId{17});
        REQUIRE(fixture.command->characterPayload.has_value());
        const auto &reset = *fixture.command->characterPayload;
        CHECK(reset.id == original->id);
        CHECK_FALSE(reset.enabled);
        CHECK(reset.geometry == Runtime::CharacterSceneGeometry{});
        CHECK(reset.movement == Runtime::CharacterSceneMovement{});
        CHECK(reset.collision == original->collision);
        CHECK(fixture.model.objects.front().components.characterController == original);
    }
}  // namespace
