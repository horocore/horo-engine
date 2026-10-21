#include "EditorPageTestSupport.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/modals/extension_pages/EditorPageModal.h"
#include "editor/modals/extension_pages/EditorPageThemeCapture.h"
#include "editor/modals/extension_pages/EditorPageView.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

namespace Horo::Editor::Tests {
    namespace {
        /** @brief Keeps the real GUI context alive longer than every borrowed font/theme/context member. */
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
            }
        };

        /** @brief Exercises the real view and validated projection inside an actual bounded GUI frame. */
        void Draw(EditorPageView &view, float width, bool settings = false) {
            ImGui::NewFrame();
            ImGui::SetNextWindowSize({width, 440});
            ImGui::Begin("Page fixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
            if (settings)
                view.DrawSettings();
            else
                view.DrawPage();
            ImGui::End();
            ImGui::Render();
        }
    }  // namespace

    TEST_CASE("Page theme bridge captures actual owner changes and keeps unchanged frame revision", "[editor][EditorPage][Theme]") {
        Gui gui;
        ImGui::GetStyle().FrameBorderSize = 0;
        EditorPageThemeCapture capture;
        REQUIRE(capture.Frame()->revision == 0);
        REQUIRE(capture.Update(gui.context).HasValue());
        const auto revision = capture.Frame()->revision;
        CHECK(revision != 0);
        CHECK(capture.Frame()->icons.strokeWidth == 0);
        CHECK(capture.Frame()->motion.focusSeconds == Theme::GetActiveTokens().motion.focus);
        CHECK(capture.Frame()->accessibility.reduceMotion == Theme::GetActiveTokens().motion.reduceMotion);
        CHECK(capture.Frame()->interaction.focusRingWidth == 0);
        REQUIRE(capture.Update(gui.context).HasValue());
        CHECK(capture.Frame()->revision == revision);
        ImGui::GetStyle().DisabledAlpha = 0.42F;
        REQUIRE(capture.Update(gui.context).HasValue());
        CHECK(capture.Frame()->revision == revision + 1);
        CHECK(capture.Frame()->interaction.disabledOpacity == 0.42F);
        CHECK(Extensions::ValidateEditorThemeFrame(*capture.Frame()).HasValue());
    }

    TEST_CASE("Actual page view draws narrow localized exact integer drafts and survives provider removal", "[editor][EditorPage][View]") {
        Extensions::Tests::PageSupport::Fixture fixture;
        fixture.Begin();
        fixture.Set(9007199254740993LL);
        Gui gui;
        EditorPageThemeCapture capture;
        REQUIRE(capture.Update(gui.context).HasValue());
        auto borrowed = std::shared_ptr<Extensions::EditorPageHost>(&fixture.host, [](auto *) {
            // The stack-owned fixture outlives the view and retains destruction responsibility.
        });
        EditorPageView view{gui.context, borrowed, capture.Frame()};
        view.Select(fixture.provider, fixture.id);
        Draw(view, 210);
        Draw(view, 640);
        CHECK(fixture.Value() == 9007199254740993LL);
        CHECK(fixture.store.writes == 0);
        REQUIRE(view.Current());
        fixture.registration->Reset();
        fixture.host.Update();
        CHECK(view.Current() == nullptr);
        Draw(view, 210);
    }

    TEST_CASE("Actual transient page joins the modal interaction owner and cancels on provider retirement", "[editor][EditorPage][Modal]") {
        Extensions::Tests::PageSupport::Fixture fixture{true};
        fixture.Begin();
        Gui gui;
        Input::InputRouter input;
        EditorModalHost modals{gui.editor, input};
        EditorPageThemeCapture capture;
        REQUIRE(capture.Update(gui.context).HasValue());
        auto borrowed = std::shared_ptr<Extensions::EditorPageHost>(&fixture.host, [](auto *) {
            // The stack-owned fixture outlives the view and retains destruction responsibility.
        });
        PumpEditorPageModals(gui.context, borrowed, capture.Frame(), modals);
        REQUIRE(modals.TopModal());
        CHECK(modals.InteractionScope().kind == EditorInteractionScopeKind::Modal);
        fixture.registration->Reset();
        fixture.host.Update();
        modals.OnUpdate(0.0F);
        CHECK(modals.TopModal() == nullptr);
        CHECK(fixture.store.writes == 0);
    }

    TEST_CASE("Settings page renders admitted boolean and text controls inside localized disabled groups", "[editor][EditorPage][View]") {
        using namespace Extensions;
        auto definition = Extensions::Tests::PageSupport::Definition(false, "fixture.page");
        auto &field = definition.fields.front();
        const EditorUiNodeBase base{.id = {"fixture.input"},
                                    .parent = {"fixture.group"},
                                    .label = {EditorUiTextKind::LocalizationKey, "fixture.count"},
                                    .description = {EditorUiTextKind::LocalizationKey, "fixture.invalid"}};
        definition.form.nodes.clear();
        definition.form.nodes.emplace_back(EditorUiContainerNode{.base = {.id = {"fixture.group"},
                                                                          .label = {EditorUiTextKind::LocalizationKey, "fixture.title"},
                                                                          .focusPolicy = EditorUiFocusPolicy::Never,
                                                                          .enabled = false}});
        SECTION("boolean") {
            field.setting.type = SettingValueType::Boolean;
            field.setting.defaultValue = true;
            definition.form.nodes.emplace_back(EditorUiBooleanNode{base, field.binding, true});
        }
        SECTION("text") {
            field.setting.type = SettingValueType::String;
            field.setting.defaultValue = std::string{"localized draft"};
            definition.form.nodes.emplace_back(
                EditorUiTextFieldNode{.base = base, .binding = field.binding, .value = "localized draft", .maximumBytes = 4096});
        }
        definition.form.nodes.emplace_back(EditorUiLabelNode{.base = {.id = {"fixture.label"}, .focusPolicy = EditorUiFocusPolicy::Never},
                                                             .text = {EditorUiTextKind::LocalizationKey, "fixture.title"}});
        definition.form.nodes.emplace_back(EditorUiTextNode{.base = {.id = {"fixture.text"}, .focusPolicy = EditorUiFocusPolicy::Never},
                                                            .text = {EditorUiTextKind::TechnicalText, "safe technical identifier"}});
        definition.form.nodes.emplace_back(
            EditorUiActionNode{.base = {.id = {"fixture.action"}, .label = {EditorUiTextKind::LocalizationKey, "fixture.title"}},
                               .action = {"fixture.action"}});
        Extensions::Tests::PageSupport::Fixture fixture{false, std::move(definition)};
        fixture.Begin();
        Gui gui;
        EditorPageThemeCapture capture;
        REQUIRE(capture.Update(gui.context).HasValue());
        auto borrowed = std::shared_ptr<EditorPageHost>(&fixture.host, [](auto *) {
            // The stack-owned fixture outlives the view and retains destruction responsibility.
        });
        EditorPageView view{gui.context, borrowed, capture.Frame()};
        view.Select(fixture.provider, fixture.id);
        Draw(view, 210, true);
        Draw(view, 210, true);
        Draw(view, 640, true);
        REQUIRE(view.Current());
        CHECK_FALSE(view.Current()->dirty);
        CHECK(fixture.store.writes == 0);
        view.Cancel();
        fixture.host.Update();
        CHECK(fixture.Page().state == EditorPageState::Cancelled);
    }

    TEST_CASE("Settings page without an admitted theme retains its draft and can still cancel", "[editor][EditorPage][View]") {
        Extensions::Tests::PageSupport::Fixture fixture;
        fixture.Begin();
        Gui gui;
        auto borrowed = std::shared_ptr<Extensions::EditorPageHost>(&fixture.host, [](auto *) {
            // The stack-owned fixture outlives the view and retains destruction responsibility.
        });
        EditorPageView view{gui.context, borrowed, {}};
        view.Select(fixture.provider, fixture.id);
        Draw(view, 210);
        REQUIRE(view.Current());
        CHECK(fixture.Value() == 20);
        CHECK(fixture.store.writes == 0);
        view.Cancel();
        fixture.host.Update();
        CHECK(fixture.Page().state == Extensions::EditorPageState::Cancelled);
        EditorPageView detached{gui.context, {}, {}};
        detached.Cancel();
        CHECK(detached.Current() == nullptr);
    }

    TEST_CASE("Actual modal renders its owned page and cancels through the modal close boundary", "[editor][EditorPage][Modal]") {
        Extensions::Tests::PageSupport::Fixture fixture{true};
        fixture.Begin();
        Gui gui;
        Input::InputRouter input;
        EditorModalHost modals{gui.editor, input};
        EditorPageThemeCapture capture;
        REQUIRE(capture.Update(gui.context).HasValue());
        auto borrowed = std::shared_ptr<Extensions::EditorPageHost>(&fixture.host, [](auto *) {
            // The stack-owned fixture outlives the view and retains destruction responsibility.
        });
        PumpEditorPageModals(gui.context, borrowed, capture.Frame(), modals);
        auto *modal = dynamic_cast<EditorPageModal *>(modals.TopModal());
        REQUIRE(modal);
        if (!modal)
            return;
        CHECK(modal->Owns(fixture.provider, fixture.id));
        CHECK_FALSE(modal->Owns(fixture.provider, "different.page"));
        CHECK(modal->Presentation().dimWorkspace);
        CHECK_FALSE(modal->ClosePolicy().allowOutsideClick);
        ImGui::NewFrame();
        modals.Draw();
        ImGui::Render();
        CHECK(fixture.store.writes == 0);
        REQUIRE(modals.RequestClose(modal->Id(), ModalCloseReason::Cancelled).HasValue());
        modals.OnUpdate(0.0F);
        fixture.host.Update();
        CHECK(modals.TopModal() == nullptr);
        CHECK(fixture.Page().state == Extensions::EditorPageState::Cancelled);
    }

}  // namespace Horo::Editor::Tests
