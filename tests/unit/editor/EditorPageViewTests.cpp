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
        void Draw(EditorPageView &view, float width) {
            ImGui::NewFrame();
            ImGui::SetNextWindowSize({width, 440});
            ImGui::Begin("Page fixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
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
}  // namespace Horo::Editor::Tests
