#include "editor/screens/workspace/panels/inspector/add_component/AddComponentMenu.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <unordered_set>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    int availabilityCalls{};

    bool Available(const AddComponentContext &) noexcept {
        ++availabilityCalls;
        return true;
    }

    Gameplay::BehaviorDescriptor Behavior(const char *id, const char *name, const bool multiple = false) {
        Gameplay::BehaviorDescriptor result;
        result.typeId = Gameplay::BehaviorTypeId::Parse(id).Value();
        result.displayName = name;
        result.allowMultiple = multiple;
        return result;
    }
}  // namespace

TEST_CASE("Add Component definitions are inert unique metadata and validate explicit registration", "[editor][component-registry]") {
    AddComponentRegistry registry;
    std::unordered_set<std::string> ids;
    for (const auto &category : registry.Categories())
        for (const auto &entry : registry.Entries(category.id)) {
            CHECK(entry.localized);
            CHECK_FALSE(entry.label.empty());
            CHECK(ids.insert(entry.id).second);
        }
    CHECK(ids.size() == BuiltinAddComponentDefinitions().size());
    availabilityCalls = 0;
    const BuiltinAddComponentDefinition custom{"extension.test_light", "workspace.inspector.kind.light", AddComponentCategory::Rendering,
                                               ComponentType::Light, Available};
    REQUIRE(registry.RegisterStatic(custom).HasValue());
    CHECK(availabilityCalls == 0);
    REQUIRE(registry.Find(custom.id));
    CHECK(registry.RegisterStatic(custom).HasError());
    auto invalid = custom;
    invalid.id = "behavior.reserved";
    CHECK(registry.RegisterStatic(invalid).HasError());
    invalid.id = "extension.invalid";
    invalid.available = nullptr;
    CHECK(registry.RegisterStatic(invalid).HasError());
    invalid.available = Available;
    invalid.category = AddComponentCategory::Count;
    CHECK(registry.RegisterStatic(invalid).HasError());
    CHECK(availabilityCalls == 0);
    SceneObject object{.id = {1}, .name = "Object"};
    PhysicsInspectorDraft physics;
    const AddComponentContext context{object, physics, true, true};
    CHECK(registry.IsAvailable(*registry.Find(custom.id), context));
    CHECK(availabilityCalls == 1);
}

TEST_CASE("Project scripts replace menu contributions by project and owner revision", "[editor][component-registry]") {
    AddComponentRegistry registry;
    std::array scripts{Behavior("game.tests.jump", "Jump"), Behavior("game.tests.run", "Run", true)};
    REQUIRE(registry.SynchronizeBehaviors("project-a", 1, scripts).HasValue());
    REQUIRE(registry.Find("behavior.game.tests.jump"));
    const auto *storage = registry.Entries(AddComponentCategory::Scripts).data();
    REQUIRE(registry.SynchronizeBehaviors("project-a", 1, scripts).HasValue());
    CHECK(registry.Entries(AddComponentCategory::Scripts).data() == storage);
    CHECK(registry.Find("behavior.game.tests.jump")->label == "Jump");
    CHECK_FALSE(registry.Find("behavior.game.tests.jump")->localized);
    scripts.front().displayName = "Renamed Jump";
    REQUIRE(registry.SynchronizeBehaviors("project-a", 2, scripts).HasValue());
    CHECK(registry.Find("behavior.game.tests.jump")->label == "Renamed Jump");
    std::array replacement{Behavior("game.other.fly", "Fly")};
    REQUIRE(registry.SynchronizeBehaviors("project-b", 2, replacement).HasValue());
    CHECK_FALSE(registry.Find("behavior.game.tests.jump"));
    REQUIRE(registry.Find("behavior.game.other.fly"));
    CHECK(registry.Find("workflow.create_script"));
    const std::array duplicates{replacement.front(), replacement.front()};
    CHECK(registry.SynchronizeBehaviors("project-b", 3, duplicates).HasError());
    CHECK(registry.Find("behavior.game.other.fly"));
    CHECK(registry.SynchronizeBehaviors("", 4, replacement).HasError());
    registry.ClearProjectBehaviors();
    CHECK_FALSE(registry.Find("behavior.game.other.fly"));
    CHECK(registry.Find("workflow.create_script"));
    REQUIRE(registry.SynchronizeBehaviors("project-b", 2, {}).HasValue());
    CHECK(registry.Entries(AddComponentCategory::Scripts).size() == 1);
}

