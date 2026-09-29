#include "AllocationProbe.h"
#include "CharacterWorldTestHelpers.h"
#include "Horo/Physics/CharacterMetrics.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace Horo::Character {
    namespace {
        struct SweepProbe final {
            std::uint32_t calls{};

            static Result<CharacterSweepProbeResult> Sweep(void *context, const CharacterSweepProbeRequest &) noexcept {
                ++static_cast<SweepProbe *>(context)->calls;
                return Result<CharacterSweepProbeResult>::Success({});
            }

            [[nodiscard]] CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &world, const std::uint64_t tick) noexcept {
                return {world.sceneGeneration,
                        world.identity,
                        world.physicsWorld,
                        this,
                        {},
                        world.collisionFilterGeneration,
                        world.originGeneration,
                        tick,
                        world.physicsSnapshotRevision,
                        Sweep};
            }
        };

        Result<CharacterMovementResult> FailMovement(void *, const CharacterMovementRequest &,
                                                     const CharacterTransformPublication &) noexcept {
            return Result<CharacterMovementResult>::Failure(MakeError(CharacterErrors::PlacementInvalid));
        }

        [[nodiscard]] Result<CharacterMetricBinding> OffBinding(const CharacterWorld &world) {
            const auto &settings = world.Settings().Values();
            return CharacterMetricBinding::Create(world.Descriptor().identity, world.Descriptor().sceneGeneration, 7,
                                                  {.maximumControllers = settings.capacities.maximumControllers,
                                                   .maximumQueriesPerTick = settings.work.maximumQueriesPerTick,
                                                   .maximumMovementIterations = settings.work.maximumMovementIterations,
                                                   .maximumContactsPerMovement = settings.work.maximumContactsPerMovement},
                                                  Telemetry::MetricCollectionLevel::Off, {});
        }

        class MetricSink final : public Telemetry::ISink {
        public:
            void Export(const Telemetry::Record &, const Telemetry::InstrumentDescriptor *descriptor) override {
                const std::lock_guard lock{mutex_};
                descriptors_.push_back(*descriptor);
            }

            void Flush() override {}

            [[nodiscard]] std::vector<Telemetry::InstrumentDescriptor> Descriptors() const {
                const std::lock_guard lock{mutex_};
                return descriptors_;
            }

        private:
            mutable std::mutex mutex_;
            std::vector<Telemetry::InstrumentDescriptor> descriptors_;
        };

        struct RuntimeGuard final {
            ~RuntimeGuard() {
                static_cast<void>(Telemetry::Runtime::Shutdown());
            }
        };
    }  // namespace

    TEST_CASE("Character capture counts actual sweep work and phase timing without tick allocation", "[physics][character][metrics]") {
        auto active = TestDetail::SpawnedActiveWorldWithController();
        auto &world = *active.world;
        SweepProbe probe;
        CharacterMetricCapture capture;
        auto input = TestDetail::FixedTick(1);
        input.query = probe.Context(world.Descriptor(), 1);
        input.metrics = &capture;
        auto movement = TestDetail::Movement(active.controller, 1, 1);
        movement.desiredVelocityMetersPerSecond = Math::Vec3{1, 0, 0};
        REQUIRE(world.QueueMovementCommand(movement).HasValue());

        const auto allocationsBefore = Tests::AllocationProbe::Count();
        const auto advanced = world.AdvanceFixedTick(input);
        const auto allocationsAfter = Tests::AllocationProbe::Count();
        REQUIRE(advanced.HasValue());
        REQUIRE(allocationsAfter == allocationsBefore);
        CHECK(capture.snapshot.world == world.Descriptor().identity);
        CHECK(capture.snapshot.tick == 1);
        CHECK(capture.snapshot.publicationRevision == world.PublishedTick().publicationRevision);
        CHECK(capture.snapshot.activeControllers == 1);
        CHECK(capture.snapshot.queries == probe.calls);
        CHECK(capture.snapshot.queries == 2);
        CHECK(capture.snapshot.movementIterations == 1);
        CHECK(capture.snapshot.contacts == 0);
        CHECK_FALSE(capture.snapshot.failed);
        for (std::size_t index{}; index < capture.snapshot.phaseCompleted.size(); ++index) {
            CHECK(capture.snapshot.phaseCompleted[index]);
            CHECK(capture.snapshot.phaseSeconds[index] >= 0.0);
        }
    }

    TEST_CASE("Character capture marks failed attempts and binding rejects stale or malformed publication",
              "[physics][character][metrics]") {
        auto active = TestDetail::SpawnedActiveWorldWithController();
        auto &world = *active.world;
        auto created = OffBinding(world);
        REQUIRE(created.HasValue());
        auto binding = std::move(created).Value();
        CharacterMetricCapture capture;
        auto first = TestDetail::FixedTick(1);
        first.metrics = &capture;
        REQUIRE(world.AdvanceFixedTick(first).HasValue());
        REQUIRE(binding.Publish(capture.snapshot, 7).HasValue());
        REQUIRE(binding.Publish(capture.snapshot, 7).HasError());

        REQUIRE(world.QueueMovementCommand(TestDetail::Movement(active.controller, 2, 1)).HasValue());
        const CharacterTickObserver observer{.movementResult = FailMovement};
        auto second = TestDetail::FixedTick(2, observer);
        second.metrics = &capture;
        REQUIRE(world.AdvanceFixedTick(second).HasError());
        CHECK(capture.snapshot.failed);
        CHECK(capture.snapshot.publicationRevision == 0);
        CHECK(capture.snapshot.phaseCompleted[0]);
        CHECK_FALSE(capture.snapshot.phaseCompleted[1]);
        REQUIRE(binding.Publish(capture.snapshot, 7).HasValue());

        auto malformed = capture.snapshot;
        malformed.tick = 3;
        malformed.queries = world.Settings().Values().work.maximumQueriesPerTick + 1;
        REQUIRE(binding.Publish(malformed, 7).HasError());
        REQUIRE(binding.Publish(malformed, 6).HasError());
        REQUIRE(binding.Close().HasValue());
        REQUIRE(binding.Publish(malformed, 7).HasError());
    }

    TEST_CASE("Character telemetry prebinds bounded subsystem series outside the tick", "[physics][character][metrics]") {
        static_cast<void>(Telemetry::Runtime::Shutdown());
        const RuntimeGuard runtimeGuard;
        auto sink = std::make_shared<MetricSink>();
        REQUIRE(Telemetry::Runtime::Initialize({.queueCapacity = 128, .metricCollectionLevel = Telemetry::MetricCollectionLevel::Detailed},
                                               sink));
        auto active = TestDetail::ActiveWorldWithControllers();
        const auto &world = *active.world;
        const auto &settings = world.Settings().Values();
        auto handles = RegisterCharacterMetricHandles(Telemetry::MetricCollectionLevel::Detailed);
        for (const auto &handle : handles.counts)
            REQUIRE(static_cast<bool>(handle));
        for (const auto &handle : handles.events)
            REQUIRE(static_cast<bool>(handle));
        for (const auto &handle : handles.phaseDurations)
            REQUIRE(static_cast<bool>(handle));
        auto created = CharacterMetricBinding::Create(world.Descriptor().identity, world.Descriptor().sceneGeneration, 9,
                                                      {.maximumControllers = settings.capacities.maximumControllers,
                                                       .maximumQueriesPerTick = settings.work.maximumQueriesPerTick,
                                                       .maximumMovementIterations = settings.work.maximumMovementIterations,
                                                       .maximumContactsPerMovement = settings.work.maximumContactsPerMovement},
                                                      Telemetry::MetricCollectionLevel::Detailed, std::move(handles));
        REQUIRE(created.HasValue());
        auto binding = std::move(created).Value();
        CharacterMetricSnapshot snapshot{.world = world.Descriptor().identity,
                                         .sceneGeneration = world.Descriptor().sceneGeneration,
                                         .tick = 1,
                                         .publicationRevision = 1,
                                         .activeControllers = 1,
                                         .queries = 2,
                                         .movementIterations = 1,
                                         .overflows = 3,
                                         .phaseSeconds = {0.001, 0.002, 0.003},
                                         .phaseCompleted = {true, true, true}};
        const auto before = Telemetry::Runtime::GetStatistics();
        REQUIRE(binding.Publish(snapshot, 9).HasValue());
        REQUIRE(Telemetry::Runtime::Flush(std::chrono::seconds{2}));
        const auto after = Telemetry::Runtime::GetStatistics();
        const auto descriptors = sink->Descriptors();
        // The real-time queue deliberately drops records on contention; prebinding
        // is independent of whether the dispatcher accepts every observation.
        CHECK(after.acceptedRecords - before.acceptedRecords + after.droppedRecords - before.droppedRecords == 8);
        REQUIRE(descriptors.size() == after.acceptedRecords - before.acceptedRecords);
        for (const auto &descriptor : descriptors)
            CHECK(descriptor.subsystem == "character");
        REQUIRE(binding.Close().HasValue());
    }
}  // namespace Horo::Character
