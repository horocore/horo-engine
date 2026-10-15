#include "GameplayCharacterContextTestSupport.h"
#include "Horo/Gameplay/GameplayCharacterContext.h"
#include "Horo/Physics/CharacterErrors.h"
#include "Horo/Platform/DynamicLibrary.h"

#include <cstring>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;
    using namespace Horo::Character;
    static_assert(std::is_constructible_v<GameModuleHost, std::vector<GameplayCapabilityId>>);
    static_assert(!std::is_default_constructible_v<GameModuleHostOptions>);

    TEST_CASE("Character host options preserve legacy empty braces and explicit missing-consumer errors", "[gameplay-character]") {
        GameModuleHost one{{}};
        GameModuleHost two{{}, {}};
        GameModuleHost three{{}, {}, {}};
        GameModuleHost typed{GameModuleHostOptions{{}, {}}};
        GameplayCharacterBinding binding{.moduleId = "game.tests", .scene = 7, .sceneGeneration = 1};
        Physics::Test::RequireError(GameplayCharacterContext::Create(binding, nullptr, nullptr), GameplayErrors::CharacterPermissionDenied);
        binding.permissionGranted = true;
        Physics::Test::RequireError(GameplayCharacterContext::Create(binding, nullptr, nullptr), GameplayErrors::CharacterUnavailable);
        const auto unavailable = GameplayCharacterContext::Withheld(binding, GameplayCharacterDenial::Unavailable);
        Physics::Test::RequireError(unavailable->Acquire("game.tests", 7, 1), GameplayErrors::CharacterUnavailable);
        const auto denied = GameplayCharacterContext::Withheld(binding, GameplayCharacterDenial::PermissionDenied);
        Physics::Test::RequireError(denied->Acquire("game.tests", 7, 1), GameplayErrors::CharacterPermissionDenied);
    }
