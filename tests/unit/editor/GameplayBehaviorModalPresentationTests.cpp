#include "editor/modals/gameplay_behavior/GameplayBehaviorFilenameModal.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <filesystem>
#include <imgui_internal.h>
#include <string>
#include <string_view>

namespace {
    /** @brief Finds a rendered modal child without depending on ImGui's generated child-window suffix. */
    ImGuiWindow *FindModalChild(const std::string_view id) {
        for (ImGuiWindow *window : ImGui::GetCurrentContext()->Windows) {
            if (std::string_view{window->Name}.find(id) != std::string_view::npos)
                return window;
        }
        return nullptr;
    }
}  // namespace

TEST_CASE("Gameplay behavior form keeps validation below an equal-height filename row without scrolling", "[unit][editor][gui][behavior]") {
    using namespace Horo::Editor;
    const auto locale = GENERATE("en-US", "tr-TR");
    const auto kind = GENERATE(GameplayBehaviorKind::Lua, GameplayBehaviorKind::Native);
    const bool validName = GENERATE(true, false);
    const float viewportWidth = GENERATE(688.0F, 520.0F);
    Tests::EditorGuiContextFixture gui{{viewportWidth, 660.0F}};
    REQUIRE(gui.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                             (std::string{locale} + ".json")));
    REQUIRE(gui.localization.Prepare(LocaleTag{locale}));
    REQUIRE(gui.localization.ActivatePrepared());
    const auto project = std::filesystem::temp_directory_path() / "horo-behavior-presentation";
    GameplayBehaviorFilenameModal
        modal{gui.context, kind, (project / "Assets/Scripts").string(), validName ? "NewBehavior" : "bad/name", project, {}, {}};
    EditorModalHost host{gui.editorEvents, gui.input};
    EditorModalContext context{gui.editorEvents, host};
    REQUIRE(modal.OnOpen(context).HasValue());
    for (unsigned frame = 0; frame < 3; ++frame) {
        gui.imgui.BeginFrame();
        static_cast<void>(modal.Draw());
        gui.imgui.EndFrame();
    }
    const auto *body = FindModalChild("##GameplayBehaviorBody");
    const auto *suffix = FindModalChild("##GameplayBehaviorExtension");
    const auto *destination = FindModalChild("##GameplayBehaviorDestination");
    REQUIRE(body != nullptr);
    REQUIRE(suffix != nullptr);
    REQUIRE(destination != nullptr);
    CHECK(suffix->Size.y == Catch::Approx(42.0F));
    CHECK(body->WindowPadding.x == Catch::Approx(20.0F));
    CHECK(body->ScrollMax.y == Catch::Approx(0.0F).margin(0.5F));
    CHECK(body->Scroll.y == Catch::Approx(0.0F));
    CHECK(destination->Pos.y > suffix->Pos.y + suffix->Size.y + ImGui::GetTextLineHeight());
    CHECK(destination->Pos.y + destination->Size.y <= body->InnerClipRect.Max.y);
}
