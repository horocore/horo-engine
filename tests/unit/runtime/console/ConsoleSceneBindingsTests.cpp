#include "Horo/Debug/ConsoleProduct.h"
#include "Horo/Debug/ConsoleSceneBindings.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

using namespace Horo;
using namespace Horo::Debug;

namespace {
    /** @brief Builds two ordinary public entities with a parent relationship and no native/backend requirements. */
    Runtime::RuntimeSceneDefinition Definition(std::uint64_t revision) {
        Runtime::SceneDefinitionBuilder builder({1}, {revision});
        Runtime::RuntimeEntityDefinition parent{.object = {1}};
        parent.localTransform.translation = {static_cast<float>(revision), 2, 3};
        builder.Add(std::move(parent));
        builder.Add(Runtime::RuntimeEntityDefinition{.object = {2}, .parent = Runtime::SceneObjectId{1}});
        auto built = std::move(builder).Build();
        REQUIRE(built.HasValue());
        return std::move(built).Value();
    }

    /** @brief Serializes the complete typed reference accepted by the descriptor's EntityId argument codec. */
    std::string Reference(Runtime::EntityRef entity) {
        return std::to_string(entity.runtime.value) + ":" + std::to_string(entity.entity.index) + ":" +
               std::to_string(entity.entity.generation);
    }
}  // namespace

TEST_CASE("Scene console bindings inspect real public state and reject a queued reference after replacement", "[console][scene]") {
    auto scene = std::make_shared<Runtime::RuntimeSceneService>(Runtime::SceneRuntimeId{41});
    REQUIRE(scene->Startup({}).HasValue());
    REQUIRE(scene->QueuePreparation(Definition(1)).HasValue());
    const Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, {}};
    REQUIRE(scene->OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
    const auto first = scene->ActiveScene();
    REQUIRE(first);
    const auto entity = first->EntityAt(0);
    REQUIRE(entity);
    const auto original = entity->entity;
    auto descriptors = ProductConsoleDescriptors();
    std::erase_if(descriptors, [](const auto &descriptor) {
        return descriptor.id != SceneTreeCommandId && descriptor.id != InspectCommandId;
    });
    RegistryPolicy policy;
    policy.profile = CompiledConsoleProfile();
    auto composed = CommandRegistry::Create(descriptors, policy);
    REQUIRE(composed.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(composed).Value());
    auto bindings = BindConsoleSceneCommands(registry, scene, {1, 1});
    REQUIRE(bindings.HasValue());
    REQUIRE(bindings.Value().size() == 2);
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(8, 8);
    auto made =
        CommandExecutor::Create(registry, bindings.Value(), jobs, operations, std::this_thread::get_id(), std::this_thread::get_id());
    REQUIRE(made.HasValue());
    auto executor = std::move(made).Value();
    CHECK(executor->Submit("game.scene_tree", {}).HasError());
    auto tree = executor->Submit("game.scene_tree", {3});
    REQUIRE(tree.HasValue());
    REQUIRE(executor->DrainOwner(1).HasValue());
    auto snapshot = operations->SnapshotIfChanged(0);
    REQUIRE(snapshot);
    REQUIRE(snapshot->operations.size() == 1);
    CHECK(snapshot->operations[0].message.find(Reference(original)) != std::string::npos);
    CHECK(snapshot->operations[0].message.find("[bounded view]") != std::string::npos);
    auto inspect = executor->Submit("game.inspect " + Reference(original), {3});
    REQUIRE(inspect.HasValue());
    REQUIRE(scene->QueuePreparation(Definition(2)).HasValue());
    REQUIRE(scene->OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
    const auto active = scene->ActiveScene();
    REQUIRE(active);
    auto stale = active->Get(original);
    REQUIRE(stale.HasError());
    REQUIRE(executor->DrainOwner(1).HasValue());
    snapshot = operations->SnapshotIfChanged(0);
    REQUIRE(snapshot);
    const auto failure = std::ranges::find(snapshot->operations, *inspect.Value().operation, &OperationRecord::id);
    REQUIRE(failure != snapshot->operations.end());
    CHECK(failure->state == OperationState::Failed);
    REQUIRE(failure->error);
    CHECK(failure->error->code.Value() == stale.ErrorValue().code.Value());
    CHECK(failure->error->domain.Value() == stale.ErrorValue().domain.Value());
    const auto replacement = active->EntityAt(0);
    REQUIRE(replacement);
    auto current = executor->Submit("game.inspect " + Reference(replacement->entity), {3});
    REQUIRE(current.HasValue());
    REQUIRE(executor->DrainOwner(1).HasValue());
    snapshot = operations->SnapshotIfChanged(0);
    REQUIRE(snapshot);
    const auto success = std::ranges::find(snapshot->operations, *current.Value().operation, &OperationRecord::id);
    REQUIRE(success != snapshot->operations.end());
    CHECK(success->state == OperationState::Succeeded);
    CHECK(success->message.find("translation=2,2,3") != std::string::npos);
    scene->Shutdown();
}
