#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/screens/workspace/panels/inspector/component_fields/definitions/AudioListenerFields.h"
#include "editor/screens/workspace/panels/inspector/component_fields/definitions/AudioSourceFields.h"
#include "editor/screens/workspace/panels/inspector/component_fields/definitions/BoxColliderFields.h"
#include "editor/screens/workspace/panels/inspector/component_fields/definitions/CameraFields.h"
#include "editor/screens/workspace/panels/inspector/component_fields/definitions/CapsuleColliderFields.h"
#include "editor/screens/workspace/panels/inspector/component_fields/definitions/CharacterGeometryFields.h"
#include "editor/screens/workspace/panels/inspector/component_fields/definitions/CharacterMovementFields.h"
#include "editor/screens/workspace/panels/inspector/component_fields/definitions/DistanceJointFields.h"
#include "editor/screens/workspace/panels/inspector/component_fields/definitions/LightFields.h"
#include "editor/screens/workspace/panels/inspector/component_fields/definitions/PlaneColliderFields.h"
#include "editor/screens/workspace/panels/inspector/component_fields/definitions/RigidBodyFields.h"
#include "editor/screens/workspace/panels/inspector/component_fields/definitions/SphereColliderFields.h"
#include "editor/screens/workspace/panels/inspector/component_fields/definitions/TriggerVolumeFields.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <filesystem>
#include <fstream>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    namespace Fields = InspectorFields;

    /** @brief Stable long translations exercise current-locale resolution and narrow controls. */
    class Localization final : public ILocalizationService {
    public:
        const std::string &Get(std::string_view, std::string_view key) const override {
            return messages.try_emplace(std::string(key), "Localized long label " + std::string(key)).first->second;
        }

        mutable std::unordered_map<std::string, std::string> messages;
    };

    template <typename Model> const auto &Find(std::string_view id) {
        const auto &fields = Fields::Schema<Model>::fields;
        const auto field = std::ranges::find_if(fields, [id](const auto &entry) {
            return entry.id == id;
        });
        REQUIRE(field != fields.end());
        return *field;
    }

    /** @brief Verifies real shipped field/choice labels without treating missing keys as translations. */
    template <typename Model> void CheckCatalog(const nlohmann::json &catalog) {
        for (const auto &field : Fields::Schema<Model>::fields) {
            INFO(field.id);
            REQUIRE(catalog.contains(field.labelKey));
            CHECK_FALSE(catalog.at(field.labelKey).at("text").template get<std::string>().empty());
            for (const auto &choice : field.choices) {
                REQUIRE(catalog.contains(choice.labelKey));
                CHECK_FALSE(catalog.at(choice.labelKey).at("text").template get<std::string>().empty());
            }
        }
    }

    /** @brief Model added only through metadata, with a sparse enum and an exact large read-only integer. */
    enum class TestMode {
        Quiet = 7,
        Loud = 41
    };

    struct TestComponent {
        float gain{2.5F};
        std::string name{"Example"};
        std::uint64_t identity{18446744073709551614ULL};
        TestMode mode{TestMode::Loud};
    };

    inline constexpr std::array testChoices{Fields::Choice{7, "workspace.value.off"}, Fields::Choice{41, "workspace.value.on"}};
    inline constexpr auto testFields = Fields::Checked(
        std::array{Fields::Bind<TestComponent, &TestComponent::gain, Fields::Control::Number>("gain",
                                                                                              "workspace.inspector.audio_source_gain"),
                   Fields::Bind<TestComponent, &TestComponent::name, Fields::Control::Text>("name", "workspace.inspector.name"),
                   Fields::Bind<TestComponent, &TestComponent::mode, Fields::Control::Choice>("mode", "workspace.inspector.light_kind")
                       .Options(testChoices),
                   Fields::Bind<TestComponent, &TestComponent::identity, Fields::Control::Unsigned>("identity",
                                                                                                    "workspace.physics.component_id")
                       .ReadOnly()});
}  // namespace

