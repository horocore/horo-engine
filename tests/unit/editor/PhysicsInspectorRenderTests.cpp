#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"

#include <catch2/catch_test_macros.hpp>
#include <unordered_map>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Stable localized strings intentionally exercise long narrow-width labels. */
    class Localization final : public ILocalizationService {
    public:
        const std::string &Get(std::string_view, const std::string_view key) const override {
            return messages_.try_emplace(std::string(key), "Uzun yerelleştirilmiş fizik denetimi açıklaması " + std::string(key))
                .first->second;
        }

    private:
        mutable std::unordered_map<std::string, std::string> messages_;
    };

    /** @brief Own an actual shared-control ImGui frame without a GPU/backend claim. */
    struct Frame final {
        Frame() {
            ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.DisplaySize = {640, 900};
            io.DeltaTime = 1.0F / 60.0F;
            io.Fonts->AddFontDefault();
            static_cast<void>(io.Fonts->Build());
        }

        ~Frame() {
            ImGui::DestroyContext();
        }

        Frame(const Frame &) = delete;
        Frame &operator=(const Frame &) = delete;
    };

    /** @brief Build an explicit portable authored projection with unresolved references kept visible. */
    EditorWorkspaceViewModel View() {
        EditorWorkspaceViewModel view;
        view.documentRevision = {1};
        view.objects = {{.id = {7}, .name = "Physics"}};
        view.primarySelection = SceneObjectId{7};
        view.selectedObjects = {{7}};
        view.physicsContext.canEdit = true;
        auto &components = view.objects.front().components;
        components.rigidBody = Runtime::RigidBodyComponent{.id = {1}, .body = {10}};
        components.colliders.push_back({.id = {2}, .collider = {20}, .body = {{7}, {10}}});
        components.physicsConstraints.push_back({.id = {3}, .constraint = {30}, .first = {.body = {{7}, {10}}}});
        return view;
    }

    /** @brief Exercise actual shared controls over consecutive stable frames without a renderer/GPU claim. */
    void Draw(const EditorWorkspaceViewModel &view, const EditorGuiContext &context, const float width) {
        InspectorPanel panel;
        for (int index = 0; index != 2; ++index) {
            ImGui::NewFrame();
            ImGui::SetNextWindowSize({width, 850});
            ImGui::Begin("PhysicsInspectorRender", nullptr, ImGuiWindowFlags_NoSavedSettings);
            EditorWorkspaceViewCommandData command;
            panel.DrawPanel(ImGui::GetCursorScreenPos(), {width - 20, 800}, view, command, context);
            CHECK(command.command == EditorWorkspaceViewCommand::None);
            ImGui::End();
            ImGui::Render();
            REQUIRE(ImGui::GetDrawData() != nullptr);
            CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
        }
        panel.OnDetach();
    }
}  // namespace

TEST_CASE("Physics Inspector shared controls render portable shapes and unsupported states", "[unit][editor][physics][inspector]") {
    Frame frame;
    EngineDataBus engine;
    EditorDataBus events;
    Localization localization;
    ImFont *font = ImGui::GetIO().Fonts->Fonts.front();
    const ThemeContext theme{.fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font}};
    const EditorSettingsSnapshot settings{};
    const EditorGuiContext context{.engineEvents = engine,
                                   .editorEvents = events,
                                   .localization = localization,
                                   .theme = theme,
                                   .settings = settings};
    auto view = View();
    auto &components = view.objects.front().components;
    float width = 360;
    SECTION("narrow localized") {
        width = 180;
    }
    SECTION("disabled owner") {
        view.physicsContext.canEdit = false;
    }
    SECTION("locked object") {
        view.objects.front().effectivelyLocked = true;
    }
    SECTION("sphere") {
        components.colliders.front().source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{}};
    }
    SECTION("capsule") {
        components.colliders.front().source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsCapsuleCollider{}};
    }
    SECTION("static plane") {
        components.colliders.front().source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsStaticPlaneCollider{}};
    }
    SECTION("unresolved asset") {
        components.colliders.front().source = Runtime::PhysicsShapeAssetReference{};
    }
    SECTION("retained diagnostic") {
        view.physicsEditError = "Physics typed validation rejected the candidate.";
    }
    Draw(view, context, width);
}
