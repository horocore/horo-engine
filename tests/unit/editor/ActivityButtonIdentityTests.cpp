#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_internal.h>

namespace {
    struct ActivityIdentityContext {
        ActivityIdentityContext() {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.DisplaySize = {320.0F, 200.0F};
            io.DeltaTime = 1.0F / 60.0F;
            io.IniFilename = nullptr;
            io.Fonts->AddFontDefault();
            static_cast<void>(io.Fonts->Build());
            auto *font = io.Fonts->Fonts.front();
            fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font, .icon = font};
        }

        ~ActivityIdentityContext() {
            ImGui::DestroyContext();
        }

        ActivityIdentityContext(const ActivityIdentityContext &) = delete;
        ActivityIdentityContext &operator=(const ActivityIdentityContext &) = delete;
        ActivityIdentityContext(ActivityIdentityContext &&) = delete;
        ActivityIdentityContext &operator=(ActivityIdentityContext &&) = delete;

        Horo::Editor::Theme::Fonts fonts;
    };
}  // namespace

TEST_CASE("Activity badge and hovered tooltip preserve the owned button item identity", "[Editor][Ui][Activity][Identity]") {
    ActivityIdentityContext context;
    bool enabled = true;
    SECTION("enabled destination") {
        enabled = true;
    }
    SECTION("disabled destination retains its disabled item flags") {
        enabled = false;
    }
    ImGuiLastItemData undecorated{};
    bool tooltipSubmitted = false;
    for (int frame = 0; frame < 3; ++frame) {
        ImGui::GetIO().AddMousePosEvent(65.0F, 65.0F);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({20.0F, 20.0F});
        ImGui::SetNextWindowSize({220.0F, 150.0F});
        ImGui::Begin("ActivityIdentity", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorScreenPos({50.0F, 50.0F});
        const auto expectedId = ImGui::GetID("##destination");
        static_cast<void>(Horo::Editor::Ui::ActivityButton({.id = "##destination",
                                                            .tooltip = frame == 0 ? "" : "Destination tooltip",
                                                            .active = true,
                                                            .enabled = enabled,
                                                            .badgeCount = frame == 0 ? 0U : 123U},
                                                           context.fonts));
        const ImGuiLastItemData actual{GImGui->LastItemData};
        CHECK(actual.ID == expectedId);
        CHECK(((actual.ItemFlags & ImGuiItemFlags_Disabled) != 0) == !enabled);
        if (frame == 0) {
            undecorated = actual;
        } else {
            CHECK(actual.ItemFlags == undecorated.ItemFlags);
            CHECK(actual.Rect.Min.x == undecorated.Rect.Min.x);
            CHECK(actual.Rect.Min.y == undecorated.Rect.Min.y);
            CHECK(actual.Rect.Max.x == undecorated.Rect.Max.x);
            CHECK(actual.Rect.Max.y == undecorated.Rect.Max.y);
            CHECK(actual.NavRect.Min.x == undecorated.NavRect.Min.x);
            CHECK(actual.NavRect.Max.y == undecorated.NavRect.Max.y);
            CHECK(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled));
            for (const auto *window : GImGui->Windows)
                tooltipSubmitted |= window->Active && (window->Flags & ImGuiWindowFlags_Tooltip) != 0;
        }
        ImGui::End();
        ImGui::Render();
    }
    CHECK(tooltipSubmitted);
}
