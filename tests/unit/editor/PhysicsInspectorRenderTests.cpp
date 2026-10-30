#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"
#include "editor/screens/workspace/panels/inspector/PhysicsInspectorWidgets.h"
#include "editor/screens/workspace/panels/inspector/add_component/AddComponentMenu.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <imgui_internal.h>
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
    SECTION("named project references") {
        const auto profile = Physics::CollisionProfileId::Parse("00000000-0000-0000-0000-000000000001");
        const auto material = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000002");
        REQUIRE(profile.HasValue());
        REQUIRE(material.HasValue());
        view.physicsProfiles.push_back({profile.Value(), "Player"});
        view.physicsMaterials.push_back({material.Value(), "Rubber"});
        components.colliders.front().collisionProfile = profile.Value();
        components.colliders.front().materials.push_back({Physics::PhysicsMaterialSlotId::FromValue(1), material.Value()});
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

TEST_CASE("Physics fields stack wrapped labels over padded full-width controls", "[editor][physics][inspector][layout]") {
    using namespace PhysicsInspectorWidgets;
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
    const float width = GENERATE(180.0F, 360.0F, 560.0F);
    float scalar = 1.25F;
    Math::Vec3 vector{1, 2, 3};
    int selected = 0;
    const std::array<const char *, 2> entries{"Static", "Dynamic"};
    auto identity = PhysicsInspectorSession::Decimal(42);
    std::uint64_t value = 42;
    for (int index = 0; index < 3; ++index) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({width, 850});
        ImGui::Begin("PhysicsFieldLayout", nullptr, ImGuiWindowFlags_NoSavedSettings);
        const auto origin = ImGui::GetCursorScreenPos();
        const float outerWidth = ImGui::GetContentRegionAvail().x;
        REQUIRE(BeginFields());
        const auto start = ImGui::GetCursorScreenPos();
        const float fieldWidth = ImGui::GetContentRegionAvail().x;
        CHECK(start.x >= origin.x + Ui::ScaledLayoutValue(12) - 1);
        CHECK(start.x + fieldWidth == Catch::Approx(origin.x + outerWidth - Ui::ScaledLayoutValue(12)).margin(1));
        CHECK_FALSE(Scalar(context, "workspace.physics.linear_damping", scalar));
        CHECK(ImGui::GetItemRectMin().x == Catch::Approx(start.x));
        CHECK(ImGui::GetItemRectMax().x == Catch::Approx(start.x + fieldWidth).margin(1));
        CHECK(ImGui::GetItemRectMin().y > start.y + ImGui::GetFontSize());
        const float scalarBottom = ImGui::GetItemRectMax().y;
        CHECK_FALSE(Vector(context, "workspace.physics.linear_velocity", vector));
        CHECK(ImGui::GetItemRectMin().y > scalarBottom + ImGui::GetFontSize());
        CHECK(ImGui::GetItemRectMax().x == Catch::Approx(start.x + fieldWidth).margin(3));
        CHECK_FALSE(Choice(context, "workspace.physics.motion", "motion", selected, entries));
        CHECK(ImGui::GetItemRectMin().x == Catch::Approx(start.x));
        CHECK(ImGui::GetItemRectMax().x == Catch::Approx(start.x + fieldWidth).margin(1));
        CHECK_FALSE(Integer(context, "workspace.physics.body_object", identity, value));
        CHECK(ImGui::GetItemRectMin().x == Catch::Approx(start.x));
        CHECK(ImGui::GetItemRectMax().x == Catch::Approx(start.x + fieldWidth).margin(1));
        Stable(context, "workspace.physics.component_id", 42);
        CHECK((ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0);
        CHECK(ImGui::GetItemRectMax().x <= start.x + fieldWidth + 1);
        EndFields();
        CHECK(ImGui::GetCursorScreenPos().y > ImGui::GetItemRectMax().y);
        ImGui::End();
        ImGui::Render();
    }
    CHECK(scalar == 1.25F);
    CHECK(vector == Math::Vec3{1, 2, 3});
    CHECK(value == 42);
}

TEST_CASE("Add Component exposes script creation through its Scripts category", "[editor][physics][inspector][menu]") {
    Frame frame;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
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
    view.projectRoot = "/projects/My Game";
    const bool create = GENERATE(true, false);
    Gameplay::BehaviorDescriptor behavior;
    behavior.typeId = Gameplay::BehaviorTypeId::Parse("game.tests.jump").Value();
    behavior.displayName = "Jump";
    view.availableBehaviors.push_back(behavior);
    AddComponentRegistry registry;
    REQUIRE(registry.SynchronizeBehaviors(view.projectRoot, 1, view.availableBehaviors).HasValue());
    PhysicsInspectorSession session;
    session.Begin(view.objects.front(), view.physicsContext, 1);
    const AddComponentContext state{view.objects.front(), session.Draft(), true, true};
    EditorWorkspaceViewCommandData command;
    for (int index = 0; index < 3; ++index) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({600, 800});
        ImGui::Begin("ComponentMenuTest");
        ImGui::OpenPopup("components");
        if (Ui::BeginMenuPopup("components")) {
            const std::string category = localization.Get("editor", "workspace.inspector.add_scripts");
            ImGui::OpenPopup(("##submenu_popup_" + category).c_str());
            if (index == 2) {
                auto &gui = *ImGui::GetCurrentContext();
                REQUIRE(gui.OpenPopupStack.Size >= 2);
                auto *popup = gui.OpenPopupStack.back().Window;
                REQUIRE(popup);
                const auto id = ImHashStr(create ? "###workflow.create_script" : "###behavior.game.tests.jump", 0, popup->ID);
                ImGui::FocusWindow(popup);
                ImGui::SetFocusID(id, popup);
                gui.NavLayer = ImGuiNavLayer_Main;
                gui.NavActivateId = gui.NavActivateDownId = gui.NavActivatePressedId = id;
            }
            if (const auto action = DrawAddComponentMenu(registry, state, context))
                ApplyAddComponentAction(*action, view.objects.front(), view.projectRoot, session, command);
            Ui::EndMenuPopup();
        }
        ImGui::End();
        ImGui::Render();
    }
    if (create) {
        CHECK(command.command == EditorWorkspaceViewCommand::CreateLuaBehavior);
        REQUIRE(command.stringPayload.has_value());
        CHECK(std::filesystem::path{*command.stringPayload} == std::filesystem::path{"/projects/My Game/Assets/Scripts"});
    } else {
        CHECK(command.command == EditorWorkspaceViewCommand::AttachBehaviorToObject);
        REQUIRE(command.behaviorTypePayload.has_value());
        CHECK(*command.behaviorTypePayload == behavior.typeId);
        CHECK(command.objectPayload == view.objects.front().id);
        CHECK(view.objects.front().components.behaviors.empty());
    }
}
