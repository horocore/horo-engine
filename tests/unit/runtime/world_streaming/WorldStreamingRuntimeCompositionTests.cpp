#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "Horo/WorldStreaming/WorldStreamingRuntimeComposition.h"
#include "WorldStreamingTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::Layer;
        using TestSupport::RequireError;
        using TestSupport::World;

        int planner;
        int assets;
        int scene;
        int physics;
        int navigation;

        StreamingRuntimeOwnerToken RuntimeOwner(const std::uint64_t identity = 5) {
            return {.partition = World(),
                    .epoch = IdentityFrom<PartitionEpoch>(1),
                    .owner = IdentityFrom<StreamingRuntimeOwnerId>(identity)};
        }

        StreamingRuntimeServiceBinding Binding(const std::uint64_t id, const StreamingRuntimeServiceRole role, const void *instance,
                                               const std::uint64_t revision = 1) {
            return {.id = IdentityFrom<StreamingRuntimeServiceId>(id),
                    .revision = IdentityFrom<StreamingRuntimeServiceRevision>(revision),
                    .role = role,
                    .instance = instance};
        }

        std::array<StreamingRuntimeServiceBinding, 4> Services() {
            using enum StreamingRuntimeServiceRole;
            return {Binding(40, FeatureAdapter, &physics), Binding(10, Planner, &planner), Binding(30, SceneRuntime, &scene),
                    Binding(20, AssetProvider, &assets)};
        }

        WorldStreamingRuntimeCompositionConfig Config(const std::uint64_t revision = 1, const std::uint32_t maximumFeatureAdapters = 2) {
            return {.owner = RuntimeOwner(),
                    .revision = IdentityFrom<StreamingRuntimeCompositionRevision>(revision),
                    .schedulerOwner = IdentityFrom<StreamingSchedulerLedgerId>(7),
                    .schedulerLimits = {.concurrentOperations = 2,
                                        .capacityUnits = 8,
                                        .concurrency = {.profile = WorldPartitionProjectProfile::Editor,
                                                        .revision = IdentityFrom<StreamingConcurrencyRevision>(1),
                                                        .loads = 2,
                                                        .activations = 2,
                                                        .retirements = 2}},
                    .maximumFeatureAdapters = maximumFeatureAdapters};
        }

        WorldStreamingRuntimeComposition Composition() {
            const auto services = Services();
            auto result = WorldStreamingRuntimeComposition::Create(Config(), services);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        StreamingCellOperation QueuedOperation() {
            return StreamingCellOperation::Create({.operation = IdentityFrom<StreamingCellOperationId>(9),
                                                   .fence = {.partition = World(),
                                                             .epoch = IdentityFrom<PartitionEpoch>(1),
                                                             .cell = {1, 2, 3, 0, Layer()},
                                                             .generation = IdentityFrom<StreamingGeneration>(1)}},
                                                  StreamingCellOperationKind::Activate)
                .Value();
        }

        static_assert(!std::is_copy_constructible_v<WorldStreamingRuntimeComposition>);
        static_assert(!std::is_copy_assignable_v<WorldStreamingRuntimeComposition>);

        TEST_CASE("Runtime composition owns one scheduler and explicit canonical service bindings",
                  "[unit][world_streaming][composition]") {
            auto composition = Composition();
            REQUIRE(composition.Owner() == RuntimeOwner());
            REQUIRE(composition.Revision() == IdentityFrom<StreamingRuntimeCompositionRevision>(1));
            REQUIRE(composition.State() == WorldStreamingRuntimeCompositionState::Active);
            REQUIRE(composition.Scheduler().Owner() == IdentityFrom<StreamingSchedulerLedgerId>(7));
            REQUIRE(composition.Services().size() == 4);
            REQUIRE(composition.Services()[0].role == StreamingRuntimeServiceRole::Planner);
            REQUIRE(composition.Services()[3].role == StreamingRuntimeServiceRole::FeatureAdapter);
            REQUIRE(composition.Resolve(IdentityFrom<StreamingRuntimeServiceId>(30)).Value().instance == &scene);
            RequireError(composition.Resolve(IdentityFrom<StreamingRuntimeServiceId>(99)),
                         WorldStreamingErrors::RuntimeCompositionRevisionStale);

            auto destination = std::move(composition);
            REQUIRE(destination.State() == WorldStreamingRuntimeCompositionState::Active);
            REQUIRE(composition.State() == WorldStreamingRuntimeCompositionState::Closed);
            RequireError(composition.Scheduler().TryAdmit(QueuedOperation(), 1, IdentityFrom<StreamingConcurrencyRevision>(1)),
                         WorldStreamingErrors::SchedulerLifecycleUnavailable);
        }

        TEST_CASE("Runtime composition rejects missing duplicate unsupported and over-capacity bindings transactionally",
                  "[unit][world_streaming][composition]") {
            auto services = Services();
            auto invalidConfig = Config();
            invalidConfig.owner = {};
            RequireError(WorldStreamingRuntimeComposition::Create(invalidConfig, services),
                         WorldStreamingErrors::RuntimeCompositionInvalid);
            invalidConfig = Config();
            invalidConfig.maximumFeatureAdapters = 0;
            RequireError(WorldStreamingRuntimeComposition::Create(invalidConfig, services),
                         WorldStreamingErrors::RuntimeCompositionInvalid);
            invalidConfig = Config();
            invalidConfig.schedulerLimits = {};
            RequireError(WorldStreamingRuntimeComposition::Create(invalidConfig, services),
                         WorldStreamingErrors::RuntimeCompositionInvalid);

            RequireError(WorldStreamingRuntimeComposition::Create(Config(), std::span{services}.first(3)),
                         WorldStreamingErrors::RuntimeCompositionInvalid);

            services[3].id = services[0].id;
            RequireError(WorldStreamingRuntimeComposition::Create(Config(), services),
                         WorldStreamingErrors::RuntimeCompositionIdentityConflict);
            services = Services();
            services[3].role = static_cast<StreamingRuntimeServiceRole>(255);
            RequireError(WorldStreamingRuntimeComposition::Create(Config(), services), WorldStreamingErrors::RuntimeCompositionInvalid);

            const auto base = Services();
            const std::array overCapacity{base[0], base[1], base[2], base[3],
                                          Binding(41, StreamingRuntimeServiceRole::FeatureAdapter, &navigation)};
            RequireError(WorldStreamingRuntimeComposition::Create(Config(1, 1), overCapacity),
                         WorldStreamingErrors::RuntimeCompositionCapacityExceeded);
        }

        TEST_CASE("Runtime composition replacement requires a fresh revision and no retained scheduler work",
                  "[unit][world_streaming][composition][lifecycle]") {
            auto composition = Composition();
            auto replacement = Services();
            replacement[0] = Binding(41, StreamingRuntimeServiceRole::FeatureAdapter, &navigation, 2);
            const auto revision = IdentityFrom<StreamingRuntimeCompositionRevision>(2);

            const auto reservation =
                composition.Scheduler().TryAdmit(QueuedOperation(), 3, IdentityFrom<StreamingConcurrencyRevision>(1)).Value();
            RequireError(composition.Replace(RuntimeOwner(), revision, replacement),
                         WorldStreamingErrors::RuntimeCompositionLifecycleUnavailable);
            REQUIRE(composition.Revision() == IdentityFrom<StreamingRuntimeCompositionRevision>(1));

            REQUIRE(composition.Scheduler().Advance(reservation, StreamingCellOperationTransition::Cancel).HasValue());
            REQUIRE(composition.Scheduler().Advance(reservation, StreamingCellOperationTransition::AcknowledgeRetirement).HasValue());
            REQUIRE(composition.Scheduler().Release(reservation).HasValue());
            REQUIRE(composition.Replace(RuntimeOwner(), revision, replacement).HasValue());
            REQUIRE(composition.Revision() == revision);
            REQUIRE(composition.Resolve(IdentityFrom<StreamingRuntimeServiceId>(41)).Value().instance == &navigation);
            RequireError(composition.Replace(RuntimeOwner(), revision, replacement), WorldStreamingErrors::RuntimeCompositionRevisionStale);

            replacement[0] = {};
            RequireError(composition.Replace(RuntimeOwner(), IdentityFrom<StreamingRuntimeCompositionRevision>(3), replacement),
                         WorldStreamingErrors::RuntimeCompositionInvalid);
            REQUIRE(composition.Revision() == revision);
        }

        TEST_CASE("Runtime composition cancellation closes admission and shutdown retains services through drain",
                  "[unit][world_streaming][composition][shutdown]") {
            auto composition = Composition();
            const auto reservation =
                composition.Scheduler().TryAdmit(QueuedOperation(), 3, IdentityFrom<StreamingConcurrencyRevision>(1)).Value();
            REQUIRE(composition.RequestCancellation(RuntimeOwner(), composition.Revision()).HasValue());
            REQUIRE(composition.RequestCancellation(RuntimeOwner(), composition.Revision()).HasValue());
            REQUIRE(composition.State() == WorldStreamingRuntimeCompositionState::Cancelling);
            RequireError(composition.Scheduler().TryAdmit(QueuedOperation(), 1, IdentityFrom<StreamingConcurrencyRevision>(1)),
                         WorldStreamingErrors::SchedulerLifecycleUnavailable);

            REQUIRE(composition.BeginShutdown(RuntimeOwner()).HasValue());
            REQUIRE(composition.State() == WorldStreamingRuntimeCompositionState::Draining);
            REQUIRE(composition.Services().size() == 4);
            REQUIRE(composition.Scheduler().Advance(reservation, StreamingCellOperationTransition::Shutdown).HasValue());
            REQUIRE(composition.Scheduler().Advance(reservation, StreamingCellOperationTransition::AcknowledgeRetirement).HasValue());
            REQUIRE(composition.Scheduler().Release(reservation).HasValue());
            REQUIRE(composition.State() == WorldStreamingRuntimeCompositionState::Closed);
            REQUIRE(composition.BeginShutdown(RuntimeOwner()).HasValue());

            auto foreign = RuntimeOwner(6);
            RequireError(composition.BeginShutdown(foreign), WorldStreamingErrors::RuntimeCompositionRevisionStale);
        }
    }  // namespace
}  // namespace Horo::WorldStreaming