#if HORO_TEST_PHYSICS_NATIVE
    using Tests::CharacterIntegration::CharacterModuleHost;

    TEST_CASE("Character factory admits only enabled exact dependencies and preserves typed Physics refusal",
              "[gameplay-character][native]") {
        CharacterModuleHost host;
        auto binding = host.character->Binding();
        binding.moduleEnabled = false;
        Physics::Test::RequireError(GameplayCharacterContext::Create(binding, host.world.character.get(), host.physics.get()),
                                    GameplayErrors::CharacterUnavailable);
        binding.moduleEnabled = true;
        ++binding.sceneGeneration;
        Physics::Test::RequireError(GameplayCharacterContext::Create(binding, host.world.character.get(), host.physics.get()),
                                    CharacterErrors::HandleWorldMismatch);
        --binding.sceneGeneration;
        auto denied = GameplayPhysicsContext::Withheld(host.physics->Binding(), GameplayPhysicsDenial::PermissionDenied);
        Physics::Test::RequireError(GameplayCharacterContext::Create(binding, host.world.character.get(), denied.get()),
                                    GameplayErrors::PhysicsPermissionDenied);
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        Physics::Test::RequireError(GameplayCharacterContext::Create(binding, host.world.character.get(), host.physics.get(),
                                                                     cancelled.Token()),
                                    CharacterErrors::CapabilityRevoked);
        host.physics->Revoke();
        Physics::Test::RequireError(GameplayCharacterContext::Create(binding, host.world.character.get(), host.physics.get()),
                                    Physics::PhysicsErrors::CapabilityRevoked);
    }

    TEST_CASE("Runtime Character admission rejects missing or revoked Physics before creating consumers", "[gameplay-character][native]") {
        CharacterModuleHost host;
        BehaviorRegistry registry;
        REQUIRE(registry.Freeze().HasValue());
        Physics::Test::RequireError(BehaviorRuntime::Create(*host.world.scene, registry, {}, {}, {}, host.character),
                                    GameplayErrors::CharacterUnavailable);
        host.physics->Revoke();
        Physics::Test::RequireError(BehaviorRuntime::Create(*host.world.scene, registry, {}, host.physics, {}, host.character),
                                    Physics::PhysicsErrors::CapabilityRevoked);
        REQUIRE(host.character->Acquire("game.tests", 7, 1).HasValue());
    }

    TEST_CASE("Native module Character startup reload and unload use fresh independently revocable scopes",
              "[gameplay-character][native][lifetime]") {
        CharacterModuleHost host;
        auto first = host.Load();
        const auto context = first->CharacterContext();
        REQUIRE(context != host.character);
        auto client = context->Acquire("game.tests", 7, 1).Value();
        Physics::Test::RequireError(context->Acquire("game.other", 7, 1), GameplayErrors::CharacterPermissionDenied);
        Physics::Test::RequireError(context->Acquire("game.tests", 8, 1), CharacterErrors::HandleWorldMismatch);
        const CharacterMovementRequest intent{.controller = host.world.controller,
                                              .tick = 1,
                                              .sequence = 968,
                                              .desiredVelocityMetersPerSecond = Math::Vec3{1, 0, 0}};
        REQUIRE(client.QueueMovementCommand(intent).HasValue());
        auto snapshot = first->PrepareReload();
        REQUIRE(snapshot.HasValue());  // The real native PrepareReload also verifies its Start-captured grant is revoked.
        Physics::Test::RequireError(client.Validate(), CharacterErrors::CapabilityRevoked);
        REQUIRE(host.character->Acquire("game.tests", 7, 1).HasValue());
        CharacterPhysicsQueryAdapter adapter{*host.world.physics};
        REQUIRE(host.world.character
                    ->AdvanceFixedTick({.tick = 1,
                                        .sceneGeneration = 1,
                                        .fixedDelta = Duration::FromNanoseconds(16'666'667),
                                        .query = adapter.Context(host.world.Expectations(1))})
                    .HasValue());
        REQUIRE(host.world.character->TickStatistics().pendingCommands == 0);
        Physics::Test::RequireError(host.world.character->ControllerLocomotionSnapshot(host.world.controller),
                                    CharacterErrors::InvalidState);
        first.reset();
        host.RefreshPhysics();
        auto replacement = host.Load();
        REQUIRE(replacement->RestoreReload(snapshot.Value()).HasValue());
        auto next = replacement->CharacterContext()->Acquire("game.tests", 7, 1).Value();
        REQUIRE(next.Identity().generation != client.Identity().generation);
        REQUIRE(next.ControllerDescriptor(host.world.controller).HasValue());
        replacement.reset();
        Physics::Test::RequireError(next.Validate(), CharacterErrors::CapabilityRevoked);
        REQUIRE(host.world.character->State() == CharacterWorldState::Active);
    }

    TEST_CASE("Character revocation precedes native reload lease rejection and wrapper withdrawal",
              "[gameplay-character][native][lifetime]") {
        CharacterModuleHost host;
        auto module = host.Load();
        auto grant = module->CharacterContext()->Acquire("game.tests", 7, 1).Value();
        auto created = BehaviorRuntime::Create(*host.world.scene, module->Registry(), {}, host.physics, {}, module->CharacterContext());
        REQUIRE(created.HasValue());
        auto behavior = std::move(created).Value();
        Physics::Test::RequireError(module->PrepareReload(), GameplayErrors::GameplayReloadRestartRequired);
        Physics::Test::RequireError(grant.Validate(), CharacterErrors::CapabilityRevoked);
        REQUIRE(host.character->Acquire("game.tests", 7, 1).HasValue());
        module.reset();  // A real native generation lease retains code, while Character authority is already closed.
        Physics::Test::RequireError(grant.Validate(), CharacterErrors::CapabilityRevoked);
        behavior->Shutdown();
        behavior.reset();
        REQUIRE(host.world.character->State() == CharacterWorldState::Active);
    }

    TEST_CASE("Application composition forwards explicitly selected Character access into a real native behavior",
              "[gameplay-character][native][host]") {
        Tests::CharacterIntegration::CanonicalCharacterHost world;
        world.gameplay.reset();
        const auto type = BehaviorTypeId::Parse("game.tests.character_client").Value();
        const auto definition =
            Tests::SingleBehaviorSceneDefinition({1}, {1}, {1}, {1}, type,
                                                 {{"slot", static_cast<std::int64_t>(world.controller.slot.index)},
                                                  {"generation", static_cast<std::int64_t>(world.controller.slot.generation)}});
        auto scene = Runtime::RuntimeScene::Create(definition, {9}).Value();
        const Application::Internal::GameplayWorldSelection selected{.moduleId = "game.tests",
                                                                     .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                                                                     .descriptorRevision =
                                                                         Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                                                                     .physicsPermission =
                                                                         Application::Internal::GameplayPhysicsPermission::Granted,
                                                                     .characterPermission =
                                                                         Application::Internal::GameplayCharacterPermission::Granted};
        auto created =
            Application::Internal::GameplayWorldComposition::Create(*scene, world.physics.get(), 1, selected, world.character.get());
        REQUIRE(created.HasValue());
        auto gameplay = std::move(created).Value();
        REQUIRE(gameplay->FixedUpdate({1.0 / 60.0}).HasValue());
        REQUIRE(scene->View().Get(*scene->View().Find({1})).Value().localTransform->translation.x == 21);
        auto retained = gameplay->CharacterContext()->Acquire("game.tests", 9, 1).Value();
        CharacterPhysicsQueryAdapter adapter{*world.physics};
        REQUIRE(world.character
                    ->AdvanceFixedTick({.tick = 1,
                                        .sceneGeneration = 1,
                                        .fixedDelta = Duration::FromNanoseconds(16'666'667),
                                        .query = adapter.Context(world.Expectations(1))})
                    .HasValue());
        REQUIRE(world.character->ControllerLocomotionSnapshot(world.controller).Value().movement.sequence == 968);
        gameplay->UnloadModule();
        Physics::Test::RequireError(retained.Validate(), CharacterErrors::CapabilityRevoked);
        REQUIRE(world.character->State() == CharacterWorldState::Active);
    }

    TEST_CASE("Module-derived Character client and weak registry storage remain host-owned after the native image unloads",
              "[gameplay-character][native][lifetime]") {
        CharacterModuleHost host;
        CharacterCapability escaped;
        {
            auto module = host.Load();
            auto loaded = Platform::LoadDynamicLibrary(module->LoadedArtifactPath().string());
            REQUIRE(loaded.HasValue());
            auto image = std::move(loaded).Value();
            void *symbol = image->GetSymbol("HoroTestCopyCharacterClient");
            REQUIRE(symbol != nullptr);
            using CopyClient = void (*)(CharacterCapability *);
            CopyClient copy{};
            static_assert(sizeof(copy) == sizeof(symbol));
            std::memcpy(&copy, &symbol, sizeof(copy));
            copy(&escaped);  // The actual module calls Derive; its issuer must dispatch back into host-owned allocation.
            REQUIRE(escaped.Validate().HasValue());
            REQUIRE(escaped.Identity().generation != module->CharacterContext()->Acquire("game.tests", 7, 1).Value().Identity().generation);
            module.reset();
            Physics::Test::RequireError(escaped.Validate(), CharacterErrors::CapabilityRevoked);
            copy = nullptr;
            image.reset();  // Releases the last native module image before escaped storage and its registry weak entry.
        }
        Physics::Test::RequireError(escaped.Validate(), CharacterErrors::CapabilityRevoked);
        escaped = {};
        REQUIRE(host.world.character->IssueCapability().HasValue());  // Reclaims expired weak entries after native unload.
        host.character.reset();
        host.world.character->Shutdown();  // Drains any remaining registry control blocks in the host image.
        REQUIRE(host.world.character->State() == CharacterWorldState::Destroyed);
    }
#endif
}  // namespace
