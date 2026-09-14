#include "CharacterControllerRegistry.h"
#include "Horo/Physics/CharacterWorld.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Character {
    namespace {
        [[nodiscard]] CharacterWorldId WorldId(const std::uint64_t value = 71) {
            const auto result = CharacterWorldId::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        [[nodiscard]] Physics::PhysicsWorldId PhysicsWorldId(const std::uint64_t value = 81) {
            const auto result = Physics::PhysicsWorldId::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        [[nodiscard]] CharacterWorldPreparationDescriptor WorldDescriptor() {
            return {61, PhysicsWorldId(), 91, 101};
        }

        [[nodiscard]] CharacterWorldSettings Settings(const std::uint32_t maximumControllers = 2) {
            CharacterWorldSettingsDescriptor descriptor;
            descriptor.capacities.maximumControllers = maximumControllers;
            const auto result = CharacterWorldSettings::Capture(descriptor);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        [[nodiscard]] Physics::PhysicsQueryMaterial Material() {
            return {
                Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value(),
                3,
                Physics::PhysicsMaterialSlotId::FromValue(5),
            };
        }

        [[nodiscard]] CharacterControllerDescriptor ControllerDescriptor(const CharacterWorldDescriptor &world) {
            CharacterControllerDescriptor descriptor;
            descriptor.sceneGeneration = world.sceneGeneration;
            descriptor.characterWorld = world.identity;
            descriptor.physicsWorld = world.physicsWorld;
            descriptor.collisionProfile = Physics::CollisionProfileId::Parse("22345678-1234-4234-8234-123456789abc").Value();
            descriptor.queryChannel = Physics::PhysicsQueryChannelId::Parse("32345678-1234-4234-8234-123456789abc").Value();
            descriptor.defaultMaterial = Material();
            return descriptor;
        }

        using Physics::Test::RequireError;

        [[nodiscard]] std::unique_ptr<CharacterWorld> PreparedWorld(const std::uint32_t maximumControllers = 2) {
            const auto settings = Settings(maximumControllers);
            auto prepared = CharacterWorld::Prepare(WorldDescriptor(), settings);
            REQUIRE(prepared.HasValue());
            return std::move(prepared).Value();
        }

        struct ActiveWorld final {
            std::unique_ptr<CharacterWorld> world;
            std::array<CharacterControllerHandle, 2> controllers{};
        };

        [[nodiscard]] ActiveWorld ActiveWorldWithControllers(const std::uint32_t controllerCount = 1) {
            REQUIRE(controllerCount <= 2);
            ActiveWorld result{PreparedWorld(controllerCount)};
            const auto descriptor = ControllerDescriptor(result.world->Descriptor());
            for (std::uint32_t index = 0; index < controllerCount; ++index)
                result.controllers[index] = result.world->CreateController(descriptor).Value();
            REQUIRE(result.world->Activate().HasValue());
            return result;
        }

        [[nodiscard]] CharacterMovementRequest Movement(const CharacterControllerHandle controller, const std::uint64_t tick,
                                                        const std::uint64_t sequence) {
            return {.controller = controller, .tick = tick, .sequence = sequence};
        }

        [[nodiscard]] CharacterFixedTickInput FixedTick(const std::uint64_t tick, const CharacterTickObserver observer = {}) {
            return {.tick = tick,
                    .sceneGeneration = WorldDescriptor().sceneGeneration,
                    .fixedDelta = Duration::FromNanoseconds(16'666'667),
                    .observer = observer};
        }

        struct CommandTrace final {
            std::array<CharacterTickPhase, 3> phases{};
            std::array<CharacterMovementRequest, 2> movements{};
            std::size_t phaseCount{};
            std::size_t movementCount{};

            static void Phase(void *context, const CharacterTickPhase phase, const std::uint64_t) noexcept {
                auto &trace = *static_cast<CommandTrace *>(context);
                trace.phases[trace.phaseCount++] = phase;
            }

            static void Movement(void *context, const CharacterMovementRequest &request) noexcept {
                auto &trace = *static_cast<CommandTrace *>(context);
                trace.movements[trace.movementCount++] = request;
            }

            [[nodiscard]] CharacterTickObserver Observer() noexcept {
                return {.context = this, .phase = Phase, .movement = Movement};
            }
        };

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
        }

        TEST_CASE("Character fixed ticks canonically order commands and select the final replacement",
                  "[physics][character][world][command]") {
            auto [world, controllers] = ActiveWorldWithControllers(2);
            const auto [first, second] = controllers;

            REQUIRE(world->QueueMovementCommand(Movement(second, 1, 5)).Value().status == CharacterCommandAdmissionStatus::Deferred);
            REQUIRE(world->QueueMovementCommand(Movement(first, 1, 2)).Value().status == CharacterCommandAdmissionStatus::Deferred);
            REQUIRE(world->QueueMovementCommand(Movement(first, 1, 3)).Value().status == CharacterCommandAdmissionStatus::Deferred);

            CommandTrace trace;
            REQUIRE(world->AdvanceFixedTick(FixedTick(1, trace.Observer())).HasValue());
            REQUIRE(trace.phaseCount == 3);
            REQUIRE(trace.phases == std::array{CharacterTickPhase::FreezeCommands, CharacterTickPhase::ResolveMovement,
                                               CharacterTickPhase::PublishCompletedTick});
            REQUIRE(trace.movementCount == 2);
            REQUIRE(trace.movements[0].controller == first);
            REQUIRE(trace.movements[0].sequence == 3);
            REQUIRE(trace.movements[1].controller == second);
            REQUIRE(trace.movements[1].sequence == 5);
            REQUIRE((world->PublishedTick() == CharacterPublishedTick{1, 1, 2}));

            trace = {};
            REQUIRE(world->AdvanceFixedTick(FixedTick(2, trace.Observer())).HasValue());
            REQUIRE(trace.movementCount == 0);
            REQUIRE((world->PublishedTick() == CharacterPublishedTick{2, 2, 0}));
            RequireError(world->QueueMovementCommand(Movement(first, 2, 6)), CharacterErrors::CommandOrderInvalid);

            const auto statistics = world->TickStatistics();
            REQUIRE(statistics.completedTicks == 2);
            REQUIRE(statistics.admittedCommands == 3);
            REQUIRE(statistics.rejectedCommands == 1);
            REQUIRE(statistics.pendingCommands == 0);
            REQUIRE(statistics.maximumCommandDepth == 3);
        }

        TEST_CASE("Character fixed ticks reject foreign owner threads without closing the command frame",
                  "[physics][character][world][command][thread]") {
            auto [world, controllers] = ActiveWorldWithControllers();
            const auto handle = controllers.front();
            REQUIRE(world->QueueMovementCommand(Movement(handle, 1, 1)).HasValue());

            std::optional<Result<void>> tickResult;
            std::thread foreign([&] {
                tickResult = world->AdvanceFixedTick(FixedTick(1));
            });
            foreign.join();
            REQUIRE(tickResult.has_value());
            RequireError(*tickResult, CharacterErrors::InvalidState);
            REQUIRE((world->PublishedTick() == CharacterPublishedTick{}));
            REQUIRE(world->TickStatistics().pendingCommands == 1);
            REQUIRE(world->AdvanceFixedTick(FixedTick(1)).HasValue());
            REQUIRE((world->PublishedTick() == CharacterPublishedTick{1, 1, 1}));
        }

        TEST_CASE("Character command admission rejects duplicates and preserves an over-budget tick",
                  "[physics][character][world][command][capacity]") {
            CharacterWorldSettingsDescriptor values;
            values.capacities.maximumControllers = 2;
            values.capacities.maximumQueuedCommands = 2;
            values.work.maximumCommandsPerTick = 1;
            const auto settings = CharacterWorldSettings::Capture(values);
            REQUIRE(settings.HasValue());
            auto prepared = CharacterWorld::Prepare(WorldDescriptor(), settings.Value());
            REQUIRE(prepared.HasValue());
            auto world = std::move(prepared).Value();
            const auto first = world->CreateController(ControllerDescriptor(world->Descriptor())).Value();
            const auto second = world->CreateController(ControllerDescriptor(world->Descriptor())).Value();
            REQUIRE(world->Activate().HasValue());

            REQUIRE(world->QueueMovementCommand(Movement(first, 1, 1)).HasValue());
            RequireError(world->QueueMovementCommand(Movement(first, 1, 1)), CharacterErrors::CommandOrderInvalid);
            REQUIRE(world->QueueMovementCommand(Movement(second, 1, 1)).HasValue());
            REQUIRE(world->QueueMovementCommand(Movement(first, 2, 2)).Value().status == CharacterCommandAdmissionStatus::RejectedFull);
            RequireError(world->AdvanceFixedTick(FixedTick(1)), CharacterErrors::CapacityExceeded);
            REQUIRE((world->PublishedTick() == CharacterPublishedTick{}));
            REQUIRE(world->TickStatistics().pendingCommands == 2);

            RequireError(world->AdvanceFixedTick(FixedTick(2)), CharacterErrors::CommandOrderInvalid);
            auto invalid = FixedTick(1);
            invalid.fixedDelta = {};
            RequireError(world->AdvanceFixedTick(invalid), CharacterErrors::CommandOrderInvalid);
        }

        struct ReentrantProducer final {
            CharacterWorld *world{};
            CharacterMovementRequest closed;
            CharacterMovementRequest future;
            std::optional<Result<CharacterCommandAdmission>> closedResult;
            std::optional<Result<CharacterCommandAdmission>> futureResult;

            static void Movement(void *context, const CharacterMovementRequest &) noexcept {
                auto &producer = *static_cast<ReentrantProducer *>(context);
                producer.closedResult = producer.world->QueueMovementCommand(producer.closed);
                producer.futureResult = producer.world->QueueMovementCommand(producer.future);
            }
        };

        TEST_CASE("Character closes the current command frame before producer callbacks execute",
                  "[physics][character][world][command][thread]") {
            auto [world, controllers] = ActiveWorldWithControllers();
            const auto handle = controllers.front();
            REQUIRE(world->QueueMovementCommand(Movement(handle, 1, 1)).HasValue());

            ReentrantProducer producer{world.get(), Movement(handle, 1, 2), Movement(handle, 2, 2)};
            const CharacterTickObserver observer{.context = &producer, .movement = ReentrantProducer::Movement};
            REQUIRE(world->AdvanceFixedTick(FixedTick(1, observer)).HasValue());
            REQUIRE(producer.closedResult.has_value());
            RequireError(*producer.closedResult, CharacterErrors::CommandOrderInvalid);
            REQUIRE(producer.futureResult.has_value());
            REQUIRE(producer.futureResult->Value().status == CharacterCommandAdmissionStatus::Deferred);
            REQUIRE(world->TickStatistics().pendingCommands == 1);
            REQUIRE(world->AdvanceFixedTick(FixedTick(2)).HasValue());
            REQUIRE((world->PublishedTick() == CharacterPublishedTick{2, 2, 1}));
        }

        TEST_CASE("Concurrent Character producers receive bounded non-blocking admission outcomes",
                  "[physics][character][world][command][thread]") {
            constexpr std::size_t ProducerCount = 16;
            auto world = PreparedWorld(1);
            const auto handle = world->CreateController(ControllerDescriptor(world->Descriptor())).Value();
            REQUIRE(world->Activate().HasValue());

            std::barrier start{static_cast<std::ptrdiff_t>(ProducerCount + 1)};
            std::array<std::optional<Result<CharacterCommandAdmission>>, ProducerCount> results;
            std::vector<std::thread> producers;
            producers.reserve(ProducerCount);
            for (std::size_t index = 0; index < ProducerCount; ++index) {
                producers.emplace_back([&, index] {
                    start.arrive_and_wait();
                    results[index] = world->QueueMovementCommand(Movement(handle, 1, index + 1));
                });
            }
            start.arrive_and_wait();
            for (std::thread &producer : producers)
                producer.join();

            std::uint32_t deferred{};
            for (const auto &result : results) {
                REQUIRE(result.has_value());
                REQUIRE(result->HasValue());
                const auto status = result->Value().status;
                REQUIRE((status == CharacterCommandAdmissionStatus::Deferred || status == CharacterCommandAdmissionStatus::RejectedBusy));
                deferred += static_cast<std::uint32_t>(status == CharacterCommandAdmissionStatus::Deferred);
            }
            REQUIRE(deferred != 0);
            REQUIRE(world->AdvanceFixedTick(FixedTick(1)).HasValue());
            REQUIRE(world->PublishedTick().appliedCommands == 1);
            REQUIRE(world->TickStatistics().pendingCommands == 0);
        }

        struct TrackedRecord final {
            explicit TrackedRecord(std::uint32_t &live) noexcept : live_(&live) {
                ++*live_;
            }

            TrackedRecord(const TrackedRecord &) = delete;
            TrackedRecord &operator=(const TrackedRecord &) = delete;

            TrackedRecord(TrackedRecord &&other) noexcept : live_(std::exchange(other.live_, nullptr)) {}

            TrackedRecord &operator=(TrackedRecord &&) noexcept = delete;

            ~TrackedRecord() noexcept {
                Release();
            }

        private:
            void Release() noexcept {
                if (live_ != nullptr)
                    --*live_;
                live_ = nullptr;
            }

            std::uint32_t *live_{};
        };

        TEST_CASE("Character registry drains owned records and retires non-wrapping generations",
                  "[physics][character][world][lifecycle]") {
            using Registry = Detail::CharacterControllerRegistry<TrackedRecord>;
            std::uint32_t live{};
            Registry source{61, WorldId(), {.maximumSlots = 1, .maximumGeneration = 2}};
            auto registry = std::move(source);
            RequireError(source.Acquire(TrackedRecord{live}), CharacterErrors::CapacityExceeded);
            const auto first = registry.Acquire(TrackedRecord{live});
            REQUIRE(first.HasValue());
            REQUIRE(live == 1);
            REQUIRE(registry.Remove(first.Value()).HasValue());
            REQUIRE(live == 0);
            const auto last = registry.Acquire(TrackedRecord{live});
            REQUIRE(last.HasValue());
            REQUIRE(last.Value().slot.generation == 2);
            REQUIRE(registry.Remove(last.Value()).HasValue());
            REQUIRE(live == 0);
            RequireError(registry.Acquire(TrackedRecord{live}), CharacterErrors::GenerationExhausted);
            REQUIRE(live == 0);

            Registry drainable{61, WorldId(), {.maximumSlots = 1}};
            REQUIRE(drainable.Acquire(TrackedRecord{live}).HasValue());
            REQUIRE(live == 1);
            drainable.Drain();
            REQUIRE(live == 0);
            REQUIRE(drainable.Statistics().active == 0);
        }

    }  // namespace
}  // namespace Horo::Character
