#include "Horo/Application/ProjectCompatibility.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/docking/WorkspacePathText.h"
#include "editor/screens/workspace/panels/input_mapping/InputMappingPanel.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "support/OwnedTestDirectory.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdlib>
#include <fstream>
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
        Input::GamepadDeviceId rawPad;
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
            rawPad = collector.ConnectGamepad("Unknown controller", false, 0, 1);
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

        void Draw(const float width, const bool disabled = false, std::string *rendered = nullptr) {
            ImGui::GetIO().DisplaySize = {width, 800};
            gui.imgui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({width, 700});
            ImGui::Begin("Input Mapping render", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            if (rendered)
                ImGui::LogToBuffer();
            ImGui::BeginDisabled(disabled);
            pageOrigin = ImGui::GetCursorScreenPos();
            panel.DrawPanel(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail(), model, command, gui.context);
            ImGui::EndDisabled();
            if (rendered) {
                *rendered = ImGui::GetCurrentContext()->LogBuffer.c_str();
                ImGui::LogFinish();
            }
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

        ImGuiWindow *ContentWindow() {
            for (auto *window : ImGui::GetCurrentContext()->Windows) {
                if (window->Active && window->ParentWindow && std::string_view{window->ParentWindow->Name} == "Input Mapping render" &&
                    std::string_view{window->Name}.find("##InputMappingContent") != std::string_view::npos)
                    return window;
            }
            FAIL("Input Mapping content was not drawn");
            return nullptr;
        }

        void ActivateRebind(const std::optional<int> binding = {}) {
            const auto &label =
                gui.localization.Get("editor", binding ? "workspace.input_mapping.rebind" : "workspace.input_mapping.add_binding");
            auto *window = ContentWindow();
            ImGui::FocusWindow(window);
            auto seed = window->IDStack.back();
            if (binding)
                seed = ImHashData(&*binding, sizeof(int), seed);
            ImGui::ActivateItemByID(ImHashStr(label.c_str(), 0, seed));
        }

        void SelectRawAxisRange(const int range, const float width) {
            Draw(width);
            auto *window = ContentWindow();
            const auto seed = ImHashStr("##RawAxisRange", 0, window->IDStack.back());
            const auto field = ImHashStr("Combo#####RawAxisRange", 0, seed);
            auto &io = ImGui::GetIO();
            const ImRect bounds = window->InnerClipRect;
            const float step = std::max(1.0F, ImGui::GetFontSize() * 0.5F);
            bool found{};
            for (float y = bounds.Min.y + step * 0.5F; y < bounds.Max.y; y += step) {
                io.AddMousePosEvent(bounds.GetCenter().x, y);
                Draw(width);
                if (ImGui::GetCurrentContext()->HoveredId == field) {
                    found = true;
                    break;
                }
            }
            REQUIRE(found);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            Draw(width);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            Draw(width);
            ImGuiWindow *popup{};
            for (auto *candidate : ImGui::GetCurrentContext()->Windows)
                if (candidate->Active && (candidate->Flags & ImGuiWindowFlags_Popup))
                    popup = candidate;
            REQUIRE(popup != nullptr);
            CHECK(popup->Size.x <= width);
            const float rowHeight = popup->ContentSize.y / 3.0F;
            io.AddMousePosEvent(popup->InnerRect.GetCenter().x,
                                popup->DC.CursorStartPos.y + (static_cast<float>(range) + 0.5F) * rowHeight);
            Draw(width);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            Draw(width);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            Draw(width);
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

    TEST_CASE("Input Mapping reports required actions missing from a saved profile without applying it", "[editor][input][gui]") {
        MappingFixture fixture;
        fixture.Locale(GENERATE("en-US", "tr-TR"));
        const Input::ActionId jump{"test.jump"};
        REQUIRE(fixture.gui.input
                    .SetActionMap({{jump,
                                    Input::ActionValueType::Digital,
                                    Input::InputContextId{"workspace"},
                                    true,
                                    {{.kind = Input::BindingControlKind::Key, .key = Input::Key::Space}}}})
                    .HasValue());
        const auto active = fixture.gui.input.Profile();
        const auto paths = Input::ResolveBindingProfilePaths({}, fixture.home.directory.Path());
        REQUIRE(paths.HasValue());
        const Input::InputBindingProfile saved{.overrides = {{jump, {}}}};
        REQUIRE(Input::SaveBindingProfileAtomically(paths.Value().editorUser, saved).HasValue());

        constexpr float Width = 260.0F;
        fixture.SelectPage(2, Width);
        fixture.Draw(Width);
        auto *window = fixture.ContentWindow();
        ImGui::FocusWindow(window);
        const auto &button = InputMappingPanel::kTextScope.Text(fixture.gui.localization, "check_saved_profile");
        ImGui::ActivateItemByID(ImHashStr(button.c_str(), 0, window->IDStack.back()));
        std::string rendered;
        fixture.Draw(Width, false, &rendered);
        CHECK(rendered.find(InputMappingPanel::kTextScope.Text(fixture.gui.localization, "unbound_required_action")) != std::string::npos);
        CHECK(rendered.find(jump.Value()) != std::string::npos);
        CHECK(fixture.gui.input.Profile() == active);
        CHECK(Input::LoadBindingProfile(paths.Value().editorUser).Value() == saved);
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

    TEST_CASE("Input Mapping persists an explicitly selected unknown-controller trigger range", "[editor][input][gui]") {
        MappingFixture fixture;
        fixture.Locale(GENERATE("en-US", "tr-TR"));
        const float width = GENERATE(260.0F, 900.0F);
        const float rest = GENERATE(-1.0F, 1.0F);
        const Input::ActionId action{"trigger"};
        REQUIRE(fixture.gui.input
                    .SetActionMap({{action,
                                    Input::ActionValueType::Digital,
                                    Input::InputContextId{"workspace"},
                                    true,
                                    {{.kind = Input::BindingControlKind::Key, .key = Input::Key::Space}}}})
                    .HasValue());
        fixture.collector.BeginFrame(2);
        REQUIRE(fixture.collector.SetRawGamepadAxis(fixture.rawPad, 0, rest));
        fixture.gui.input.BeginFrame(fixture.collector.Commit());
        fixture.SelectRawAxisRange(rest < 0 ? 1 : 2, width);
        fixture.ActivateRebind(0);
        fixture.Draw(width);
        REQUIRE_FALSE(fixture.gui.input.IsContextActive(fixture.workspace));
        fixture.collector.BeginFrame(3);
        REQUIRE(fixture.collector.SetRawGamepadAxis(fixture.rawPad, 0, -rest));
        fixture.gui.input.BeginFrame(fixture.collector.Commit());
        fixture.Draw(width);
        const auto paths = Input::ResolveBindingProfilePaths({}, fixture.home.directory.Path());
        REQUIRE(paths.HasValue());
        const auto saved = Input::LoadBindingProfile(paths.Value().editorUser);
        REQUIRE(saved.HasValue());
        const auto policy = rest < 0 ? Input::DeadzoneKind::UnipolarFromNegativeOne : Input::DeadzoneKind::UnipolarFromPositiveOne;
        REQUIRE(saved.Value().overrides.front().bindings.front().deadzoneKind == policy);
        fixture.collector.BeginFrame(4);
        REQUIRE(fixture.collector.SetRawGamepadAxis(fixture.rawPad, 0, rest));
        fixture.gui.input.BeginFrame(fixture.collector.Commit());
        fixture.collector.BeginFrame(5);
        fixture.gui.input.BeginFrame(fixture.collector.Commit());
        CHECK_FALSE(fixture.gui.input.ReadAction(fixture.workspace, action).down);
        fixture.collector.BeginFrame(6);
        REQUIRE(fixture.collector.SetRawGamepadAxis(fixture.rawPad, 0, -rest));
        fixture.gui.input.BeginFrame(fixture.collector.Commit());
        CHECK(fixture.gui.input.ReadAction(fixture.workspace, action).pressed);
    }

    TEST_CASE("Pointer gestures outside Input Mapping abandon capture without authoring another surface's input", "[editor][input][gui]") {
        MappingFixture fixture;
        fixture.Draw(900);
        fixture.ActivateRebind();
        fixture.Draw(900);
        REQUIRE_FALSE(fixture.gui.input.IsContextActive(fixture.workspace));
        const auto profile = fixture.gui.input.Profile();
        fixture.collector.BeginFrame(2);
        ImGui::GetIO().AddMousePosEvent(950, 780);
        SECTION("Pointer button") {
            fixture.collector.SetPointerButton(Input::PointerButton::Primary, true);
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        }
        SECTION("Wheel") {
            fixture.collector.AddPointerWheel(0.0F, 1.0F);
            ImGui::GetIO().AddMouseWheelEvent(0.0F, 1.0F);
        }
        fixture.gui.input.BeginFrame(fixture.collector.Commit());
        fixture.Draw(900);
        CHECK(fixture.gui.input.IsContextActive(fixture.workspace));
        CHECK(fixture.gui.input.Profile() == profile);
        const auto paths = Input::ResolveBindingProfilePaths({}, fixture.home.directory.Path());
        REQUIRE(paths.HasValue());
        CHECK_FALSE(std::filesystem::exists(paths.Value().editorUser));
    }

    TEST_CASE("Input Mapping persists keyboard gamepad and pointer captures for the open project only", "[editor][input][gui]") {
        MappingFixture fixture;
        const auto project = fixture.home.directory.Path() / "Input project";
        const auto root = project / ".horo";
        std::filesystem::create_directories(root);
        const auto release = Application::CurrentEngineReleaseVersion();
        const auto *decision = Application::BuiltInReleaseCompatibilityRegistry().Find(release);
        REQUIRE(decision != nullptr);
        std::ofstream(root / "project.json")
            << "{\"horoVersion\":\"" << Application::FormatHoroVersion(release.value) << "\",\"persistentContract\":\""
            << Application::FormatPersistentContractHash(decision->persistentContract)
            << R"(","projectId":"input-test","name":"Input Project","projectVersion":"0.1.0","createdAt":"2026-07-19T00:00:00Z","settings":{"renderBackend":"opengl"}})";
        fixture.model.projectRoot = WorkspacePathText(project);
        const Input::ActionId action{"interact"};
        const Input::InputContextId context{"workspace"};
        const std::vector<Input::ActionDescriptor> actions{
            {action, Input::ActionValueType::Digital, context, true, {{.kind = Input::BindingControlKind::Key, .key = Input::Key::Space}}}};
        REQUIRE(fixture.gui.input.SetActionMap(actions).HasValue());
        const auto paths = Input::ResolveBindingProfilePaths(project, fixture.home.directory.Path(), "input-test");
        REQUIRE(paths.HasValue());
        Input::InputBindingProfile global;
        global.overrides.push_back({action, {{.kind = Input::BindingControlKind::Key, .key = Input::Key::B}}});
        REQUIRE(Input::SaveBindingProfileAtomically(paths.Value().editorUser, global).HasValue());
        REQUIRE(fixture.gui.input.ReloadProfile(paths.Value()).HasValue());

        fixture.Draw(900);
        fixture.ActivateRebind(0);
        fixture.Draw(900);
        REQUIRE_FALSE(fixture.gui.input.IsContextActive(fixture.workspace));
        fixture.collector.BeginFrame(2);
        Input::InputBinding captured;
        SECTION("Keyboard") {
            fixture.collector.SetKey(Input::Key::D, true);
            captured = {.kind = Input::BindingControlKind::Key, .key = Input::Key::D};
        }
        SECTION("Gamepad button") {
            REQUIRE(fixture.collector.SetGamepadButton(fixture.pad, Input::GamepadButton::South, true));
            captured = {.kind = Input::BindingControlKind::GamepadButton, .gamepadButton = Input::GamepadButton::South};
        }
        SECTION("Pointer button") {
            const auto center = fixture.ContentWindow()->InnerClipRect.GetCenter();
            ImGui::GetIO().AddMousePosEvent(center.x, center.y);
            fixture.Draw(900);
            REQUIRE(ImGui::GetCurrentContext()->HoveredWindow == fixture.ContentWindow());
            fixture.collector.SetPointerPosition(center.x, center.y);
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
            fixture.collector.SetPointerButton(Input::PointerButton::Secondary, true);
            captured = {.kind = Input::BindingControlKind::PointerButton, .pointerButton = Input::PointerButton::Secondary};
        }
        fixture.gui.input.BeginFrame(fixture.collector.Commit());
        fixture.Draw(900);
        const auto saved = Input::LoadBindingProfile(paths.Value().projectUser);
        INFO((saved.HasError() ? saved.ErrorValue().message : ""));
        REQUIRE(saved.HasValue());
        REQUIRE(saved.Value().overrides.size() == 1);
        CHECK(saved.Value().overrides.front().bindings == std::vector{captured});
        const auto savedGlobal = Input::LoadBindingProfile(paths.Value().editorUser);
        REQUIRE(savedGlobal.HasValue());
        CHECK(savedGlobal.Value() == global);

        Input::InputRouter reopened;
        REQUIRE(reopened.SetActionMap(actions).HasValue());
        REQUIRE(reopened.ReloadProfile(paths.Value()).HasValue());
        CHECK(reopened.Profile().overrides == saved.Value().overrides);
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
