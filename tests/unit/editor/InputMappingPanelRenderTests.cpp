#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/input_mapping/InputMappingPanel.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "support/OwnedTestDirectory.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdlib>
#include <imgui_internal.h>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct MappingHome {
        Horo::Tests::OwnedTestDirectory directory{"input-mapping"};
#if defined(_WIN32)
        static constexpr auto variable = "USERPROFILE";
#else
        static constexpr auto variable = "HOME";
#endif
        std::optional<std::string> previous;

        MappingHome() {
            if (const auto value = std::getenv(variable))
                previous = value;
            Set(directory.Path().string());
        }

        ~MappingHome() {
            Set(previous.value_or(""));
#if !defined(_WIN32)
            if (!previous)
                unsetenv(variable);
#endif
        }

        static void Set(const std::string &value) {
#if defined(_WIN32)
            _putenv_s(variable, value.c_str());
#else
            setenv(variable, value.c_str(), 1);
#endif
        }
    };

    struct MappingFixture {
        MappingHome home;
        Editor::Tests::EditorGuiContextFixture gui;
        InputMappingPanel panel;
        EditorWorkspaceViewModel model;
        EditorWorkspaceViewCommandData command;
        Input::InputContextToken workspace;
        Input::RawInputCollector collector;
        Input::GamepadDeviceId pad;
        ImVec2 pageOrigin;

        MappingFixture() {
            ImGui::GetIO().IniFilename = nullptr;
            for (const auto *locale : {"en-US", "tr-TR"})
                REQUIRE(gui.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                         (std::string{locale} + ".json")));
            Locale("en-US");
            gui.context.inputRouter = &gui.input;
            const Input::InputContextId id{"workspace"};
            REQUIRE(gui.input
                        .SetActionMap({{Input::ActionId{std::string(160, 'a')},
                                        Input::ActionValueType::Axis1D,
                                        id,
                                        true,
                                        {{.kind = Input::BindingControlKind::GamepadAxis,
                                          .deadzoneKind = Input::DeadzoneKind::Axial,
                                          .deadzone = 0.15F}}}})
                        .HasValue());
            workspace = gui.input.PushContext(id, Input::InputContextKind::EditorWorkspace);
            collector.BeginFrame(1);
            pad = collector.ConnectGamepad(std::string(160, 'b'));
            gui.input.BeginFrame(collector.Commit());
            PanelContext context{.dataBus = gui.editorEvents, .inputRouter = &gui.input, .workspaceInputContext = &workspace};
            panel.OnAttach(context);
        }

        ~MappingFixture() {
            panel.OnDetach();
            Theme::SetThemePreset(Theme::Preset::HoroDark);
            Theme::ApplyCurrentTheme();
        }

        void Locale(const char *locale) {
            REQUIRE(gui.localization.Prepare(LocaleTag{locale}));
            REQUIRE(gui.localization.ActivatePrepared());
        }

        void Draw(const float width, const bool disabled = false) {
            ImGui::GetIO().DisplaySize = {width, 800};
            gui.imgui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({width, 700});
            ImGui::Begin("Input Mapping render", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            ImGui::BeginDisabled(disabled);
            pageOrigin = ImGui::GetCursorScreenPos();
            panel.DrawPanel(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail(), model, command, gui.context);
            ImGui::EndDisabled();
            ImGui::End();
            gui.imgui.EndFrame();
        }

        void SelectPage(const int page, const float width) {
            Draw(width);
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent(pageOrigin.x + 2.0F, pageOrigin.y + 2.0F);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            Draw(width);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            Draw(width);
            ImGuiWindow *popup{};
            for (auto *window : ImGui::GetCurrentContext()->Windows)
                if (window->Active && (window->Flags & ImGuiWindowFlags_Popup))
                    popup = window;
            REQUIRE(popup != nullptr);
            const float rowHeight = popup->ContentSize.y / 3.0F;
            io.AddMousePosEvent((popup->InnerRect.Min.x + popup->InnerRect.Max.x) * 0.5F,
                                popup->DC.CursorStartPos.y + (static_cast<float>(page) + 0.5F) * rowHeight);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            Draw(width);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            Draw(width);
        }

        void ActivateRebind(const std::optional<int> binding = {}) {
            const auto &label =
                gui.localization.Get("editor", binding ? "workspace.input_mapping.rebind" : "workspace.input_mapping.add_binding");
            for (auto *window : ImGui::GetCurrentContext()->Windows) {
                if (window->Active && std::string_view{window->Name}.find("##InputMappingContent") != std::string_view::npos) {
                    ImGui::FocusWindow(window);
                    auto seed = window->IDStack.back();
                    if (binding)
                        seed = ImHashData(&*binding, sizeof(int), seed);
                    ImGui::ActivateItemByID(ImHashStr(label.c_str(), 0, seed));
                    return;
                }
            }
            FAIL("Input Mapping content was not drawn");
        }
    };

    TEST_CASE("Input Mapping renders localized narrow and wide states through shared controls", "[editor][input][gui]") {
        MappingFixture fixture;
        fixture.Locale(GENERATE("en-US", "tr-TR"));
        const auto preset = GENERATE(Theme::Preset::HoroDark, Theme::Preset::Light);
        const auto width = GENERATE(260.0F, 900.0F);
        const auto disabled = GENERATE(false, true);
        const auto page = GENERATE(0, 1, 2);
        Theme::SetThemePreset(preset);
        Theme::ApplyCurrentTheme();
        REQUIRE(fixture.gui.input.AssignGamepad(255, fixture.pad));
        fixture.gui.input.AssignKeyboardMouse(255);
        fixture.SelectPage(page, width);
        for (int frame = 0; frame < 3; ++frame)
            fixture.Draw(width, disabled);
        REQUIRE(ImGui::GetDrawData()->TotalVtxCount > 0);
        bool actionTable{};
        bool keyboardCard{};
        for (const auto *window : ImGui::GetCurrentContext()->Windows) {
            if (!window->Active)
                continue;
            INFO(window->Name);
            CHECK(window->ScrollMax.x < 1.0F);
            CHECK(window->Size.x <= width);
            keyboardCard = keyboardCard || std::string_view{window->Name}.find("keyboard-mouse") != std::string_view::npos;
            if (const auto *table = ImGui::TableFindByID(ImHashStr("##ActionMap", 0, window->ID)); table)
                actionTable = actionTable || table->LastFrameActive == ImGui::GetFrameCount();
        }
        CHECK(actionTable == (page == 0));
        CHECK(keyboardCard == (page == 1));
    }

    TEST_CASE("Input Mapping replaces one composite binding and persists the other controls", "[editor][input][gui]") {
        MappingFixture fixture;
        const Input::ActionId action{"move"};
        const std::vector<Input::InputBinding> bindings{{.kind = Input::BindingControlKind::Key, .key = Input::Key::A, .scale = -1.0F},
                                                        {.kind = Input::BindingControlKind::Key, .key = Input::Key::W, .component = 1}};
        REQUIRE(
            fixture.gui.input.SetActionMap({{action, Input::ActionValueType::Axis2D, Input::InputContextId{"workspace"}, true, bindings}})
                .HasValue());
        fixture.Draw(900);
        fixture.ActivateRebind(0);
        fixture.Draw(900);
        REQUIRE_FALSE(fixture.gui.input.IsContextActive(fixture.workspace));
        fixture.collector.BeginFrame(2);
        bool analog{};
        SECTION("Keyboard replacement") {
            fixture.collector.SetKey(Input::Key::D, true);
        }
        SECTION("Negative analog gesture preserves negative action direction") {
            analog = true;
            REQUIRE(fixture.collector.SetGamepadAxis(fixture.pad, Input::GamepadAxis::LeftX, -1.0F));
        }
        fixture.gui.input.BeginFrame(fixture.collector.Commit());
        fixture.Draw(900);
        const auto paths = Input::ResolveBindingProfilePaths({}, fixture.home.directory.Path());
        REQUIRE(paths.HasValue());
        const auto saved = Input::LoadBindingProfile(paths.Value().editorUser);
        REQUIRE(saved.HasValue());
        REQUIRE(saved.Value().overrides.size() == 1);
        const auto &updated = saved.Value().overrides.front().bindings;
        REQUIRE(updated.size() == 2);
        CHECK(updated[0].kind == (analog ? Input::BindingControlKind::GamepadAxis : Input::BindingControlKind::Key));
        if (!analog)
            CHECK(updated[0].key == Input::Key::D);
        CHECK(updated[0].scale == (analog ? 1.0F : -1.0F));
        CHECK(updated[0].component == 0);
        CHECK(updated[1] == bindings[1]);
        CHECK(fixture.gui.input.Profile().overrides == saved.Value().overrides);
        fixture.collector.BeginFrame(3);
        fixture.gui.input.BeginFrame(fixture.collector.Commit());
        fixture.collector.BeginFrame(4);
        fixture.gui.input.BeginFrame(fixture.collector.Commit());
        CHECK(fixture.gui.input.ReadAction(fixture.workspace, action).x == -1.0F);
    }

    TEST_CASE("Hidden Input Mapping releases its capture without retiring another modal owner", "[editor][input][gui]") {
        MappingFixture fixture;
        fixture.Draw(900);
        fixture.ActivateRebind();
        fixture.Draw(900);
        REQUIRE_FALSE(fixture.gui.input.IsContextActive(fixture.workspace));
        auto modal = fixture.gui.input.PushContext(Input::InputContextId{"modal"}, Input::InputContextKind::ModalRoot);
        fixture.panel.OnPresentationHidden(fixture.command);
        fixture.collector.BeginFrame(2);
        fixture.gui.input.BeginFrame(fixture.collector.Commit());
        REQUIRE(fixture.gui.input.IsContextActive(modal));
        REQUIRE_FALSE(fixture.gui.input.IsContextActive(fixture.workspace));
    }
}  // namespace
