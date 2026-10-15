#pragma once
#include "CharacterGameplayTestSupport.h"
#include "Horo/Gameplay/GameplayCharacterContext.h"

#if HORO_TEST_PHYSICS_NATIVE
namespace Horo::Tests::CharacterIntegration {
    /** @brief Real canonical Physics/Character owners with independently issued Gameplay grants and native module loading. */
    struct CharacterModuleHost final {
        Tests::CharacterIntegration::CanonicalCharacterHost world;
        std::shared_ptr<GameplayPhysicsContext> physics;
        std::shared_ptr<GameplayCharacterContext> character;
        std::uint64_t sceneId;

        explicit CharacterModuleHost(const std::uint64_t scene = 7) : sceneId(scene) {
            world.gameplay.reset();
            RefreshPhysics();
            const GameplayCharacterBinding
                binding{"game.tests", sceneId, 1, world.character->Descriptor().identity, world.physics->Identity(), true, true};
            auto created = GameplayCharacterContext::Create(binding, world.character.get(), physics.get());
            REQUIRE(created.HasValue());
            character = std::move(created).Value();
        }

        void RefreshPhysics() {
            auto created =
                GameplayPhysicsContext::Create({"game.tests", sceneId, 1, world.physics->Identity(), true, true}, world.physics.get());
            REQUIRE(created.HasValue());
            physics = std::move(created).Value();
        }

        std::unique_ptr<LoadedGameModule> Load() {
            GameModuleHost loader{GameModuleHostOptions{{}, {.physics = physics, .character = character}}};
            auto loaded = loader.Load(HORO_TEST_GAME_MODULE_PATH, {"game.tests", CurrentGameplayBuildFingerprint(),
                                                                   Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)});
            REQUIRE(loaded.HasValue());
            return std::move(loaded).Value();
        }
    };

}  // namespace Horo::Tests::CharacterIntegration
#endif