TEST_CASE("Inspector defaults are read from canonical models in canonical units", "[editor][inspector][field-schema]") {
    const Runtime::CameraComponent camera;
    CHECK(std::get<float>(Find<Runtime::CameraComponent>("camera_field_of_view").defaultValue()) == camera.verticalFieldOfViewRadians);
    CHECK(std::get<float>(Find<Runtime::CameraComponent>("camera_field_of_view").defaultValue()) *
              Find<Runtime::CameraComponent>("camera_field_of_view").displayScale ==
          Catch::Approx(60.0F));
    CHECK(std::get<float>(Find<Runtime::CameraComponent>("camera_near_plane").defaultValue()) == camera.nearPlane);
    CHECK(std::get<Math::Vec3>(Find<Runtime::LightComponent>("light_color").defaultValue()) == Runtime::LightComponent{}.color);
    CHECK(std::get<float>(Find<Runtime::AudioSourceComponent>("audio_source_pitch").defaultValue()) ==
          Runtime::AudioSourceComponent{}.playback.pitch);
    CHECK(std::get<std::uint64_t>(Find<Runtime::AudioListenerComponent>("audio_listener_view").defaultValue()) == 0);
    CHECK(std::get<float>(Find<Runtime::RigidBodyComponent>("workspace.physics.linear_limit").defaultValue()) ==
          Runtime::RigidBodyComponent{}.maximumLinearSpeed);
    CHECK(std::get<float>(Find<Runtime::CharacterSceneMovement>("workspace.inspector.character.jump").defaultValue()) ==
          Runtime::CharacterSceneMovement{}.jumpSpeedMetersPerSecond);
    CHECK(std::get<std::uint64_t>(testFields.back().defaultValue()) == TestComponent{}.identity);
    CHECK(std::get<std::string>(testFields[1].defaultValue()) == "Example");
    CHECK(std::get<std::int64_t>(testFields[2].defaultValue()) == 41);
}

TEST_CASE("Inspector visibility and validation read the current typed draft", "[editor][inspector][field-schema]") {
    Runtime::CameraComponent camera;
    const auto &fov = Find<Runtime::CameraComponent>("camera_field_of_view");
    const auto &height = Find<Runtime::CameraComponent>("camera_orthographic_height");
    CHECK(fov.visible(camera));
    CHECK_FALSE(height.visible(camera));
    camera.projection = Runtime::CameraProjection::Orthographic;
    CHECK_FALSE(fov.visible(camera));
    CHECK(height.visible(camera));
    camera.nearPlane = camera.farPlane;
    CHECK_FALSE(Find<Runtime::CameraComponent>("camera_near_plane").valid(camera));
    Runtime::LightComponent light;
    const auto &range = Find<Runtime::LightComponent>("light_range");
    const auto &inner = Find<Runtime::LightComponent>("light_inner_cone");
    CHECK_FALSE(range.visible(light));
    CHECK_FALSE(inner.visible(light));
    light.kind = Runtime::LightKind::Point;
    CHECK(range.visible(light));
    CHECK_FALSE(inner.visible(light));
    light.kind = Runtime::LightKind::Spot;
    CHECK(inner.visible(light));
    light.innerConeRadians = light.outerConeRadians + 1;
    CHECK_FALSE(inner.valid(light));
    // Metadata validation never publishes/clamps an invalid authored draft.
    CHECK(light.innerConeRadians > light.outerConeRadians);
}

