#include "AllocationProbe.h"
#include "CharacterCapabilityInternal.h"
#include "CharacterWorldTestHelpers.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <type_traits>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        static_assert(std::is_default_constructible_v<Detail::CharacterCapabilityRegistry>);
        static_assert(!std::is_copy_constructible_v<Detail::CharacterCapabilityRegistry>);
        static_assert(!std::is_copy_assignable_v<Detail::CharacterCapabilityRegistry>);
        static_assert(!std::is_move_constructible_v<Detail::CharacterCapabilityRegistry>);
        static_assert(!std::is_move_assignable_v<Detail::CharacterCapabilityRegistry>);
        static_assert(std::is_copy_constructible_v<CharacterCapability>);
        static_assert(std::is_move_constructible_v<CharacterCapability>);

        /** @brief Exercises every public operation's common access fence without synthetic providers. */
        void RequireClosedClient(const CharacterCapability &client, const CharacterControllerHandle handle,
                                 const CharacterControllerDescriptor &descriptor, const ErrorCodeDescriptor &error) {
            RequireError(client.CreateController(descriptor), error);
            RequireError(client.QueueMovementCommand(Movement(handle, 1, 1)), error);
            RequireError(client.ControllerDescriptor(handle), error);
            RequireError(client.ControllerTransform(handle), error);
            RequireError(client.ControllerLocomotionSnapshot(handle), error);
        }

        /** @brief Exercises structural capability denial through real tick and placement callbacks. */
        struct ClientPhaseProbe final {
            CharacterWorld *world;
            CharacterCapability client;
            bool issueRejected{};
            bool createRejected{};

            void Check() {
                const auto issued = world->IssueCapability();
                const auto created = client.CreateController(ControllerDescriptor(world->Descriptor()));
                issueRejected = issued.HasError() && issued.ErrorValue().code.Value() == CharacterErrors::InvalidState.code.Value();
                createRejected = created.HasError() && created.ErrorValue().code.Value() == CharacterErrors::InvalidState.code.Value();
            }

            static void Phase(void *context, const CharacterTickPhase phase, std::uint64_t) noexcept {
                if (phase == CharacterTickPhase::FreezeCommands)
                    static_cast<ClientPhaseProbe *>(context)->Check();
            }

            static Result<CharacterOverlapProbeResult> Overlap(void *context, const CharacterOverlapProbeRequest &) noexcept {
                static_cast<ClientPhaseProbe *>(context)->Check();
                return Result<CharacterOverlapProbeResult>::Success({});
            }
        };

        /** @brief Performs actual capability issuance and prepared-owner creation for lifecycle/admission scenarios. */
        struct PreparedClientController final {
            explicit PreparedClientController(std::unique_ptr<CharacterWorld> owner)
                : world(std::move(owner)), client(world->IssueCapability().Value()), descriptor(ControllerDescriptor(world->Descriptor())),
                  handle(client.CreateController(descriptor).Value()) {}

            std::unique_ptr<CharacterWorld> world;
            CharacterCapability client;
            CharacterControllerDescriptor descriptor;
            CharacterControllerHandle handle;
        };

        TEST_CASE("Character grant registry exclusively retires retained borrows without destroying the owner",
                  "[character][capability][lifetime]") {
            auto world = PreparedWorld(1);
            std::shared_ptr<CharacterCapabilityState> retired;
            std::shared_ptr<CharacterCapabilityState> replacement;
            {
                Detail::CharacterCapabilityRegistry registry;
                retired = registry.Issue(*world, std::this_thread::get_id(), {}).Value();
                REQUIRE(retired->world == world.get());
                const auto identity = retired->identity;
                registry.Retire();
                registry.Retire();
                REQUIRE(retired->world == nullptr);
                REQUIRE(retired->identity.generation == identity.generation);
                replacement = registry.Issue(*world, std::this_thread::get_id(), {}).Value();
                REQUIRE(replacement->world == world.get());
                REQUIRE(replacement->identity.generation > identity.generation);
            }
            REQUIRE(retired->world == nullptr);
            REQUIRE(replacement->world == nullptr);
            REQUIRE(world->State() == CharacterWorldState::Prepared);
            REQUIRE(world->IssueCapability().HasValue());
        }

        TEST_CASE("Character grants are explicit inert and owner-phase creation preserves bounded slots", "[character][capability]") {
            auto world = PreparedWorld(1);
            const auto descriptor = ControllerDescriptor(world->Descriptor());
            const CharacterCapability absent;
            REQUIRE_FALSE(absent.Identity().world.IsValid());
            absent.Revoke();
            RequireClosedClient(absent, {}, descriptor, CharacterErrors::CapabilityUnavailable);
            auto client = world->IssueCapability().Value();
            REQUIRE(world->State() == CharacterWorldState::Prepared);
            REQUIRE(world->ActiveControllerCount() == 0);
            REQUIRE(client.Identity().world == world->Descriptor().identity);
            REQUIRE(client.Identity().physicsWorld == descriptor.physicsWorld);
            REQUIRE(client.Identity().sceneGeneration == descriptor.sceneGeneration);
            REQUIRE(client.Identity().generation != 0);
            auto invalid = descriptor;
            invalid.capsule.radiusMeters = 0;
            RequireError(client.CreateController(invalid), CharacterErrors::DescriptorInvalid);
            const auto handle = client.CreateController(descriptor).Value();
            REQUIRE(handle.slot.index == 0);
            RequireError(client.CreateController(descriptor), CharacterErrors::CapacityExceeded);
            REQUIRE(world->Activate().HasValue());
            RequireError(client.CreateController(descriptor), CharacterErrors::InvalidState);
            REQUIRE(world->ActiveControllerCount() == 1);
            auto moved = std::move(client);
            RequireClosedClient(client, handle, descriptor, CharacterErrors::CapabilityUnavailable);
            REQUIRE(moved.ControllerDescriptor(handle).HasValue());
        }

        TEST_CASE("Character capability preserves stale controller and foreign world errors", "[character][capability][generation]") {
            auto first = PreparedWorld(1);
            auto second = PreparedWorld(1);
            const auto client = first->IssueCapability().Value();
            const auto descriptor = ControllerDescriptor(first->Descriptor());
            RequireError(client.CreateController(ControllerDescriptor(second->Descriptor())), CharacterErrors::HandleWorldMismatch);
            const auto old = client.CreateController(descriptor).Value();
            REQUIRE(first->DestroyController(old).HasValue());
            const auto replacement = client.CreateController(descriptor).Value();
            REQUIRE(replacement.slot.index == old.slot.index);
            REQUIRE(replacement.slot.generation != old.slot.generation);
            RequireError(client.ControllerDescriptor(old), CharacterErrors::HandleStale);
            RequireError(client.ControllerTransform({}), CharacterErrors::HandleMalformed);
            const auto foreign = second->CreateController(ControllerDescriptor(second->Descriptor())).Value();
            RequireError(client.ControllerLocomotionSnapshot(foreign), CharacterErrors::HandleWorldMismatch);
            REQUIRE(first->Activate().HasValue());
            RequireError(client.QueueMovementCommand(Movement(old, 1, 1)), CharacterErrors::HandleStale);
        }

        TEST_CASE("Character client admission and copied publications retain exact tick sequence and capacity", "[character][capability]") {
            auto active = SpawnedActiveWorldWithController();
            auto &world = *active.world;
            const auto client = world.IssueCapability().Value();
            const auto handle = active.controller;
            RequireError(client.ControllerLocomotionSnapshot(handle), CharacterErrors::InvalidState);
            RequireError(client.QueueMovementCommand(Movement(handle, 0, 1)), CharacterErrors::RequestInvalid);
            RequireError(client.QueueMovementCommand(Movement(handle, 1, 0)), CharacterErrors::RequestInvalid);
            auto first = Movement(handle, 1, 1);
            first.desiredVelocityMetersPerSecond = Math::Vec3{1, 0, 0};
            REQUIRE(client.QueueMovementCommand(first).Value().status == CharacterCommandAdmissionStatus::Deferred);
            RequireError(client.QueueMovementCommand(first), CharacterErrors::CommandOrderInvalid);
            auto replacement = first;
            replacement.sequence = 2;
            replacement.desiredVelocityMetersPerSecond = Math::Vec3{2, 0, 0};
            REQUIRE(client.QueueMovementCommand(replacement).HasValue());
            CommandTrace trace;
            REQUIRE(world.AdvanceFixedTick(FixedTick(1, trace.Observer())).HasValue());
            REQUIRE(trace.movementCount == 1);
            REQUIRE(trace.movements[0].sequence == 2);
            auto snapshot = client.ControllerLocomotionSnapshot(handle).Value();
            REQUIRE(snapshot.tick == 1);
            REQUIRE(snapshot.movement.sequence == 2);
            REQUIRE(snapshot.transform.position == world.ControllerTransform(handle).Value().position);
            const auto committed = snapshot.transform.position;
            snapshot.transform.position = {100, 100, 100};
            REQUIRE(client.ControllerTransform(handle).Value().position == committed);
            RequireError(client.QueueMovementCommand(replacement), CharacterErrors::CommandOrderInvalid);
            REQUIRE(world.AdvanceFixedTick(FixedTick(2)).HasValue());
            REQUIRE(client.ControllerLocomotionSnapshot(handle).Value().tick == 1);
        }

        TEST_CASE("Character client closure and revocation preserve host debug publication identity", "[character][capability][debug]") {
            auto active = SpawnedActiveWorldWithController();
            auto &world = *active.world;
            const auto client = world.IssueCapability().Value();
            const auto &owner = world.Descriptor();
            const CharacterDebugCaptureRequest request{active.controller, owner.physicsWorld, owner.collisionFilterGeneration,
                                                       owner.originGeneration};
            REQUIRE(client.QueueMovementCommand(Movement(active.controller, 1, 1)).HasValue());
            REQUIRE(world.AdvanceFixedTick(FixedTick(1)).HasValue());
            const auto copied = client.ControllerLocomotionSnapshot(active.controller).Value();
            const auto captured = world.CaptureDebugSnapshot(request);
            REQUIRE(captured.status == CharacterDebugCaptureStatus::Captured);
            REQUIRE(captured.snapshot->Identity().sourceTick == copied.tick);
            REQUIRE(captured.snapshot->Identity().controllerStateRevision == copied.stateRevision);
            REQUIRE(captured.snapshot->Locomotion()->movement.sequence == copied.movement.sequence);
            REQUIRE(client.QueueMovementCommand(Movement(active.controller, 2, 2)).HasValue());
            client.Revoke();
            REQUIRE(world.AdvanceFixedTick(FixedTick(2)).HasValue());
            const auto retained = world.CaptureDebugSnapshot(request);
            REQUIRE(retained.status == CharacterDebugCaptureStatus::Captured);
            REQUIRE(retained.snapshot->Identity().sourceTick == copied.tick);
            REQUIRE(retained.snapshot->Identity().ageTicks == 1);
            REQUIRE(retained.snapshot->Identity().controllerStateRevision == copied.stateRevision);
            REQUIRE(retained.snapshot->Transform().position == copied.transform.position);
        }

        TEST_CASE("Character scoped queue exhaustion is reported and revoked future intents drain at closure",
                  "[character][capability][capacity]") {
            CharacterWorldSettingsDescriptor settings;
            settings.capacities.maximumControllers = 1;
            settings.capacities.maximumQueuedCommands = 2;
            settings.work.maximumCommandsPerTick = 2;
            PreparedClientController prepared(
                CharacterWorld::Prepare(WorldDescriptor(), CharacterWorldSettings::Capture(settings).Value()).Value());
            auto &world = prepared.world;
            const auto &client = prepared.client;
            const auto handle = prepared.handle;
            REQUIRE(world->Activate().HasValue());
            REQUIRE(client.QueueMovementCommand(Movement(handle, 2, 2)).HasValue());
            RequireError(client.QueueMovementCommand(Movement(handle, 1, 3)), CharacterErrors::CommandOrderInvalid);
            REQUIRE(client.QueueMovementCommand(Movement(handle, 1, 1)).HasValue());
            REQUIRE(client.QueueMovementCommand(Movement(handle, 3, 3)).Value().status == CharacterCommandAdmissionStatus::RejectedFull);
            REQUIRE(world->TickStatistics().commandOverflowCount == 1);
            REQUIRE(world->TickStatistics().pendingCommands == 2);
            client.Revoke();
            REQUIRE(world->AdvanceFixedTick(FixedTick(1)).HasValue());
            REQUIRE(world->TickStatistics().pendingCommands == 0);
            const auto replacement = world->IssueCapability().Value();
            REQUIRE(replacement.QueueMovementCommand(Movement(handle, 2, 1)).HasValue());
        }

        TEST_CASE("Character tick callbacks cannot issue grants or perform structural client creation", "[character][capability][phase]") {
            auto active = SpawnedActiveWorldWithController();
            const auto client = active.world->IssueCapability().Value();

            ClientPhaseProbe callback{active.world.get(), client};
            const CharacterTickObserver observer{.context = &callback, .phase = ClientPhaseProbe::Phase};
            REQUIRE(client.QueueMovementCommand(Movement(active.controller, 1, 1)).HasValue());
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(1, observer)).HasValue());
            REQUIRE(callback.issueRejected);
            REQUIRE(callback.createRejected);
            REQUIRE(active.world->ActiveControllerCount() == 1);
            REQUIRE(client.ControllerLocomotionSnapshot(active.controller).Value().movement.sequence == 1);
        }

        TEST_CASE("Character placement callbacks cannot issue grants or mutate client controller storage",
                  "[character][capability][phase][placement]") {
            auto active = ActiveWorldWithControllers(1);
            const auto client = active.world->IssueCapability().Value();

            ClientPhaseProbe placement{active.world.get(), client};

            OverlapProbe probe;
            auto query = probe.Context(active.world->Descriptor());
            query.context = &placement;
            query.overlap = ClientPhaseProbe::Overlap;
            const auto controller = active.controllers[0];
            REQUIRE(active.world->SpawnController(controller, query).HasValue());
            REQUIRE(placement.issueRejected);
            REQUIRE(placement.createRejected);
            REQUIRE(active.world->ActiveControllerCount() == 1);
            REQUIRE(client.ControllerTransform(controller).HasValue());
            REQUIRE(active.world->IssueCapability().HasValue());
        }

        TEST_CASE("Character client intents retain whole-frame rollback after a later controller fails",
                  "[character][capability][rollback]") {
            auto active = ActiveWorldWithControllers(2);
            OverlapProbe placement;
            const auto client = active.world->IssueCapability().Value();
            std::array<CharacterTransformPublication, 2> previous;
            for (std::size_t index = 0; index < active.controllers.size(); ++index) {
                const auto handle = active.controllers[index];
                REQUIRE(active.world->SpawnController(handle, placement.Context(active.world->Descriptor())).HasValue());
                previous[index] = client.ControllerTransform(handle).Value();
                REQUIRE(client.QueueMovementCommand(Movement(handle, 1, 1)).HasValue());
            }
            std::uint32_t calls{};
            CharacterTickObserver observer{.context = &calls};
            observer.movementResult = [](void *context, const CharacterMovementRequest &request,
                                         const CharacterTransformPublication &previous) noexcept -> Result<CharacterMovementResult> {
                if (++*static_cast<std::uint32_t *>(context) == 2)
                    return Result<CharacterMovementResult>::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
                CharacterMovementResult result;
                result.controller = request.controller;
                result.tick = request.tick;
                result.sequence = request.sequence;
                result.finalPosition = previous.position + Math::Vec3{1, 0, 0};
                result.finalHeading = previous.heading;
                result.up = previous.up;
                return Result<CharacterMovementResult>::Success(result);
            };
            RequireError(active.world->AdvanceFixedTick(FixedTick(1, observer)), Physics::PhysicsErrors::QuerySnapshotStale);
            REQUIRE(calls == 2);
            REQUIRE(active.world->PublishedTick().completedTick == 0);
            for (std::size_t index = 0; index < active.controllers.size(); ++index) {
                REQUIRE(client.ControllerTransform(active.controllers[index]).Value().position == previous[index].position);
                RequireError(client.ControllerLocomotionSnapshot(active.controllers[index]), CharacterErrors::InvalidState);
            }
        }

        TEST_CASE("Character revocation drops only unfrozen scoped intents and leaves direct producers intact",
                  "[character][capability][revocation]") {
            auto active = SpawnedActiveWorldWithController();
            auto &world = *active.world;
            CancellationSource module;
            const auto old = world.IssueCapability(module.Token()).Value();
            const auto copy = old;
            const auto direct = Movement(active.controller, 1, 1);
            REQUIRE(world.QueueMovementCommand(direct).HasValue());
            REQUIRE(old.QueueMovementCommand(Movement(active.controller, 1, 2)).HasValue());
            REQUIRE(old.QueueMovementCommand(Movement(active.controller, 2, 3)).HasValue());
            module.RequestCancellation();
            RequireClosedClient(copy, active.controller, ControllerDescriptor(world.Descriptor()), CharacterErrors::CapabilityRevoked);
            CommandTrace trace;
            REQUIRE(world.AdvanceFixedTick(FixedTick(1, trace.Observer())).HasValue());
            REQUIRE(trace.movementCount == 1);
            REQUIRE(trace.movements[0].sequence == direct.sequence);
            REQUIRE(world.TickStatistics().pendingCommands == 0);
            REQUIRE(world.AdvanceFixedTick(FixedTick(2)).HasValue());
            REQUIRE(world.ControllerLocomotionSnapshot(active.controller).Value().tick == 1);
            const auto fresh = world.IssueCapability().Value();
            REQUIRE(fresh.Identity().generation > old.Identity().generation);
            REQUIRE(fresh.QueueMovementCommand(Movement(active.controller, 3, 2)).HasValue());
            REQUIRE(world.AdvanceFixedTick(FixedTick(3)).HasValue());
        }

        TEST_CASE("Missing Character consumers do not cancel admitted intent or retain grant slots", "[character][capability][lifetime]") {
            auto active = SpawnedActiveWorldWithController();
            CharacterCapabilityIdentity expiredIdentity;
            {
                const auto consumer = active.world->IssueCapability().Value();
                expiredIdentity = consumer.Identity();
                REQUIRE(consumer.QueueMovementCommand(Movement(active.controller, 1, 1)).HasValue());
            }
            const auto replacement = active.world->IssueCapability().Value();
            REQUIRE(replacement.Identity().generation > expiredIdentity.generation);
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(1)).HasValue());
            const auto result = replacement.ControllerLocomotionSnapshot(active.controller).Value();
            REQUIRE(result.tick == 1);
            REQUIRE(result.movement.sequence == 1);
            REQUIRE(result.movement.controller == active.controller);
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(2)).HasValue());
            REQUIRE(replacement.ControllerLocomotionSnapshot(active.controller).Value().stateRevision == result.stateRevision);
        }

        TEST_CASE("Character command closure owns frozen intent despite callback revocation", "[character][capability][closure]") {
            auto active = SpawnedActiveWorldWithController();
            const auto client = active.world->IssueCapability().Value();
            REQUIRE(client.QueueMovementCommand(Movement(active.controller, 1, 1)).HasValue());

            struct AtClosure final {
                CharacterCapability client;
                bool called{};
            } context{client};

            CharacterTickObserver observer{.context = &context};
            observer.phase = [](void *state, CharacterTickPhase phase, std::uint64_t) noexcept {
                if (phase == CharacterTickPhase::FreezeCommands) {
                    auto &value = *static_cast<AtClosure *>(state);
                    value.called = true;
                    value.client.Revoke();
                }
            };
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(1, observer)).HasValue());
            REQUIRE(context.called);
            REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).Value().movement.sequence == 1);
            RequireError(client.ControllerTransform(active.controller), CharacterErrors::CapabilityRevoked);
        }

        TEST_CASE("Retained Character grants and owned snapshots survive shutdown and world replacement safely",
                  "[character][capability][lifetime]") {
            auto active = SpawnedActiveWorldWithController();
            const auto client = active.world->IssueCapability().Value();
            REQUIRE(client.QueueMovementCommand(Movement(active.controller, 1, 1)).HasValue());
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(1)).HasValue());
            const auto snapshot = client.ControllerLocomotionSnapshot(active.controller).Value();
            const auto descriptor = client.ControllerDescriptor(active.controller).Value();
            const auto identity = client.Identity();
            active.world->Shutdown();
            active.world->Shutdown();
            RequireClosedClient(client, active.controller, descriptor, CharacterErrors::CapabilityStale);
            active.world.reset();
            auto replacement = PreparedWorld(1);
            REQUIRE(replacement->Descriptor().identity != identity.world);
            RequireClosedClient(client, active.controller, descriptor, CharacterErrors::CapabilityStale);
            REQUIRE(snapshot.tick == 1);
            REQUIRE(snapshot.movement.controller == active.controller);
            REQUIRE(client.Identity().world == identity.world);
            client.Revoke();
            RequireError(client.ControllerTransform(active.controller), CharacterErrors::CapabilityRevoked);
        }

        TEST_CASE("Prepared owner destruction retires retained Character grants without explicit shutdown",
                  "[character][capability][lifetime]") {
            PreparedClientController prepared(PreparedWorld(1));
            auto &world = prepared.world;
            const auto &client = prepared.client;
            const auto &descriptor = prepared.descriptor;
            const auto handle = prepared.handle;
            const auto copied = client.ControllerDescriptor(handle).Value();
            const auto identity = client.Identity();
            world.reset();
            RequireClosedClient(client, handle, descriptor, CharacterErrors::CapabilityStale);
            REQUIRE(copied.characterWorld == identity.world);
            REQUIRE(copied.sceneGeneration == identity.sceneGeneration);
            auto replacement = PreparedWorld(1);
            const auto fresh = replacement->IssueCapability().Value();
            REQUIRE(fresh.Identity().world != identity.world);
            REQUIRE(fresh.CreateController(ControllerDescriptor(replacement->Descriptor())).HasValue());
            RequireClosedClient(client, handle, copied, CharacterErrors::CapabilityStale);
        }

        TEST_CASE("Character clients reject foreign-thread access while cancellation remains cross-thread safe",
                  "[character][capability][thread]") {
            PreparedClientController prepared(PreparedWorld(1));
            auto &world = prepared.world;
            const auto &client = prepared.client;
            const auto &descriptor = prepared.descriptor;
            const auto handle = prepared.handle;
            const auto identity = world->Descriptor().identity;
            std::array<bool, 6> rejected{};
            bool identityMatched{};
            std::thread worker([&] {
                const auto check = [](const auto &result) {
                    return result.HasError() && result.ErrorValue().code.Value() == CharacterErrors::ThreadAffinityViolation.code.Value();
                };
                rejected = {check(world->IssueCapability()),
                            check(client.CreateController(descriptor)),
                            check(client.QueueMovementCommand(Movement(handle, 1, 1))),
                            check(client.ControllerDescriptor(handle)),
                            check(client.ControllerTransform(handle)),
                            check(client.ControllerLocomotionSnapshot(handle))};
                identityMatched = client.Identity().world == identity;
                client.Revoke();
            });
            worker.join();
            REQUIRE(std::ranges::all_of(rejected, std::identity{}));
            REQUIRE(identityMatched);
            RequireError(client.ControllerDescriptor(handle), CharacterErrors::CapabilityRevoked);
            REQUIRE(world->ActiveControllerCount() == 1);
        }

        TEST_CASE("Character grant capacity and allocation rollback are finite and do not advance failed identities",
                  "[character][capability][capacity]") {
            auto world = PreparedWorld(1);
            const auto first = world->IssueCapability().Value();
            for (const std::size_t failure : {0U, 1U}) {
                Tests::AllocationProbe::ScopedFailure injection{failure};
                RequireError(world->IssueCapability(), CharacterErrors::CapacityExceeded);
            }
            std::array<CharacterCapability, MaximumCharacterCapabilitiesPerWorld> clients;
            clients[0] = first;
            for (std::size_t index = 1; index < clients.size(); ++index)
                clients[index] = world->IssueCapability().Value();
            REQUIRE(clients[1].Identity().generation == first.Identity().generation + 1);
            RequireError(world->IssueCapability(), CharacterErrors::CapacityExceeded);
            clients[0].Revoke();
            const auto next = world->IssueCapability().Value();
            REQUIRE(next.Identity().generation > clients.back().Identity().generation);
            RequireError(first.ControllerDescriptor({}), CharacterErrors::CapabilityRevoked);
            CancellationSource cancelled;
            cancelled.RequestCancellation();
            RequireError(world->IssueCapability(cancelled.Token()), CharacterErrors::CapabilityRevoked);
        }

        TEST_CASE("Character scoped admission tick and copied query fast paths do not allocate", "[character][capability][allocation]") {
            auto active = SpawnedActiveWorldWithController();
            const auto client = active.world->IssueCapability().Value();
            const auto command = Movement(active.controller, 1, 1);
            const auto before = Tests::AllocationProbe::Count();
            const auto admitted = client.QueueMovementCommand(command);
            const auto advanced = active.world->AdvanceFixedTick(FixedTick(1));
            const auto copied = client.ControllerLocomotionSnapshot(active.controller);
            const auto after = Tests::AllocationProbe::Count();
            REQUIRE(admitted.HasValue());
            REQUIRE(advanced.HasValue());
            REQUIRE(copied.HasValue());
            REQUIRE(after == before);
        }

