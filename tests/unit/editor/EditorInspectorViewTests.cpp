#include "EditorInspectorControllerSupport.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/modals/extension_pages/EditorPageThemeCapture.h"
#include "editor/screens/workspace/panels/inspector/InspectorContributionView.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <imgui.h>
#include <limits>

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

        /** @brief Keeps authored Lua input and exact expected scalar together for real control gestures. */
        struct ScalarControlProfile final {
            Extensions::EditorInspectorFieldKind kind;
            std::string_view luaDefault;
            Gameplay::BehaviorFieldValue expected;
        };

        /** @brief Actual last control geometry and inert window background; test gestures do not depend on hard-coded widget offsets. */
        struct Geometry final {
            ImVec2 center;
            ImVec2 toggle;
            ImVec2 dismiss;
            std::size_t covered{};
        };

        /** @brief Renders the real shared-control projection using current owner data and returns measured interaction bounds. */
        Geometry Frame(InspectorContributionView &view, const InspectorControllerSupport::Fixture &owner, const Gui &gui,
                       EditorWorkspaceViewCommandData &command, float width = 640, const EditorWorkspaceViewModel *snapshot = nullptr) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({width, 440});
            ImGui::Begin("Inspector fixture", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
            const auto &model = snapshot ? *snapshot : owner.controller->ViewModel();
            view.Synchronize(model, model.selectedObjects);
            if (model.selectedObjects.size() > 1)
                view.DrawMulti(model, command, gui.context);
            else
                view.Draw(owner.attachment, model, command, gui.context);
            const auto minimum = ImGui::GetItemRectMin();
            const auto maximum = ImGui::GetItemRectMax();
            const auto window = ImGui::GetWindowPos();
            const auto size = ImGui::GetWindowSize();
            const Geometry result{{(minimum.x + maximum.x) / 2, (minimum.y + maximum.y) / 2},
                                  {(window.x + ImGui::GetWindowContentRegionMin().x + minimum.x) / 2, (minimum.y + maximum.y) / 2},
                                  {window.x + size.x - 8, window.y + size.y - 8},
                                  view.CoveredFields(owner.attachment, gui.context).size()};
            ImGui::End();
            ImGui::Render();
            return result;
        }

        /** @brief Sends a complete actual button gesture through the same GUI frame owner, without manufacturing an approval. */
        void Click(ImVec2 point, InspectorContributionView &view, const InspectorControllerSupport::Fixture &owner, const Gui &gui,
                   EditorWorkspaceViewCommandData &command, const EditorWorkspaceViewModel *snapshot = nullptr) {
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent(point.x, point.y);
            io.AddMouseButtonEvent(0, true);
            Frame(view, owner, gui, command, 640, snapshot);
            io.AddMouseButtonEvent(0, false);
            Frame(view, owner, gui, command, 640, snapshot);
        }

        /** @brief Reads the actual canonical numeric owner value for GUI/history assertions. */
        double ScalarValue(const InspectorControllerSupport::Fixture &owner) {
            return std::get<double>(owner.controller->ViewModel().objects.front().components.behaviors.front().fields.front().value);
        }

        /** @brief Uses the existing controller's history commands after a custom scalar commit. */
        void CheckScalarHistory(InspectorControllerSupport::Fixture &owner, double proposed) {
            EditorWorkspaceViewCommandData command;
            command.command = EditorWorkspaceViewCommand::UndoScene;
            owner.Process(command);
            CHECK(ScalarValue(owner) == 25.0);
            command.command = EditorWorkspaceViewCommand::RedoScene;
            owner.Process(command);
            CHECK(ScalarValue(owner) == proposed);
        }

        /** @brief Declares a custom scalar presenter over the real inspector numeric field. */
        Extensions::EditorInspectorDefinition ScalarDefinition() {
            using namespace Extensions;
            auto definition = InspectorControllerSupport::Presentation(EditorInspectorFieldKind::Number);
            definition.fields.front().minimumNumber = 0.0;
            definition.fields.front().maximumNumber = 100.0;
            auto &number = std::get<EditorUiNumberNode>(definition.form.nodes.front().payload);
            number.minimum = 0.0;
            number.maximum = 100.0;
            number.base.custom = EditorUiCustomPresentation{.widget = EditorUiCustomWidget::ScalarDial,
                                                            .font = EditorThemeFontRole::SansEmphasis,
                                                            .icon = EditorThemeIconRole::Info};
            return definition;
        }

        /** @brief Withdraws real view input between pointer down and release; a preview cannot become a document command. */
        void CheckScalarPreviewCancellation(ImVec2 point, InspectorContributionView &view, const InspectorControllerSupport::Fixture &owner,
                                            const Gui &gui, EditorWorkspaceViewCommandData &command) {
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent(point.x, point.y);
            io.AddMouseButtonEvent(0, true);
            Frame(view, owner, gui, command);
            CHECK(command.command == EditorWorkspaceViewCommand::None);
            CHECK(view.Accessibility("fixture.inspector").front().preview);
            view.SetInputAdmitted(false);
            io.AddMouseButtonEvent(0, false);
            Frame(view, owner, gui, command);
            CHECK(command.command == EditorWorkspaceViewCommand::None);
            CHECK_FALSE(view.Accessibility("fixture.inspector").front().preview);
            view.SetInputAdmitted(true);
            Frame(view, owner, gui, command);
        }

        /** @brief A disabled contribution drops an active drag; reactivation cannot commit that abandoned gesture. */
        void CheckScalarProviderWithdrawal(ImVec2 point, InspectorContributionView &view, InspectorControllerSupport::Fixture &owner,
                                           const Gui &gui, EditorWorkspaceViewCommandData &command) {
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent(point.x, point.y);
            io.AddMouseButtonEvent(0, true);
            Frame(view, owner, gui, command);
            REQUIRE(owner.provider.registry
                        .SetProviderStatus({"fixture.package", "fixture.module"}, Extensions::EditorSurfaceProviderStatus::Disabled)
                        .HasValue());
            Frame(view, owner, gui, command);
            CHECK(view.Accessibility("fixture.inspector").empty());
            REQUIRE(owner.provider.registry
                        .SetProviderStatus({"fixture.package", "fixture.module"}, Extensions::EditorSurfaceProviderStatus::Active)
                        .HasValue());
            Frame(view, owner, gui, command);
            io.AddMouseButtonEvent(0, false);
            Frame(view, owner, gui, command);
            CHECK(command.command == EditorWorkspaceViewCommand::None);
            CHECK(ScalarValue(owner) == 25.0);
        }

        /** @brief A hidden contribution loses native pointer capture and cannot replay its abandoned preview on reopening. */
        void CheckScalarHiddenGesture(ImVec2 point, InspectorContributionView &view, const InspectorControllerSupport::Fixture &owner,
                                      const Gui &gui, EditorWorkspaceViewCommandData &command) {
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent(point.x, point.y);
            io.AddMouseButtonEvent(0, true);
            Frame(view, owner, gui, command);
            ImGui::NewFrame();
            ImGui::Render();
            io.AddMouseButtonEvent(0, false);
            Frame(view, owner, gui, command);
            CHECK(command.command == EditorWorkspaceViewCommand::None);
            CHECK(ScalarValue(owner) == 25.0);
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
        auto &io = ImGui::GetIO();
        io.ConfigMacOSXBehaviors = GENERATE(false, true);
        CAPTURE(io.ConfigMacOSXBehaviors);
        const auto selectAllModifier = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        InspectorContributionView view{owner.provider.host, *gui.capture.Frame()};
        EditorWorkspaceViewCommandData command;
        Frame(view, owner, gui, command);
        auto geometry = Frame(view, owner, gui, command);
        Click(geometry.center, view, owner, gui, command);
        io.AddKeyEvent(selectAllModifier, true);
        io.AddKeyEvent(ImGuiKey_A, true);
        Frame(view, owner, gui, command);
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(selectAllModifier, false);
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

    TEST_CASE("Inspector selection epochs discard active drafts without truncating high revision bits",
              "[editor][inspector][view][selection]") {
        const auto nextEpoch = GENERATE(std::uint64_t{2}, (std::uint64_t{1} << 32) + 1, std::numeric_limits<std::uint64_t>::max());
        CAPTURE(nextEpoch);
        InspectorControllerSupport::Fixture owner;
        owner.Approve();
        Gui gui;
        InspectorContributionView view{owner.provider.host, *gui.capture.Frame()};
        EditorWorkspaceViewCommandData command;
        auto model = owner.controller->ViewModel();
        model.selectionRevision = 1;
        Frame(view, owner, gui, command, 640, &model);
        auto geometry = Frame(view, owner, gui, command, 640, &model);
        Click(geometry.center, view, owner, gui, command, &model);
        auto &io = ImGui::GetIO();
        const auto modifier = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        io.AddKeyEvent(modifier, true);
        io.AddKeyEvent(ImGuiKey_A, true);
        Frame(view, owner, gui, command, 640, &model);
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(modifier, false);
        io.AddInputCharactersUTF8("77");
        Frame(view, owner, gui, command, 640, &model);
        REQUIRE(command.command == EditorWorkspaceViewCommand::None);
        model.selectionRevision = nextEpoch;
        geometry = Frame(view, owner, gui, command, 640, &model);
        Click(geometry.dismiss, view, owner, gui, command, &model);
        CHECK(command.command == EditorWorkspaceViewCommand::None);
        CHECK(owner.Value() == 9007199254740993LL);
        Click(geometry.center, view, owner, gui, command, &model);
        io.AddKeyEvent(modifier, true);
        io.AddKeyEvent(ImGuiKey_A, true);
        Frame(view, owner, gui, command, 640, &model);
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(modifier, false);
        io.AddInputCharactersUTF8("88");
        Frame(view, owner, gui, command, 640, &model);
        Click(geometry.dismiss, view, owner, gui, command, &model);
        REQUIRE(command.command == EditorWorkspaceViewCommand::UpdateInspectorBehaviorField);
        REQUIRE(command.inspectorEdit);
        CHECK(command.inspectorEdit->selectionRevision == nextEpoch);
        CHECK(std::get<std::int64_t>(command.inspectorEdit->value) == 88);
    }

    TEST_CASE("Actual inspector boolean decimal and UTF8 controls preserve exact owner values", "[editor][inspector][view][scalar]") {
        using enum Extensions::EditorInspectorFieldKind;
        const auto profile = GENERATE(ScalarControlProfile{Boolean, "true", false}, ScalarControlProfile{Number, "1.25", -0.125},
                                      ScalarControlProfile{String, "'başlangıç'", std::string{"son değer"}});
        const auto kind = profile.kind;
        CAPTURE(kind);
        InspectorControllerSupport::Fixture owner{InspectorControllerSupport::Presentation(kind), profile.luaDefault};
        owner.Approve();
        Gui gui;
        InspectorContributionView view{owner.provider.host, *gui.capture.Frame()};
        EditorWorkspaceViewCommandData command;
        Frame(view, owner, gui, command, 210);
        const auto geometry = Frame(view, owner, gui, command);
        REQUIRE(geometry.covered == 1);
        Click(kind == Boolean ? geometry.toggle : geometry.center, view, owner, gui, command);
        if (kind != Boolean) {
            auto &io = ImGui::GetIO();
            const auto modifier = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
            io.AddKeyEvent(modifier, true);
            io.AddKeyEvent(ImGuiKey_A, true);
            Frame(view, owner, gui, command);
            io.AddKeyEvent(ImGuiKey_A, false);
            io.AddKeyEvent(modifier, false);
            io.AddInputCharactersUTF8(kind == Number ? "-0.125" : "son değer");
            Frame(view, owner, gui, command);
            Click(geometry.dismiss, view, owner, gui, command);
        }
        REQUIRE(command.command == EditorWorkspaceViewCommand::UpdateInspectorBehaviorField);
        REQUIRE(command.inspectorEdit);
        CHECK(command.inspectorEdit->value == profile.expected);
        owner.Process(command);
        const auto &fields = owner.controller->ViewModel().objects.front().components.behaviors.front().fields;
        CHECK(fields.front().value == profile.expected);
        CHECK(fields.back().value == Gameplay::BehaviorFieldValue{std::string{"preserve"}});
    }

    TEST_CASE("Inspector malformed numeric gestures retain authored values without publishing edits",
              "[editor][inspector][view][validation]") {
        using enum Extensions::EditorInspectorFieldKind;
        const auto kind = GENERATE(Integer, Number);
        InspectorControllerSupport::Fixture owner{InspectorControllerSupport::Presentation(kind), kind == Integer ? "9" : "1.25"};
        owner.Approve();
        Gui gui;
        InspectorContributionView view{owner.provider.host, *gui.capture.Frame()};
        EditorWorkspaceViewCommandData command;
        Frame(view, owner, gui, command);
        const auto geometry = Frame(view, owner, gui, command);
        Click(geometry.center, view, owner, gui, command);
        auto &io = ImGui::GetIO();
        const auto modifier = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        io.AddKeyEvent(modifier, true);
        io.AddKeyEvent(ImGuiKey_A, true);
        Frame(view, owner, gui, command);
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(modifier, false);
        io.AddInputCharactersUTF8("invalid");
        Frame(view, owner, gui, command);
        Click(geometry.dismiss, view, owner, gui, command);
        CHECK(command.command == EditorWorkspaceViewCommand::None);
        const auto expected = kind == Integer ? Gameplay::BehaviorFieldValue{std::int64_t{9}} : Gameplay::BehaviorFieldValue{1.25};
        CHECK(owner.controller->ViewModel().objects.front().components.behaviors.front().fields.front().value == expected);
        CHECK(Frame(view, owner, gui, command).covered == 1);
    }

    TEST_CASE("Inspector multi-selection projection rejects ambiguous missing and stale attachments",
              "[editor][inspector][view][projection]") {
        InspectorControllerSupport::Fixture owner;
        owner.Approve();
        Gui gui;
        InspectorContributionView view{owner.provider.host, *gui.capture.Frame()};
        EditorWorkspaceViewCommandData command;
        auto model = owner.controller->ViewModel();
        auto second = model.objects.front();
        second.id = {987654};
        second.components.behaviors.front().instanceId = {987655};
        second.components.behaviors.front().fields.front().value = std::int64_t{7};
        model.objects.push_back(second);
        model.selectedObjects.push_back(second.id);
        ++model.selectionRevision;
        CHECK(Frame(view, owner, gui, command, 210, &model).covered == 1);
        CHECK(command.command == EditorWorkspaceViewCommand::None);
        SECTION("Repeated schema attachment is ambiguous") {
            auto duplicate = second.components.behaviors.front();
            duplicate.instanceId = {987656};
            model.objects.back().components.behaviors.push_back(duplicate);
        }
        SECTION("Missing declared field keeps generic presentation") {
            model.objects.back().components.behaviors.front().fields.erase(
                model.objects.back().components.behaviors.front().fields.begin());
        }
        SECTION("Stale schema keeps generic presentation") {
            ++model.objects.back().components.behaviors.front().schemaVersion;
        }
        SECTION("Wrong authored kind keeps generic presentation") {
            model.objects.back().components.behaviors.front().fields.front().value = std::string{"wrong kind"};
        }
        ++model.documentRevision.value;
        CHECK(Frame(view, owner, gui, command, 210, &model).covered == 0);
        CHECK(command.command == EditorWorkspaceViewCommand::None);
        CHECK(owner.Value() == 9007199254740993LL);
    }

    TEST_CASE("Inspector passive nested presentation survives width changes and loses coverage with an invalid theme",
              "[editor][inspector][view][theme]") {
        using namespace Extensions;
        auto definition = Extensions::Tests::InspectorSupport::Definition();
        std::get<EditorUiNumberNode>(definition.form.nodes.front().payload).base.parent = {"root"};
        definition.form.nodes.emplace(definition.form.nodes.begin(),
                                      EditorUiContainerNode{.base = {.id = {"root"}, .focusPolicy = EditorUiFocusPolicy::Never}});
        definition.form.nodes.emplace_back(EditorUiLabelNode{.base = {.id = {"caption"},
                                                                      .parent = {"root"},
                                                                      .description = {EditorUiTextKind::TechnicalText, "Read-only context"},
                                                                      .focusPolicy = EditorUiFocusPolicy::Never},
                                                             .text = {EditorUiTextKind::TechnicalText, "Exact typed value"}});
        InspectorControllerSupport::Fixture owner{std::move(definition)};
        owner.Approve();
        Gui gui;
        auto theme = *gui.capture.Frame();
        InspectorContributionView view{owner.provider.host, theme};
        EditorWorkspaceViewCommandData command;
        CHECK(Frame(view, owner, gui, command, 210).covered == 1);
        CHECK(Frame(view, owner, gui, command, 640).covered == 1);
        CHECK(command.command == EditorWorkspaceViewCommand::None);
        theme.revision = 0;
        CHECK(Frame(view, owner, gui, command).covered == 0);
        CHECK(owner.Value() == 9007199254740993LL);
    }

    TEST_CASE("Custom scalar native gesture uses the approved document transaction with undo redo and retirement",
              "[editor][inspector][composition][undo]") {
        using namespace Extensions;
        InspectorControllerSupport::Fixture owner{ScalarDefinition(), "25.0"};
        owner.Approve();
        Gui gui;
        InspectorContributionView view{owner.provider.host, *gui.capture.Frame()};
        EditorWorkspaceViewCommandData command;
        Frame(view, owner, gui, command);
        const auto geometry = Frame(view, owner, gui, command);
        const auto semantic = view.Accessibility("fixture.inspector");
        REQUIRE(semantic.size() == 1);
        CHECK(semantic.front().role == EditorUiAccessibleRole::ScalarDial);
        CHECK(ScalarValue(owner) == 25.0);
        const auto before = owner.controller->ViewModel().documentRevision;
        CheckScalarPreviewCancellation(geometry.center, view, owner, gui, command);
        CheckScalarProviderWithdrawal(geometry.center, view, owner, gui, command);
        CheckScalarHiddenGesture(geometry.center, view, owner, gui, command);
        Click(geometry.center, view, owner, gui, command);
        REQUIRE(command.command == EditorWorkspaceViewCommand::UpdateInspectorBehaviorField);
        REQUIRE(command.inspectorEdit);
        const auto proposed = std::get<double>(command.inspectorEdit->value);
        CHECK(proposed > 40.0);
        CHECK(proposed < 60.0);
        CHECK(ScalarValue(owner) == 25.0);
        owner.Process(command);
        CHECK(ScalarValue(owner) == proposed);
        CHECK(owner.controller->ViewModel().documentRevision.value == before.value + 1);
        CheckScalarHistory(owner, proposed);
        auto revoked = owner.Request();
        revoked.value = proposed + 0.1;
        owner.provider.admission->Revoke();
        command.command = EditorWorkspaceViewCommand::UpdateInspectorBehaviorField;
        command.inspectorEdit = std::move(revoked);
        owner.Process(command);
        CHECK(ScalarValue(owner) == proposed);
        owner.provider.registration->Reset();
        command = {};
        Frame(view, owner, gui, command);
        CHECK(view.Accessibility("fixture.inspector").empty());
        CHECK(command.command == EditorWorkspaceViewCommand::None);
    }

}  // namespace Horo::Editor::Tests
