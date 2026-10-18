#pragma once
/** @file NavigationSettingsPresentationFixture.h
 * @brief Real project/session presentation with queued ImGui keyboard gestures on existing shared controls.
 */
#include "HeadlessEditorGuiFixture.h"
#include "editor/modals/settings/NavigationSettingsDraft.h"
#include "support/NavigationSettingsProjectTestSupport.h"

#include <imgui_internal.h>

namespace Horo::Editor::Tests {
    /** @brief Owns every borrowed presentation dependency through the final frame. */
    struct NavigationSettingsPresentationFixture final {
        Horo::Tests::NavigationSettingsSupport::SessionProjectFixture project;
        EditorGuiContextFixture gui{{1200, 2400}};
        std::unique_ptr<NavigationProjectSettingsSession> session;
        NavigationDefinitionEditorState document;
        SettingsState settings;
        ImVec2 size{1100, 2300};

        explicit NavigationSettingsPresentationFixture(const std::span<const Application::NavigationTargetProviderEvidence> targets = {}) {
            const auto catalog = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/en-US.json";
            LocalizationError error;
            REQUIRE(gui.localization.LoadCatalogFile(catalog, &error));
            auto opened = project.Open(7, targets);
            REQUIRE(opened.HasValue());
            session = std::move(opened).Value();
            document.session = session->Borrow();
            document.draft = std::make_shared<NavigationSettingsDraft>();
            settings.navigationSettings = session->Borrow();
        }

        /** @brief Draws the real controls and queues a keyboard activation for the next frame, never invoking a command directly. */
        template <class Draw> void Frame(Draw draw, const std::string_view action = {}) {
            gui.imgui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            ImGui::Begin("NavigationPresentation", nullptr, ImGuiWindowFlags_NoSavedSettings);
            const auto label = action.starts_with("settings.") ? gui.localization.Get("editor", action) : std::string{action};
            const auto id = ImGui::GetID(label.c_str());
            draw();
            if (!action.empty())
                ImGui::ActivateItemByID(id);
            ImGui::End();
            gui.imgui.EndFrame();
            REQUIRE(ImGui::GetDrawData() != nullptr);
            REQUIRE(ImGui::GetDrawData()->TotalVtxCount > 0);
        }

        void Definition(const std::string_view action = {}) {
            const auto draw = [this] {
                DrawNavigationDefinitionEditor(document, gui.context, Horo::Tests::NavigationSettingsSupport::Asset(1));
            };
            Frame(draw, action);
            if (!action.empty())
                Frame(draw);
        }

        void Policy(const std::string_view action = {}) {
            const auto draw = [this] {
                SettingsModalInternal::DrawNavigationSettings(settings, gui.context);
            };
            Frame(draw, action);
            if (!action.empty())
                Frame(draw);
        }

        [[nodiscard]] NavigationDefinitionSnapshot Current() const {
            const auto definitions = session->Definitions();
            REQUIRE(definitions.HasValue());
            REQUIRE(definitions.Value().size() == 1);
            return definitions.Value().front();
        }
    };
}  // namespace Horo::Editor::Tests