#if HORO_TEST_PHYSICS_NATIVE
        TEST_CASE("Character capability forwards real canonical-query movement and preserves native typed failure",
                  "[character][capability][native]") {
            auto runtime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical).Value();
            auto physics = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
            const auto paired = PhysicsWorldId(967);
            REQUIRE(physics->Activate(paired).HasValue());
            REQUIRE(
                physics->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 61, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
            auto descriptor = WorldDescriptor();
            descriptor.physicsWorld = paired;
            descriptor.physicsSnapshotRevision = physics->PublishedTick().publicationRevision;
            PreparedClientController prepared(CharacterWorld::Prepare(descriptor, Settings(1)).Value());
            auto &world = prepared.world;
            const auto &client = prepared.client;
            const auto handle = prepared.handle;
            REQUIRE(world->Activate().HasValue());
            CharacterPhysicsQueryAdapter adapter(*physics);
            const CharacterPhysicsQueryExpectations expected{61, world->Descriptor().identity,      paired, 91, 101,
                                                             0,  descriptor.physicsSnapshotRevision};
            REQUIRE(world->SpawnController(handle, adapter.Context(expected)).HasValue());
            auto queryIdentity = expected;
            queryIdentity.tick = 1;
            auto input = FixedTick(1);
            input.query = adapter.Context(queryIdentity);
            REQUIRE(client.QueueMovementCommand(Movement(handle, 1, 1)).HasValue());
            REQUIRE(world->AdvanceFixedTick(input).HasValue());
            const auto retained = client.ControllerLocomotionSnapshot(handle).Value();
            REQUIRE(retained.tick == 1);
            REQUIRE(retained.movement.sequence == 1);
            physics->Shutdown();
            queryIdentity.tick = 2;
            input = FixedTick(2);
            input.query = adapter.Context(queryIdentity);
            REQUIRE(client.QueueMovementCommand(Movement(handle, 2, 2)).HasValue());
            RequireError(world->AdvanceFixedTick(input), Physics::PhysicsErrors::InvalidState);
            REQUIRE(world->PublishedTick().completedTick == 1);
            REQUIRE(client.ControllerLocomotionSnapshot(handle).Value().stateRevision == retained.stateRevision);
            world.reset();
            RequireError(client.ControllerTransform(handle), CharacterErrors::CapabilityStale);
        }
#endif
    }  // namespace
}  // namespace Horo::Character
