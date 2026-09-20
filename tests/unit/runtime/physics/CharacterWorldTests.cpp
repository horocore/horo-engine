#include "CharacterWorldTestHelpers.h"

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        TEST_CASE("Character world preparation captures exact ownership and capacity", "[physics][character][world]") {
            const auto settings = Settings(2);
            const auto owner = WorldDescriptor();
            auto prepared = CharacterWorld::Prepare(owner, settings);
            REQUIRE(prepared.HasValue());
            REQUIRE(prepared.Value()->State() == CharacterWorldState::Prepared);
            REQUIRE(prepared.Value()->Descriptor().sceneGeneration == owner.sceneGeneration);
            REQUIRE(prepared.Value()->Descriptor().physicsWorld == owner.physicsWorld);
            REQUIRE(prepared.Value()->Descriptor().collisionFilterGeneration == owner.collisionFilterGeneration);
            REQUIRE(prepared.Value()->Descriptor().originGeneration == owner.originGeneration);
            REQUIRE(prepared.Value()->Descriptor().physicsSnapshotRevision == owner.physicsSnapshotRevision);
            REQUIRE(prepared.Value()->Descriptor().identity.IsValid());
            REQUIRE(prepared.Value()->Settings().Identity() == settings.Identity());
            REQUIRE(prepared.Value()->ControllerCapacity() == 2);
            REQUIRE(prepared.Value()->ActiveControllerCount() == 0);

            auto invalid = owner;
            invalid.sceneGeneration = 0;
            RequireError(CharacterWorld::Prepare(invalid, settings), CharacterErrors::WorldInvalid);
            invalid = owner;
            invalid.collisionFilterGeneration = 0;
            RequireError(CharacterWorld::Prepare(invalid, settings), CharacterErrors::WorldInvalid);
            invalid = owner;
            invalid.physicsWorld = {};
            RequireError(CharacterWorld::Prepare(invalid, settings), CharacterErrors::WorldInvalid);
            invalid = owner;
            invalid.originGeneration = 0;
            RequireError(CharacterWorld::Prepare(invalid, settings), CharacterErrors::WorldInvalid);
            invalid = owner;
            invalid.physicsSnapshotRevision = 0;
            RequireError(CharacterWorld::Prepare(invalid, settings), CharacterErrors::WorldInvalid);
        }

        TEST_CASE("Character world deterministically reuses slots without aliasing stale handles",
                  "[physics][character][world][capacity]") {
            auto world = PreparedWorld();
            const auto descriptor = ControllerDescriptor(world->Descriptor());
            const auto first = world->CreateController(descriptor);
            const auto second = world->CreateController(descriptor);
            REQUIRE(first.HasValue());
            REQUIRE(second.HasValue());
            REQUIRE(first.Value().slot.index == 0);
            REQUIRE(second.Value().slot.index == 1);
            REQUIRE(first.Value().slot.generation == 1);
            RequireError(world->CreateController(descriptor), CharacterErrors::CapacityExceeded);

            REQUIRE(world->DestroyController(first.Value()).HasValue());
            RequireError(world->ControllerDescriptor(first.Value()), CharacterErrors::HandleStale);
            RequireError(world->DestroyController(first.Value()), CharacterErrors::HandleStale);

            const auto replacement = world->CreateController(descriptor);
            REQUIRE(replacement.HasValue());
            REQUIRE(replacement.Value().slot.index == first.Value().slot.index);
            REQUIRE(replacement.Value().slot.generation == first.Value().slot.generation + 1);
            RequireError(world->ControllerDescriptor(first.Value()), CharacterErrors::HandleStale);
            REQUIRE(world->ControllerDescriptor(replacement.Value()).Value().physicsWorld == descriptor.physicsWorld);
        }

        TEST_CASE("Character world rejects malformed foreign and over-budget controller creation transactionally",
                  "[physics][character][world]") {
            auto world = PreparedWorld();
            auto descriptor = ControllerDescriptor(world->Descriptor());
            descriptor.capsule.radiusMeters = 0;
            RequireError(world->CreateController(descriptor), CharacterErrors::DescriptorInvalid);
            REQUIRE(world->ActiveControllerCount() == 0);

            descriptor = ControllerDescriptor(world->Descriptor());
            descriptor.characterWorld = WorldId(72);
            RequireError(world->CreateController(descriptor), CharacterErrors::HandleWorldMismatch);
            descriptor = ControllerDescriptor(world->Descriptor());
            descriptor.physicsWorld = PhysicsWorldId(82);
            RequireError(world->CreateController(descriptor), CharacterErrors::HandleWorldMismatch);
            REQUIRE(world->ActiveControllerCount() == 0);

            CharacterWorldSettingsDescriptor settingsDescriptor;
            settingsDescriptor.capacities.maximumControllers = 1;
            settingsDescriptor.work.maximumContactsPerMovement = 1;
            const auto settings = CharacterWorldSettings::Capture(settingsDescriptor);
            REQUIRE(settings.HasValue());
            auto prepared = CharacterWorld::Prepare(WorldDescriptor(), settings.Value());
            REQUIRE(prepared.HasValue());
            RequireError(prepared.Value()->CreateController(ControllerDescriptor(prepared.Value()->Descriptor())),
                         CharacterErrors::CapacityExceeded);
            REQUIRE(prepared.Value()->ActiveControllerCount() == 0);
        }

        TEST_CASE("Character world activation and shutdown are explicit terminal lifecycle operations",
                  "[physics][character][world][lifecycle]") {
            auto world = PreparedWorld();
            const auto descriptor = ControllerDescriptor(world->Descriptor());
            const auto handle = world->CreateController(descriptor);
            REQUIRE(handle.HasValue());
            REQUIRE(world->Activate().HasValue());
            REQUIRE(world->State() == CharacterWorldState::Active);
            RequireError(world->Activate(), CharacterErrors::InvalidState);
            RequireError(world->CreateController(descriptor), CharacterErrors::InvalidState);
            RequireError(world->DestroyController(handle.Value()), CharacterErrors::InvalidState);

            world->Shutdown();
            REQUIRE(world->State() == CharacterWorldState::Destroyed);
            REQUIRE(world->ActiveControllerCount() == 0);
            world->Shutdown();
            RequireError(world->ControllerDescriptor(handle.Value()), CharacterErrors::InvalidState);
            RequireError(world->DestroyController(handle.Value()), CharacterErrors::InvalidState);
            RequireError(world->CreateController(descriptor), CharacterErrors::InvalidState);
            RequireError(world->Activate(), CharacterErrors::InvalidState);
        }

        TEST_CASE("Character spawn recovers bounded overlap and teleport publishes one detached root",
                  "[physics][character][world][placement]") {
            auto world = PreparedWorld();
            const auto controller = world->CreateController(ControllerDescriptor(world->Descriptor())).Value();
            REQUIRE(world->Activate().HasValue());

            OverlapProbe probe{.overlappingCalls = 2};
            const auto spawned = world->SpawnController(controller, probe.Context(world->Descriptor()));
            REQUIRE(spawned.HasValue());
            REQUIRE(spawned.Value().operation == CharacterPlacementOperation::Spawn);
            REQUIRE(spawned.Value().recovered);
            REQUIRE(spawned.Value().recoveryIterations == 2);
            REQUIRE(probe.calls == 3);
            REQUIRE(spawned.Value().publication.position == Math::Vec3{0, 2, 0});
            REQUIRE(spawned.Value().publication.groundingRevalidationRequired);
            REQUIRE_FALSE(spawned.Value().publication.grounded);
            REQUIRE_FALSE(spawned.Value().publication.platformAttached);

            const CharacterTeleportRequest teleport{controller, 1, {8, 4, -2}, Math::Quaternion::Identity()};
            const auto teleported = world->TeleportController(teleport, probe.Context(world->Descriptor(), teleport.tick));
            REQUIRE(teleported.HasValue());
            REQUIRE(teleported.Value().operation == CharacterPlacementOperation::Teleport);
            REQUIRE(teleported.Value().publication.position == teleport.targetPosition);
            REQUIRE(teleported.Value().publication.publicationRevision == spawned.Value().publication.publicationRevision + 1);
            REQUIRE_FALSE(teleported.Value().publication.grounded);
            REQUIRE_FALSE(teleported.Value().publication.platformAttached);
            REQUIRE(teleported.Value().publication.groundingRevalidationRequired);
            REQUIRE(world->ControllerTransform(controller).Value().position == teleport.targetPosition);
            RequireError(world->SpawnController(controller, probe.Context(world->Descriptor())), CharacterErrors::InvalidState);
        }

        TEST_CASE("Character spawn failure exhausts recovery without penetrating or publishing partial state",
                  "[physics][character][world][placement][failure]") {
            auto active = ActiveWorldWithControllers(1, 2);

            OverlapProbe probe{.alwaysOverlapping = true};
            RequireError(active.world->SpawnController(active.controllers[0], probe.Context(active.world->Descriptor())),
                         CharacterErrors::OverlapRecoveryFailed);
            REQUIRE(probe.calls == 3);
            RequireError(active.world->ControllerTransform(active.controllers[0]), CharacterErrors::InvalidState);

            probe = {};
            const auto retry = active.world->SpawnController(active.controllers[0], probe.Context(active.world->Descriptor()));
            REQUIRE(retry.HasValue());
            REQUIRE_FALSE(retry.Value().recovered);
            REQUIRE(retry.Value().recoveryIterations == 0);
        }

        TEST_CASE("Character placement contracts reject malformed evidence and stale lifecycle requests",
                  "[physics][character][world][placement][validation]") {
            auto world = PreparedWorld();
            const auto controller = world->CreateController(ControllerDescriptor(world->Descriptor())).Value();
            REQUIRE(world->Activate().HasValue());

            OverlapProbe probe;
            auto context = probe.Context(world->Descriptor());
            context.sceneGeneration += 1;
            RequireError(world->SpawnController(controller, context), CharacterErrors::HandleWorldMismatch);
            context = probe.Context(world->Descriptor());
            context.overlap = nullptr;
            RequireError(world->SpawnController(controller, context), CharacterErrors::OperationUnsupported);

            const CharacterTeleportRequest malformed{controller, 1, {0, 0, 0}, {0, 0, 0, 0}};
            RequireError(world->TeleportController(malformed, probe.Context(world->Descriptor(), malformed.tick)),
                         CharacterErrors::RequestInvalid);
        }

        TEST_CASE("Character placement reentrancy rejects removal and defers shutdown drain safely",
                  "[physics][character][world][placement][lifetime]") {
            auto world = PreparedWorld(1);
            const auto controller = world->CreateController(ControllerDescriptor(world->Descriptor())).Value();
            REQUIRE(world->Activate().HasValue());

            ReentrantPlacementProbe removal{world.get()};
            const auto recovered = world->SpawnController(controller, removal.Context(world->Descriptor()));
            REQUIRE(recovered.HasValue());
            REQUIRE(removal.destroyResult.has_value());
            RequireError(*removal.destroyResult, CharacterErrors::InvalidState);
            REQUIRE(world->ControllerTransform(controller).HasValue());

            auto shutdownWorld = PreparedWorld(1);
            const auto shutdownController = shutdownWorld->CreateController(ControllerDescriptor(shutdownWorld->Descriptor())).Value();
            REQUIRE(shutdownWorld->Activate().HasValue());
            ReentrantPlacementProbe shutdown{shutdownWorld.get(), {}, true};
            const auto rejected = shutdownWorld->SpawnController(shutdownController, shutdown.Context(shutdownWorld->Descriptor()));
            RequireError(rejected, CharacterErrors::InvalidState);
            REQUIRE(shutdown.destroyResult.has_value());
            RequireError(*shutdown.destroyResult, CharacterErrors::InvalidState);
            REQUIRE(shutdownWorld->State() == CharacterWorldState::Destroyed);
            REQUIRE(shutdownWorld->ActiveControllerCount() == 0);
            RequireError(shutdownWorld->ControllerTransform(shutdownController), CharacterErrors::InvalidState);
        }

        TEST_CASE("Character recovery budgets admit the final clearance probe deterministically",
                  "[physics][character][world][placement][recovery]") {
            for (const std::uint32_t budget : {0U, 1U, CharacterWorldSettingLimits::MaximumRecoveryIterations}) {
                auto active = ActiveWorldWithControllers(1, budget);
                OverlapProbe probe{.overlappingCalls = budget};
                const auto result = active.world->SpawnController(active.controllers[0], probe.Context(active.world->Descriptor()));
                REQUIRE(result.HasValue());
                REQUIRE(result.Value().recoveryIterations == budget);
                REQUIRE(probe.calls == budget + 1);
            }
        }

        TEST_CASE("Character teleports reserve the exact next tick and require clear bounded targets",
                  "[physics][character][world][placement][command-order]") {
            auto active = SpawnedActiveWorldWithController();
            auto &world = active.world;
            const auto controller = active.controller;
            OverlapProbe probe;

            const CharacterTeleportRequest future{controller, 2, {1, 0, 0}, Math::Quaternion::Identity()};
            RequireError(world->TeleportController(future, probe.Context(world->Descriptor(), 2)), CharacterErrors::CommandOrderInvalid);
            const CharacterTeleportRequest first{controller, 1, {1, 0, 0}, Math::Quaternion::Identity()};
            REQUIRE(world->TeleportController(first, probe.Context(world->Descriptor(), 1)).HasValue());
            RequireError(world->TeleportController(first, probe.Context(world->Descriptor(), 1)), CharacterErrors::CommandOrderInvalid);
            RequireError(world->QueueMovementCommand(Movement(controller, 1, 1)), CharacterErrors::CommandOrderInvalid);
            REQUIRE(world->AdvanceFixedTick(FixedTick(1)).HasValue());
            RequireError(world->TeleportController(first, probe.Context(world->Descriptor(), 1)), CharacterErrors::CommandOrderInvalid);
            const CharacterTeleportRequest second{controller, 2, {2, 0, 0}, Math::Quaternion::Identity()};
            REQUIRE(world->TeleportController(second, probe.Context(world->Descriptor(), 2)).HasValue());

            std::optional<Result<CharacterPlacementResult>> foreignResult;
            std::thread foreign([&] {
                foreignResult = world->TeleportController(CharacterTeleportRequest{controller, 3, {3, 0, 0}, Math::Quaternion::Identity()},
                                                          probe.Context(world->Descriptor(), 3));
            });
            foreign.join();
            REQUIRE(foreignResult.has_value());
            RequireError(*foreignResult, CharacterErrors::InvalidState);
        }

        TEST_CASE("Character placement rejects stale, out-of-envelope, oversized and overlapping targets",
                  "[physics][character][world][placement][bounds]") {
            auto outOfEnvelopeWorld = PreparedWorld(1);
            auto outOfEnvelopeDescriptor = ControllerDescriptor(outOfEnvelopeWorld->Descriptor());
            outOfEnvelopeDescriptor.collisionRootPosition = {Physics::MaximumPhysicsLocalHalfExtentMeters + 1.0F, 0, 0};
            const auto outOfEnvelopeController = outOfEnvelopeWorld->CreateController(outOfEnvelopeDescriptor).Value();
            REQUIRE(outOfEnvelopeWorld->Activate().HasValue());
            OverlapProbe outOfEnvelopeProbe;
            RequireError(outOfEnvelopeWorld->SpawnController(outOfEnvelopeController,
                                                             outOfEnvelopeProbe.Context(outOfEnvelopeWorld->Descriptor())),
                         CharacterErrors::PlacementInvalid);

            auto active = SpawnedActiveWorldWithController();
            auto &world = active.world;
            const auto controller = active.controller;
            OverlapProbe probe;
            const CharacterTeleportRequest oversized{controller, 1, {200, 0, 0}, Math::Quaternion::Identity()};
            RequireError(world->TeleportController(oversized, probe.Context(world->Descriptor(), 1)), CharacterErrors::PlacementInvalid);
            probe = OverlapProbe{.overlappingCalls = 1};
            const CharacterTeleportRequest overlapping{controller, 1, {1, 0, 0}, Math::Quaternion::Identity()};
            RequireError(world->TeleportController(overlapping, probe.Context(world->Descriptor(), 1)), CharacterErrors::PlacementInvalid);
            OverlapProbe nonFinite{.overlappingCalls = 1, .recoveryDisplacement = {std::numeric_limits<float>::quiet_NaN(), 0, 0}};
            const auto nonFiniteResult = world->TeleportController(overlapping, nonFinite.Context(world->Descriptor(), overlapping.tick));
            RequireError(nonFiniteResult, CharacterErrors::PlacementInvalid);
            REQUIRE(nonFiniteResult.ErrorValue().message == "Overlap recovery displacement must be finite.");
            REQUIRE(world->ControllerTransform(controller).Value().position == Math::Vec3{});
        }

        TEST_CASE("Character overlap recovery rejects zero and non-finite depenetration evidence",
                  "[physics][character][world][placement][recovery][validation]") {
            for (const Math::Vec3 displacement : {Math::Vec3{}, {std::numeric_limits<float>::quiet_NaN(), 0, 0}, {200, 0, 0}}) {
                auto world = PreparedWorld(1);
                const auto controller = world->CreateController(ControllerDescriptor(world->Descriptor())).Value();
                REQUIRE(world->Activate().HasValue());
                OverlapProbe probe{.overlappingCalls = 1, .recoveryDisplacement = displacement};
                const auto result = world->SpawnController(controller, probe.Context(world->Descriptor()));
                if (Math::IsFinite(displacement) && displacement != Math::Vec3{})
                    RequireError(result, CharacterErrors::PlacementInvalid);
                else if (!Math::IsFinite(displacement))
                    RequireError(result, CharacterErrors::PlacementInvalid);
                else
                    RequireError(result, CharacterErrors::OverlapRecoveryFailed);
                REQUIRE_FALSE(world->ControllerTransform(controller).HasValue());
            }
        }

        TEST_CASE("Character worlds issue distinct owner generations and reject cross-world aliases",
                  "[physics][character][world][identity]") {
            auto first = PreparedWorld(1);
            auto second = PreparedWorld(1);
            REQUIRE(first->Descriptor().identity != second->Descriptor().identity);
            const auto firstHandle = first->CreateController(ControllerDescriptor(first->Descriptor()));
            const auto secondHandle = second->CreateController(ControllerDescriptor(second->Descriptor()));
            REQUIRE(firstHandle.HasValue());
            REQUIRE(secondHandle.HasValue());
            REQUIRE(firstHandle.Value().slot == secondHandle.Value().slot);
            RequireError(first->ControllerDescriptor(secondHandle.Value()), CharacterErrors::HandleWorldMismatch);
            RequireError(second->ControllerDescriptor(firstHandle.Value()), CharacterErrors::HandleWorldMismatch);
        }

        TEST_CASE("Character world rejects prepared mutation from a foreign thread without changing storage",
                  "[physics][character][world][thread]") {
            auto world = PreparedWorld(1);
            const auto descriptor = ControllerDescriptor(world->Descriptor());
            std::optional<Result<void>> activationResult;
            std::thread activation([&] {
                activationResult = world->Activate();
            });
            activation.join();
            REQUIRE(activationResult.has_value());
            RequireError(*activationResult, CharacterErrors::InvalidState);
            REQUIRE(world->State() == CharacterWorldState::Prepared);

            std::optional<Result<CharacterControllerHandle>> creationResult;
            std::thread foreign([&] {
                creationResult = world->CreateController(descriptor);
            });
            foreign.join();
            REQUIRE(creationResult.has_value());
            RequireError(*creationResult, CharacterErrors::InvalidState);
            REQUIRE(world->ActiveControllerCount() == 0);

            std::thread foreignShutdown([&] {
                world->Shutdown();
            });
            foreignShutdown.join();
            REQUIRE(world->State() == CharacterWorldState::Prepared);
        }

    }  // namespace
}  // namespace Horo::Character