TEST_CASE("Typed Inspector schema renders all controls and preserves passive drafts", "[editor][inspector][field-schema][render]") {
    const float width = GENERATE(180.0F, 360.0F);
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.DisplaySize = {640, 1000};
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());
    EngineDataBus engine;
    EditorDataBus events;
    Localization localization;
    auto *font = io.Fonts->Fonts.front();
    const ThemeContext theme{.fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font}};
    const EditorSettingsSnapshot settings{};
    const EditorGuiContext context{.engineEvents = engine,
                                   .editorEvents = events,
                                   .localization = localization,
                                   .theme = theme,
                                   .settings = settings};
    TestComponent model;
    Runtime::CameraComponent camera;
    Runtime::LightComponent light;
    light.kind = Runtime::LightKind::Spot;
    for (int frame = 0; frame < 3; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({width, 900});
        ImGui::Begin("FieldSchema", nullptr, ImGuiWindowFlags_NoSavedSettings);
        const auto edit = Fields::Draw(model, std::span<const Fields::Field<TestComponent>>{testFields}, context, Fields::Layout::Stacked);
        CHECK_FALSE(edit.changed);
        CHECK_FALSE(edit.committed);
        CHECK((ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0);
        CHECK(model.identity == 18446744073709551614ULL);
        CHECK(model.mode == TestMode::Loud);
        CHECK(model.name == "Example");
        ImGui::PushID("camera");
        CHECK_FALSE(Fields::Draw(camera, context).changed);
        ImGui::PopID();
        ImGui::PushID("light");
        CHECK_FALSE(Fields::Draw(light, context).changed);
        ImGui::PopID();
        ImGui::End();
        ImGui::Render();
        CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    }
    CHECK(camera == Runtime::CameraComponent{});
    CHECK(light.innerConeRadians == Runtime::LightComponent{}.innerConeRadians);
    ImGui::DestroyContext();
}

TEST_CASE("Every registered component field has both editor translations", "[editor][inspector][field-schema][localization]") {
    for (const auto *locale : {"en-US", "tr-TR"}) {
        INFO(locale);
        std::ifstream input{std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                            (std::string{locale} + ".json")};
        REQUIRE(input.good());
        const auto catalog = nlohmann::json::parse(input).at("messages");
        CheckCatalog<Runtime::CameraComponent>(catalog);
        CheckCatalog<Runtime::LightComponent>(catalog);
        CheckCatalog<Runtime::TriggerVolumeComponent>(catalog);
        CheckCatalog<Runtime::AudioSourceComponent>(catalog);
        CheckCatalog<Runtime::AudioListenerComponent>(catalog);
        CheckCatalog<Runtime::CharacterSceneMovement>(catalog);
        CheckCatalog<Runtime::CharacterSceneGeometry>(catalog);
        CheckCatalog<Runtime::RigidBodyComponent>(catalog);
        CheckCatalog<Runtime::PhysicsBoxCollider>(catalog);
        CheckCatalog<Runtime::PhysicsSphereCollider>(catalog);
        CheckCatalog<Runtime::PhysicsCapsuleCollider>(catalog);
        CheckCatalog<Runtime::PhysicsStaticPlaneCollider>(catalog);
        CheckCatalog<Runtime::PhysicsDistanceConstraint>(catalog);
    }
}

TEST_CASE("Declared text field edits complete through the shared control", "[editor][inspector][field-schema][interaction]") {
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.DisplaySize = {640, 480};
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());
    EngineDataBus engine;
    EditorDataBus events;
    Localization localization;
    auto *font = io.Fonts->Fonts.front();
    const ThemeContext theme{.fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font}};
    const EditorSettingsSnapshot settings{};
    const EditorGuiContext context{.engineEvents = engine,
                                   .editorEvents = events,
                                   .localization = localization,
                                   .theme = theme,
                                   .settings = settings};
    TestComponent model;
    ImVec2 position{};
    bool changed = false;
    bool committed = false;
    const auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({600, 400});
        ImGui::Begin("DeclaredText", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration);
        const auto edit =
            Fields::Draw(model, std::span<const Fields::Field<TestComponent>>{testFields.data() + 1, 1}, context, Fields::Layout::Stacked);
        changed |= edit.changed;
        committed |= edit.committed;
        position = ImGui::GetItemRectMin();
        position.x += 25;
        position.y += 10;
        ImGui::End();
        ImGui::Render();
    };
    io.ConfigMacOSXBehaviors = false;
    io.AddFocusEvent(true);
    frame();
    frame();
    io.AddMousePosEvent(position.x, position.y);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame();
    io.AddInputCharactersUTF8("Changed");
    frame();
    CHECK(model.name == "Changed");
    CHECK(changed);
    io.AddMousePosEvent(580, 350);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    CHECK(committed);
    CHECK(model.identity == TestComponent{}.identity);
    ImGui::DestroyContext();
}
