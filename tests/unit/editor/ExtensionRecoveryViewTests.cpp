#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/screens/workspace/ExtensionRecoveryView.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>

namespace Horo::Editor::Tests {
    namespace {
        /** @brief Destroys the native-free GUI context after all borrowed font data expires. */
        struct ContextDeleter {
            void operator()(ImGuiContext *context) const noexcept {
                ImGui::DestroyContext(context);
            }
        };

        /** @brief Real shared control/font composition with no renderer backend. */
        struct Gui {
            std::unique_ptr<ImGuiContext, ContextDeleter> imgui{ImGui::CreateContext()};
            EngineDataBus engine;
            EditorDataBus editor;
            LocalizationService localization{LocaleTag{"en-US"}};
            Theme::Fonts fonts;
            ThemeContext theme{fonts};
            EditorSettingsSnapshot settings{.settings = DefaultEditorSettings()};
            EditorGuiContext context{engine, editor, localization, theme, settings};

            Gui() {
                auto &io = ImGui::GetIO();
                io.DisplaySize = {320, 400};
                io.DeltaTime = 1.0F / 60.0F;
                io.IniFilename = nullptr;
                io.Fonts->AddFontDefault();
                REQUIRE(io.Fonts->Build());
                auto *font = io.Fonts->Fonts.front();
                fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font, .icon = font};
            }
        };
    }  // namespace

    TEST_CASE("Unavailable provider workspace intent is rendered as a real host placeholder and closes without data loss",
              "[unit][editor][extension-recovery]") {
        Gui gui;
        Extensions::EditorSurfaceRegistry registry;
        REQUIRE(registry
                    .Restore({.surfaces = {{.surfaceId = "com.example.long.surface",
                                            .provider = {"com.example", "com.example.module"},
                                            .open = true,
                                            .opaqueState = {0, 255}}}})
                    .HasValue());
        ExtensionRecoveryView view{gui.context, &registry};
        view.Update();
        ImGui::NewFrame();
        view.Draw();
        ImGui::Render();
        auto *window = ImGui::FindWindowByName("###ExtensionRecovery");
        REQUIRE(window);
        CHECK(window->Active);
        CHECK(window->DrawList->VtxBuffer.Size > 0);
        REQUIRE(registry.Close("com.example.long.surface").HasValue());
        view.Update();
        ImGui::NewFrame();
        view.Draw();
        ImGui::Render();
        CHECK_FALSE(window->Active);
        CHECK(registry.Save().surfaces.front().opaqueState == std::vector<std::uint8_t>{0, 255});
        registry.BeginShutdown();
        view.Update();
        ImGui::NewFrame();
        view.Draw();
        ImGui::Render();
        CHECK_FALSE(window->Active);
    }
}  // namespace Horo::Editor::Tests
