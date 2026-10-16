#include "EditorInspectorControllerSupport.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/modals/extension_pages/EditorPageThemeCapture.h"
#include "editor/screens/workspace/panels/inspector/InspectorContributionView.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

namespace Horo::Editor::Tests {
    namespace {
        /** @brief Owns the actual GUI/fonts/theme capture before every borrowed projection and control. */
        struct Gui final {
            struct Destroy {
                void operator()(ImGuiContext *value) const {
                    ImGui::DestroyContext(value);
                }
            };

            std::unique_ptr<ImGuiContext, Destroy> imgui{ImGui::CreateContext()};
            EngineDataBus engine;
            EditorDataBus editor;
            LocalizationService localization{LocaleTag{"tr-TR"}};
            Theme::Fonts fonts;
            ThemeContext theme{fonts};
            EditorSettingsSnapshot settings{.settings = DefaultEditorSettings()};
            EditorGuiContext context{engine, editor, localization, theme, settings};
            EditorPageThemeCapture capture;

            Gui() {
                auto &io = ImGui::GetIO();
                io.DisplaySize = {640, 480};
                io.DeltaTime = 1.0F / 60.0F;
                io.IniFilename = nullptr;
                io.Fonts->AddFontDefault();
                static_cast<void>(io.Fonts->Build());
                auto *font = io.Fonts->Fonts.front();
                fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font, .icon = font};
                settings.settings.languageTag = "tr-TR";
                Theme::ApplyCurrentTheme();
                REQUIRE(capture.Update(context).HasValue());
            }
        };

        /** @brief Actual last control geometry and inert window background; test gestures do not depend on hard-coded widget offsets. */
        struct Geometry final {
            ImVec2 center;
            ImVec2 dismiss;
            std::size_t covered{};
        };

        /** @brief Renders the real shared-control projection using current owner data and returns measured interaction bounds. */
        Geometry Frame(InspectorContributionView &view, InspectorControllerSupport::Fixture &owner, Gui &gui,
                       EditorWorkspaceViewCommandData &command, float width = 640) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({width, 440});
            ImGui::Begin("Inspector fixture", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
            const auto &model = owner.controller->ViewModel();
            view.Synchronize(model, model.selectedObjects);
            view.Draw(owner.attachment, model, command, gui.context);
            const auto minimum = ImGui::GetItemRectMin();
            const auto maximum = ImGui::GetItemRectMax();
            const auto window = ImGui::GetWindowPos();
            const auto size = ImGui::GetWindowSize();
            const Geometry result{{(minimum.x + maximum.x) / 2, (minimum.y + maximum.y) / 2},
                                  {window.x + size.x - 8, window.y + size.y - 8},
                                  view.CoveredFields(owner.attachment, gui.context).size()};
            ImGui::End();
            ImGui::Render();
            return result;
        }

        /** @brief Sends a complete actual button gesture through the same GUI frame owner, without manufacturing an approval. */
        void Click(ImVec2 point, InspectorContributionView &view, InspectorControllerSupport::Fixture &owner, Gui &gui,
                   EditorWorkspaceViewCommandData &command) {
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent(point.x, point.y);
            io.AddMouseButtonEvent(0, true);
            Frame(view, owner, gui, command);
            io.AddMouseButtonEvent(0, false);
            Frame(view, owner, gui, command);
        }
    }  // namespace

    TEST_CASE("Actual inspector button admits exact owner approval before custom controls replace generic fields",
              "[editor][inspector][view][authority]") {
        InspectorControllerSupport::Fixture owner;
        Gui gui;
        InspectorContributionView view{owner.provider.host, *gui.capture.Frame()};
        EditorWorkspaceViewCommandData command;
        Frame(view, owner, gui, command, 210);
        auto geometry = Frame(view, owner, gui, command);
        CHECK(geometry.covered == 0);
        CHECK(owner.controller->ViewModel().inspectorApprovals.empty());
        CHECK(owner.Value() == 9007199254740993LL);
        Click(geometry.center, view, owner, gui, command);
        REQUIRE(command.command == EditorWorkspaceViewCommand::ApproveInspectorContribution);
        owner.Process(command);
        command = {};
        geometry = Frame(view, owner, gui, command);
        REQUIRE(geometry.covered == 1);
        CHECK(owner.controller->ViewModel().inspectorApprovals.size() == 1);
        CHECK(owner.Value() == 9007199254740993LL);
        owner.provider.registration->Reset();
        geometry = Frame(view, owner, gui, command, 210);
        CHECK(geometry.covered == 0);
        CHECK(owner.Value() == 9007199254740993LL);
        CHECK(command.command == EditorWorkspaceViewCommand::None);
    }

    TEST_CASE("Actual inspector exact integer gesture reaches one existing transaction and undo owner", "[editor][inspector][view][undo]") {
        InspectorControllerSupport::Fixture owner;
        owner.Approve();
        REQUIRE(owner.controller->ViewModel().inspectorApprovals.size() == 1);
        Gui gui;
        InspectorContributionView view{owner.provider.host, *gui.capture.Frame()};
        EditorWorkspaceViewCommandData command;
        Frame(view, owner, gui, command);
        auto geometry = Frame(view, owner, gui, command);
        Click(geometry.center, view, owner, gui, command);
        auto &io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_A, true);
        Frame(view, owner, gui, command);
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        io.AddInputCharactersUTF8("-9223372036854775808");
        geometry = Frame(view, owner, gui, command);
        CHECK(command.command == EditorWorkspaceViewCommand::None);
        Click(geometry.dismiss, view, owner, gui, command);
        REQUIRE(command.command == EditorWorkspaceViewCommand::UpdateInspectorBehaviorField);
        REQUIRE(command.inspectorEdit);
        CHECK(std::get<std::int64_t>(command.inspectorEdit->value) == INT64_MIN);
        CHECK(owner.Value() == 9007199254740993LL);
        const auto before = owner.controller->ViewModel().documentRevision;
        owner.Process(command);
        CHECK(owner.Value() == INT64_MIN);
        CHECK(owner.controller->ViewModel().documentRevision.value == before.value + 1);
        command = {};
        command.command = EditorWorkspaceViewCommand::UndoScene;
        owner.Process(command);
        CHECK(owner.Value() == 9007199254740993LL);
    }
}  // namespace Horo::Editor::Tests