TEST_CASE("Registry availability follows current components locks multiplicity and Physics drafts", "[editor][component-registry]") {
    AddComponentRegistry registry;
    const std::array scripts{Behavior("game.tests.single", "Single"), Behavior("game.tests.multi", "Multi", true)};
    REQUIRE(registry.SynchronizeBehaviors("project", 1, scripts).HasValue());
    SceneObject object{.id = {7}, .name = "Box"};
    PhysicsInspectorDraft physics;
    AddComponentContext context{object, physics, true, true};
    const auto available = [&](const char *id) {
        const auto *entry = registry.Find(id);
        REQUIRE(entry);
        return registry.IsAvailable(*entry, context);
    };
    CHECK(available("component.camera"));
    object.components.camera.emplace();
    CHECK_FALSE(available("component.camera"));
    CHECK_FALSE(available("physics.joint"));
    CHECK(available("physics.body"));
    physics.body.emplace();
    CHECK_FALSE(available("physics.body"));
    CHECK(available("physics.joint"));
    object.components.behaviors.push_back({{1}, scripts[0].typeId});
    object.components.behaviors.push_back({{2}, scripts[1].typeId});
    CHECK_FALSE(available("behavior.game.tests.single"));
    CHECK(available("behavior.game.tests.multi"));
    context.physicsEditable = false;
    CHECK_FALSE(available("physics.box_collider"));
    object.effectivelyLocked = true;
    CHECK_FALSE(available("component.light"));
    CHECK_FALSE(available("behavior.game.tests.multi"));
    CHECK(available("workflow.create_script"));
    context.hasProject = false;
    CHECK_FALSE(available("workflow.create_script"));
    context.canIssueCommand = false;
    CHECK_FALSE(available("component.audio_source"));
}

TEST_CASE("Registry actions retain canonical commands and detached Physics ownership", "[editor][component-registry]") {
    SceneObject object{.id = {7}, .name = "Box"};
    PhysicsInspectorSession physics;
    physics.Begin(object, {}, 1);
    EditorWorkspaceViewCommandData command;
    ApplyAddComponentAction(ComponentType::Camera, object, "/project", physics, command);
    CHECK(command.command == EditorWorkspaceViewCommand::AddComponentToObject);
    CHECK(command.objectPayload == object.id);
    CHECK(command.componentTypePayload == ComponentType::Camera);
    CHECK_FALSE(object.components.camera.has_value());
    command = {};
    ApplyAddComponentAction(AddPhysicsComponent::TriggerCollider, object, "/project", physics, command);
    CHECK(command.command == EditorWorkspaceViewCommand::None);
    REQUIRE(physics.Draft().body.has_value());
    REQUIRE(physics.Draft().colliders.size() == 1);
    CHECK(physics.Draft().colliders.front().value.sensor);
    CHECK(object.components.colliders.empty());
    CHECK_FALSE(object.components.rigidBody.has_value());
    CHECK(physics.Request().HasError());
    ApplyAddComponentAction(AddComponentWorkflow::CreateScript, object, "/projects/My Game", physics, command);
    CHECK(command.command == EditorWorkspaceViewCommand::CreateLuaBehavior);
    REQUIRE(command.stringPayload);
    CHECK(std::filesystem::path{*command.stringPayload} == std::filesystem::path{"/projects/My Game/Assets/Scripts"});
}
