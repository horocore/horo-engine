#pragma once

#include "Horo/Assets/AssetProvider.h"
#include "RuntimeSceneTestSupport.h"

#include <array>
#include <chrono>
#include <thread>

namespace RuntimeSceneTestSupport {
    /** @brief Test-only material envelope loaded through the ordinary asynchronous Scene boundary. */
    struct ReplicaCharacterAssets final {
        static constexpr std::array<std::uint8_t, 16> identity{1};
        static constexpr Assets::AssetId material = Assets::AssetId::FromBytes(identity);

        Assets::AssetRegistry registry;
        Assets::MemoryAssetProvider provider;
        JobSystem jobs{{1, 4}};
        Assets::AssetLoadService loads{jobs, provider};

        ReplicaCharacterAssets() {
            const Assets::AssetRecord record{material, Assets::AssetTypeId::Parse("core.physics_material").Value(),
                                             ProjectPath::Parse("assets/replica-material.horoasset").Value(),
                                             ProjectPath::Parse("assets/replica-material.horoasset.horo").Value()};
            REQUIRE(registry.Publish({record}).status == Assets::AssetRegistryBuildStatus::Complete);
            provider.Insert(material, {1});
        }
    };

    /** @brief An authored Character with a child, used without a native owner in replica routing tests. */
    inline RuntimeSceneDefinition ReplicaCharacterDefinition() {
        SceneDefinitionBuilder builder{SceneDefinitionId{8}, SceneDefinitionRevision{1}};
        auto actor = Entity(1);
        actor.components.characterController = CharacterControllerComponent{.id = {7}, .enabled = true};
        auto &collision = actor.components.characterController->collision;
        collision.profile = Physics::CollisionProfileId::FromBytes(ReplicaCharacterAssets::identity);
        collision.queryChannel = Physics::PhysicsQueryChannelId::FromBytes(ReplicaCharacterAssets::identity);
        collision.defaultMaterial =
            PhysicsColliderMaterialBinding{Physics::PhysicsMaterialSlotId::FromValue(1), ReplicaCharacterAssets::material};
        builder.Add(std::move(actor));
        builder.Add(Entity(2, 1));
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        return std::move(definition).Value();
    }

    /** @brief Completes ordinary or resource-bearing activation through the Scene phase boundary. */
    inline void ActivateReplicaTestScene(RuntimeSceneService &service, const RuntimeSceneDefinition &definition) {
        const auto queued = service.QueuePreparation(definition);
        INFO((queued.HasError() ? queued.ErrorValue().message : "replica preparation queued"));
        REQUIRE(queued.HasValue());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!service.ActiveScene() && std::chrono::steady_clock::now() < deadline) {
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context({})).HasValue());
            const auto error = service.TakeOperationError();
            INFO((error ? error->message : "replica activation pending"));
            REQUIRE_FALSE(error);
            if (!service.ActiveScene())
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        REQUIRE(service.ActiveScene());
    }
}  // namespace RuntimeSceneTestSupport
