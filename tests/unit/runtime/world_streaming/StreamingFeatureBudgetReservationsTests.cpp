#include "Horo/WorldStreaming/StreamingFeatureBudgetReservations.h"
#include "WorldStreamingTestUtils.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <type_traits>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::RequireError;
        using namespace WorldStreamingErrors;
        using Feature = StreamingBudgetFeature;
        using Axis = StreamingBudgetDimension;

        [[nodiscard]] StreamingBudgetAmounts Amounts(const std::uint64_t cpu = 0, const std::uint64_t gpu = 0,
                                                     const std::uint64_t staging = 0, const std::uint64_t io = 0,
                                                     const std::uint64_t scratch = 0, const std::uint64_t retired = 0,
                                                     const std::uint64_t work = 0) {
            const std::array values{cpu, gpu, staging, io, scratch, retired, work};
            std::array<StreamingBudgetAmount, StreamingBudgetDimensionCount> amounts{};
            for (std::size_t i = 0; i < values.size(); ++i)
                amounts[i] = {static_cast<Axis>(i), values[i]};
            return StreamingBudgetAmounts::Create(amounts).Value();
        }

        [[nodiscard]] StreamingBudgetPolicy Aggregate(const std::uint64_t cap = 100) {
            std::array<StreamingBudgetLimit, StreamingBudgetDimensionCount> limits{};
            for (std::size_t i = 0; i < limits.size(); ++i)
                limits[i] = {static_cast<Axis>(i), cap, cap};
            return StreamingBudgetPolicy::Create(IdentityFrom<StreamingBudgetPolicyRevision>(3), limits, std::chrono::milliseconds{2})
                .Value();
        }

        [[nodiscard]] auto Slices(const std::uint64_t each = 20) {
            const auto amount = Amounts(each, each, each, each, each, each, each);
            return std::array{StreamingFeatureBudgetAmount{Feature::Terrain, amount},
                              StreamingFeatureBudgetAmount{Feature::Foliage, amount},
                              StreamingFeatureBudgetAmount{Feature::Navigation, amount},
                              StreamingFeatureBudgetAmount{Feature::Physics, amount}};
        }

        [[nodiscard]] auto PlanEntries(const Feature feature = Feature::Terrain, const StreamingBudgetAmounts &amount = Amounts()) {
            const auto zero = Amounts();
            std::array entries{StreamingFeatureBudgetAmount{Feature::Terrain, zero}, StreamingFeatureBudgetAmount{Feature::Foliage, zero},
                               StreamingFeatureBudgetAmount{Feature::Navigation, zero},
                               StreamingFeatureBudgetAmount{Feature::Physics, zero}, StreamingFeatureBudgetAmount{Feature::General, zero}};
            entries[static_cast<std::size_t>(feature)].amounts = amount;
            return entries;
        }

        [[nodiscard]] StreamingFeatureBudgetPlan Plan(const Feature feature = Feature::Terrain,
                                                      const StreamingBudgetAmounts &amount = Amounts()) {
            return StreamingFeatureBudgetPlan::Create(PlanEntries(feature, amount)).Value();
        }

        [[nodiscard]] StreamingCellOperation Operation(const std::uint64_t id = 1, const std::uint64_t generation = 1) {
            return StreamingCellOperation::Create({IdentityFrom<StreamingCellOperationId>(id),
                                                   {TestSupport::World(),
                                                    IdentityFrom<PartitionEpoch>(1),
                                                    {1, 2, 3, 0, TestSupport::Layer()},
                                                    IdentityFrom<StreamingGeneration>(generation)}},
                                                  StreamingCellOperationKind::Load)
                .Value();
        }

        [[nodiscard]] SharedAssetKey Key(const std::uint8_t id = 1) {
            return {TestSupport::Asset(id), IdentityFrom<SharedAssetRevision>(1)};
        }

        struct Host final {
            int service{};

            [[nodiscard]] auto Bindings() const {
                return std::array{StreamingRuntimeServiceBinding{IdentityFrom<StreamingRuntimeServiceId>(1),
                                                                 IdentityFrom<StreamingRuntimeServiceRevision>(1),
                                                                 StreamingRuntimeServiceRole::Planner, &service},
                                  StreamingRuntimeServiceBinding{IdentityFrom<StreamingRuntimeServiceId>(2),
                                                                 IdentityFrom<StreamingRuntimeServiceRevision>(1),
                                                                 StreamingRuntimeServiceRole::AssetProvider, &service},
                                  StreamingRuntimeServiceBinding{IdentityFrom<StreamingRuntimeServiceId>(3),
                                                                 IdentityFrom<StreamingRuntimeServiceRevision>(1),
                                                                 StreamingRuntimeServiceRole::SceneRuntime, &service},
                                  StreamingRuntimeServiceBinding{IdentityFrom<StreamingRuntimeServiceId>(4),
                                                                 IdentityFrom<StreamingRuntimeServiceRevision>(1),
                                                                 StreamingRuntimeServiceRole::FeatureAdapter, &service}};
            }

            [[nodiscard]] StreamingFeatureBudgetConfig Config(const std::uint32_t operations = 4, const std::uint32_t entries = 4,
                                                              const std::uint32_t leases = 8) const {
                return {1,
                        {TestSupport::WorldOwner(),
                         IdentityFrom<StreamingRuntimeCompositionRevision>(1),
                         IdentityFrom<StreamingSchedulerLedgerId>(9),
                         {operations, 10},
                         4},
                        entries,
                        leases};
            }

            [[nodiscard]] StreamingFeatureBudgetReservations Ledger(const std::uint32_t operations = 4, const std::uint32_t entries = 4,
                                                                    const std::uint32_t leases = 8) const {
                auto result =
                    StreamingFeatureBudgetReservations::Create(Config(operations, entries, leases), Bindings(),
                                                               StreamingFeatureBudgetPolicy::Create(Aggregate(), Slices()).Value());
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }
        };

        [[nodiscard]] Result<StreamingFeatureBudgetReservation> Admit(StreamingFeatureBudgetReservations &ledger,
                                                                      const StreamingFeatureBudgetPlan &plan = Plan(),
                                                                      const std::uint64_t id = 1, const std::uint64_t generation = 1,
                                                                      const std::uint64_t units = 1) {
            const auto operation = Operation(id, generation);
            return ledger.TryAdmit(ledger.Context(), operation, plan, units, operation.Handle().fence);
        }

        void Complete(StreamingFeatureBudgetReservations &ledger, const StreamingFeatureBudgetReservation &reservation) {
            REQUIRE(ledger.Advance(ledger.Context(), reservation, StreamingCellOperationTransition::BeginPreparation).HasValue());
            REQUIRE(ledger.Advance(ledger.Context(), reservation, StreamingCellOperationTransition::Complete).HasValue());
            REQUIRE(ledger.Release(ledger.Context(), reservation).HasValue());
        }

        [[nodiscard]] Result<SharedAssetLease> Realize(StreamingFeatureBudgetReservations &ledger,
                                                       const StreamingFeatureBudgetReservation &reservation,
                                                       const StreamingBudgetAmounts &cost = Amounts(12),
                                                       const StreamingBudgetAmounts &portion = Amounts(12),
                                                       const SharedAssetKey &key = Key(), const Feature feature = Feature::Terrain) {
            return ledger.RealizeShared(ledger.Context(), reservation,
                                        {feature, key, cost, portion, {reservation.scheduler.operation.fence, std::nullopt}});
        }

        void AcknowledgeOperationRetirement(StreamingFeatureBudgetReservations &ledger,
                                            const StreamingFeatureBudgetReservation &reservation) {
            REQUIRE(ledger.Advance(ledger.Context(), reservation, StreamingCellOperationTransition::AcknowledgeRetirement).HasValue());
            REQUIRE(ledger.Release(ledger.Context(), reservation).HasValue());
        }

        void RetireCache(StreamingFeatureBudgetReservations &ledger, const SharedAssetLease &lease) {
            REQUIRE(ledger.ReleaseShared(ledger.Context(), lease).HasValue());
            const auto retired = ledger.BeginRetireShared(ledger.Context(), lease.key, lease.charge).Value();
            REQUIRE(ledger.AcknowledgeSharedRetired(ledger.Context(), retired).HasValue());
        }

        struct ResidentFixture final {
            Host host;
            StreamingFeatureBudgetReservations ledger{host.Ledger()};
            StreamingFeatureBudgetReservation reservation;
            SharedAssetLease lease;

            explicit ResidentFixture(const std::uint64_t peak = 12)
                : reservation(Admit(ledger, Plan(Feature::Terrain, Amounts(peak))).Value()), lease(Realize(ledger, reservation).Value()) {}
        };

        static_assert(!std::is_copy_constructible_v<StreamingFeatureBudgetReservations>);
        static_assert(std::is_same_v<decltype(std::declval<StreamingFeatureBudgetReservations &>().Runtime()),
                                     const WorldStreamingRuntimeComposition &>);
        static_assert(std::is_same_v<decltype(std::declval<StreamingFeatureBudgetReservations &>().SharedAssets()),
                                     const SharedAssetResidencyLedger &>);

        TEST_CASE("Feature slices partition global capacity and general remainder without overflow",
                  "[unit][world_streaming][feature_budget]") {
            const auto policy = StreamingFeatureBudgetPolicy::Create(Aggregate(), Slices()).Value();
            REQUIRE(policy.Revision() == IdentityFrom<StreamingBudgetPolicyRevision>(3));
            for (const auto &amount : policy.GlobalCapacity().Entries()) {
                REQUIRE(amount.value == 100);
                for (std::size_t feature = 0; feature < StreamingBudgetFeatureCount; ++feature)
                    REQUIRE(policy.Capacity(static_cast<Feature>(feature)).Value().Value(amount.dimension).Value() == 20);
            }
            RequireError(policy.Capacity(Feature::Count), FeatureBudgetUnsupported);
            auto slices = Slices(25);
            const auto exact = StreamingFeatureBudgetPolicy::Create(Aggregate(), slices).Value();
            REQUIRE(exact.Capacity(Feature::General).Value().IsZero());
            slices[3].amounts = Amounts(26);
            RequireError(StreamingFeatureBudgetPolicy::Create(Aggregate(), slices), FeatureBudgetCapacityExceeded);
            slices = Slices(0);
            slices[0].amounts = Amounts(std::numeric_limits<std::uint64_t>::max());
            slices[1].amounts = Amounts(1);
            RequireError(StreamingFeatureBudgetPolicy::Create(Aggregate(std::numeric_limits<std::uint64_t>::max()), slices),
                         FeatureBudgetCapacityExceeded);
            slices[1].amounts = Amounts();
            REQUIRE(StreamingFeatureBudgetPolicy::Create(Aggregate(std::numeric_limits<std::uint64_t>::max()), slices).HasValue());
        }

        TEST_CASE("Feature policy and peak plans require complete unique supported feature vectors",
                  "[unit][world_streaming][feature_budget]") {
            auto slices = Slices();
            RequireError(StreamingFeatureBudgetPolicy::Create(Aggregate(), std::span{slices}.first(3)), FeatureBudgetInvalid);
            slices[3].feature = Feature::Terrain;
            RequireError(StreamingFeatureBudgetPolicy::Create(Aggregate(), slices), FeatureBudgetInvalid);
            slices[3].feature = Feature::General;
            RequireError(StreamingFeatureBudgetPolicy::Create(Aggregate(), slices), FeatureBudgetUnsupported);
            auto entries = PlanEntries(Feature::Physics, Amounts(1, 2, 3, 4, 5, 6, 7));
            std::ranges::reverse(entries);
            const auto plan = StreamingFeatureBudgetPlan::Create(entries).Value();
            REQUIRE(plan.Amounts(Feature::Physics).Value() == Amounts(1, 2, 3, 4, 5, 6, 7));
            RequireError(plan.Amounts(Feature::Count), FeatureBudgetUnsupported);
            RequireError(StreamingFeatureBudgetPlan::Create(std::span{entries}.first(4)), FeatureBudgetInvalid);
            entries[0].feature = Feature::Terrain;
            RequireError(StreamingFeatureBudgetPlan::Create(entries), FeatureBudgetInvalid);
            entries[0].feature = static_cast<Feature>(99);
            RequireError(StreamingFeatureBudgetPlan::Create(entries), FeatureBudgetUnsupported);
        }

        TEST_CASE("Feature reservation factory validates version host bindings and bounded metadata",
                  "[unit][world_streaming][feature_budget]") {
            Host host;
            const auto policy = StreamingFeatureBudgetPolicy::Create(Aggregate(), Slices()).Value();
            auto config = host.Config();
            config.contractVersion = 2;
            RequireError(StreamingFeatureBudgetReservations::Create(config, host.Bindings(), policy), FeatureBudgetUnsupported);
            config = host.Config();
            config.runtime.owner = {};
            RequireError(StreamingFeatureBudgetReservations::Create(config, host.Bindings(), policy), RuntimeCompositionInvalid);
            config = host.Config();
            config.sharedEntries = SharedAssetResidencyLimits::MaximumEntries + 1;
            RequireError(StreamingFeatureBudgetReservations::Create(config, host.Bindings(), policy), SharedAssetInvalid);
            config = host.Config();
            config.sharedLeases = 0;
            RequireError(StreamingFeatureBudgetReservations::Create(config, host.Bindings(), policy), SharedAssetInvalid);
            RequireError(StreamingFeatureBudgetReservations::Create(host.Config(), std::span<const StreamingRuntimeServiceBinding>{},
                                                                    policy),
                         RuntimeCompositionInvalid);
        }

        TEST_CASE("Atomic admission couples all resource axes and the actual canonical scheduler",
                  "[unit][world_streaming][feature_budget]") {
            Host host;
            auto ledger = host.Ledger();
            auto entries = PlanEntries();
            for (auto &entry : entries)
                entry.amounts = Amounts(20, 20, 20, 20, 20, 20, 20);
            const auto reservation = Admit(ledger, StreamingFeatureBudgetPlan::Create(entries).Value()).Value();
            REQUIRE(ledger.Runtime().Scheduler().Inspect(reservation.scheduler).Value().State() == StreamingCellOperationState::Admitted);
            REQUIRE(ledger.Runtime().Scheduler().ReservedCount() == 1);
            for (std::size_t axis = 0; axis < StreamingBudgetDimensionCount; ++axis)
                REQUIRE(ledger.Used(static_cast<Axis>(axis)).Value() == 100);
            const auto before = ledger.Context();
            RequireError(Admit(ledger, Plan(Feature::Physics, Amounts(1)), 2), FeatureBudgetCapacityExceeded);
            REQUIRE(ledger.Context().revision == before.revision);
            REQUIRE(ledger.Runtime().Scheduler().ReservedCount() == 1);
            Complete(ledger, reservation);
            for (std::size_t axis = 0; axis < StreamingBudgetDimensionCount; ++axis)
                REQUIRE(ledger.Used(static_cast<Axis>(axis)).Value() == 0);
        }

        TEST_CASE("Feature and scheduler denials leave both authorities unchanged", "[unit][world_streaming][feature_budget]") {
            Host host;
            auto ledger = host.Ledger(1);
            RequireError(Admit(ledger, Plan(Feature::Terrain, Amounts(21))), FeatureBudgetCapacityExceeded);
            const auto reservation = Admit(ledger, Plan(Feature::Terrain, Amounts(10)), 1, 1, 10).Value();
            const auto before = ledger.Context();
            RequireError(Admit(ledger, Plan(Feature::Physics, Amounts(1)), 2), SchedulerCapacityExceeded);
            REQUIRE(ledger.Context().revision == before.revision);
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 10);
            Complete(ledger, reservation);
        }

        TEST_CASE("Owner policy revision and current generation fences reject stale admission atomically",
                  "[unit][world_streaming][feature_budget]") {
            Host host;
            auto ledger = host.Ledger();
            const auto operation = Operation();
            auto context = ledger.Context();
            context.owner.owner = IdentityFrom<StreamingRuntimeOwnerId>(999);
            RequireError(ledger.TryAdmit(context, operation, Plan(), 1, operation.Handle().fence), FeatureBudgetStale);
            context = ledger.Context();
            context.policyRevision = IdentityFrom<StreamingBudgetPolicyRevision>(4);
            RequireError(ledger.TryAdmit(context, operation, Plan(), 1, operation.Handle().fence), FeatureBudgetStale);
            context = ledger.Context();
            context.revision = {};
            RequireError(ledger.TryAdmit(context, operation, Plan(), 1, operation.Handle().fence), FeatureBudgetInvalid);
            RequireError(ledger.TryAdmit(ledger.Context(), operation, Plan(), 1, Operation(2, 2).Handle().fence), FeatureBudgetStale);
            RequireError(ledger.TryAdmit(ledger.Context(), operation, Plan(), 1, {}), FeatureBudgetInvalid);
            const auto old = ledger.Context();
            const auto reservation = Admit(ledger).Value();
            RequireError(ledger.Grow(old, reservation, Plan()), FeatureBudgetStale);
            REQUIRE(ledger.Runtime().Scheduler().ReservedCount() == 1);
            auto forged = reservation;
            ++forged.scheduler.capacityUnits;
            RequireError(ledger.Grow(ledger.Context(), forged, Plan()), FeatureBudgetStale);
            RequireError(ledger.Release(ledger.Context(), {}), FeatureBudgetInvalid);
            Complete(ledger, reservation);
        }

        TEST_CASE("Peak growth is checked before allocation and late-axis rejection is atomic", "[unit][world_streaming][feature_budget]") {
            Host host;
            auto ledger = host.Ledger();
            const auto reservation = Admit(ledger, Plan(Feature::Terrain, Amounts(10))).Value();
            REQUIRE(ledger.Grow(ledger.Context(), reservation, Plan(Feature::Terrain, Amounts(10))).HasValue());
            const auto before = ledger.Context();
            RequireError(ledger.Grow(ledger.Context(), reservation, Plan(Feature::Physics, Amounts(5, 0, 0, 0, 0, 0, 21))),
                         FeatureBudgetCapacityExceeded);
            REQUIRE(ledger.Used(Feature::Physics, Axis::CpuResidentBytes).Value() == 0);
            REQUIRE(ledger.Context().revision == before.revision);
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 20);
            RequireError(ledger.Used(Feature::Count, Axis::CpuResidentBytes), FeatureBudgetUnsupported);
            RequireError(ledger.Used(Feature::Terrain, Axis::Count), BudgetDimensionUnsupported);
            RequireError(ledger.Used(Axis::Count), BudgetDimensionUnsupported);
            Complete(ledger, reservation);
        }

        TEST_CASE("Cache realization transfers a single charge and consumer release returns no premature credit",
                  "[unit][world_streaming][feature_budget]") {
            Host host;
            auto ledger = host.Ledger();
            const auto first = Admit(ledger, Plan(Feature::Terrain, Amounts(20))).Value();
            const auto lease = Realize(ledger, first).Value();
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 20);
            REQUIRE(ledger.SharedAssets().Charged(Axis::CpuResidentBytes).Value() == 12);
            const auto second = Admit(ledger, Plan(), 2, 2).Value();
            const auto reused = Realize(ledger, second, Amounts(12), Amounts(), Key(), Feature::Foliage).Value();
            REQUIRE(reused.charge == lease.charge);
            REQUIRE(ledger.SharedAssets().ChargedCount() == 1);
            REQUIRE(ledger.Used(Feature::Foliage, Axis::CpuResidentBytes).Value() == 0);
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 20);
            Complete(ledger, first);
            Complete(ledger, second);
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 12);
            REQUIRE(ledger.ReleaseShared(ledger.Context(), lease).HasValue());
            RequireError(ledger.BeginRetireShared(ledger.Context(), lease.key, lease.charge), SharedAssetLifecycleUnavailable);
            REQUIRE(ledger.ReleaseShared(ledger.Context(), reused).HasValue());
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 12);
            const auto retirement = ledger.BeginRetireShared(ledger.Context(), lease.key, lease.charge).Value();
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 12);
            REQUIRE(ledger.AcknowledgeSharedRetired(ledger.Context(), retirement).HasValue());
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 0);
        }

        TEST_CASE("Cache reuse resolves explicitly reserved duplicate peaks without additive physical accounting",
                  "[unit][world_streaming][feature_budget]") {
            Host host;
            auto ledger = host.Ledger();
            const auto first = Admit(ledger, Plan(Feature::Terrain, Amounts(12))).Value();
            const auto lease = Realize(ledger, first).Value();
            const auto second = Admit(ledger, Plan(Feature::Foliage, Amounts(12)), 2, 2).Value();
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 24);
            const auto unchanged = ledger.Context();
            RequireError(Realize(ledger, second, Amounts(13), Amounts(), Key(), Feature::Foliage), SharedAssetConflict);
            REQUIRE(ledger.Context().revision == unchanged.revision);
            const SharedAssetRetirement premature{ledger.Context().owner.owner, lease.charge, lease.key};
            RequireError(ledger.AcknowledgeSharedRetired(ledger.Context(), premature), SharedAssetLifecycleUnavailable);
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 24);
            const auto reused = Realize(ledger, second, Amounts(12), Amounts(12), Key(), Feature::Foliage).Value();
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 12);
            REQUIRE(ledger.Used(Feature::Foliage, Axis::CpuResidentBytes).Value() == 0);
            RequireError(Realize(ledger, second, Amounts(12), Amounts(), Key(), Feature::Foliage), SharedAssetConflict);
            Complete(ledger, first);
            Complete(ledger, second);
            REQUIRE(ledger.ReleaseShared(ledger.Context(), reused).HasValue());
            RetireCache(ledger, lease);
        }

        TEST_CASE("Materialization validates costs exact consumers and child lease capacity before mutation",
                  "[unit][world_streaming][feature_budget]") {
            Host host;
            auto ledger = host.Ledger(4, 1, 1);
            const auto reservation = Admit(ledger, Plan(Feature::Terrain, Amounts(20))).Value();
            const auto before = ledger.Context();
            RequireError(Realize(ledger, reservation, Amounts(12), Amounts(11)), FeatureBudgetInvalid);
            RequireError(Realize(ledger, reservation, Amounts(21), Amounts(21)), FeatureBudgetCapacityExceeded);
            RequireError(Realize(ledger, reservation, Amounts(12), Amounts(12), {}, Feature::Terrain), SharedAssetInvalid);
            RequireError(Realize(ledger, reservation, Amounts(12), Amounts(12), Key(), Feature::Count), FeatureBudgetUnsupported);
            RequireError(ledger.RealizeShared(ledger.Context(), reservation,
                                              {Feature::Terrain,
                                               Key(),
                                               Amounts(12),
                                               Amounts(12),
                                               {Operation(2, 2).Handle().fence, std::nullopt}}),
                         FeatureBudgetStale);
            REQUIRE(ledger.Context().revision == before.revision);
            const auto lease = Realize(ledger, reservation).Value();
            const auto second = Admit(ledger, Plan(Feature::Physics, Amounts(5)), 2, 2).Value();
            const auto retained = ledger.Context();
            RequireError(Realize(ledger, second, Amounts(12), Amounts(), Key(), Feature::Physics), SharedAssetCapacityExceeded);
            REQUIRE(ledger.Context().revision == retained.revision);
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 25);
            Complete(ledger, reservation);
            Complete(ledger, second);
            RetireCache(ledger, lease);
        }

        TEST_CASE("Cancellation failure and replacement retain peak and cache charges until exact retirement",
                  "[unit][world_streaming][feature_budget]") {
            for (const auto reason : {StreamingCellOperationTransition::Cancel, StreamingCellOperationTransition::Fail,
                                      StreamingCellOperationTransition::Replace}) {
                Host host;
                auto ledger = host.Ledger();
                const auto reservation = Admit(ledger, Plan(Feature::Terrain, Amounts(20))).Value();
                const auto lease = Realize(ledger, reservation).Value();
                REQUIRE(ledger.Advance(ledger.Context(), reservation, reason).Value().State() == StreamingCellOperationState::Retiring);
                RequireError(ledger.Release(ledger.Context(), reservation), SchedulerLifecycleUnavailable);
                RequireError(ledger.Grow(ledger.Context(), reservation, Plan()), FeatureBudgetLifecycleUnavailable);
                RequireError(Realize(ledger, reservation), FeatureBudgetLifecycleUnavailable);
                REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 20);
                const auto successor = Admit(ledger, Plan(Feature::Physics, Amounts(10)), 2, 2).Value();
                AcknowledgeOperationRetirement(ledger, reservation);
                REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 22);
                Complete(ledger, successor);
                RetireCache(ledger, lease);
                REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 0);
            }
        }

        TEST_CASE("Late cache retirement cannot free a successor charge or a new owner", "[unit][world_streaming][feature_budget]") {
            ResidentFixture fixture{12};
            auto &ledger = fixture.ledger;
            const auto &first = fixture.reservation;
            const auto &lease = fixture.lease;
            Complete(ledger, first);
            REQUIRE(ledger.ReleaseShared(ledger.Context(), lease).HasValue());
            const auto old = ledger.BeginRetireShared(ledger.Context(), lease.key, lease.charge).Value();
            REQUIRE(ledger.AcknowledgeSharedRetired(ledger.Context(), old).HasValue());
            const auto second = Admit(ledger, Plan(Feature::Terrain, Amounts(12)), 2, 2).Value();
            const auto newer = Realize(ledger, second).Value();
            REQUIRE(newer.charge != old.charge);
            const auto before = ledger.Context();
            RequireError(ledger.AcknowledgeSharedRetired(ledger.Context(), old), FeatureBudgetStale);
            REQUIRE(ledger.Context().revision == before.revision);
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 12);
            auto foreign = newer;
            foreign.owner = IdentityFrom<StreamingRuntimeOwnerId>(99);
            RequireError(ledger.ReleaseShared(ledger.Context(), foreign), SharedAssetStale);
            Complete(ledger, second);
            RetireCache(ledger, newer);
        }

        TEST_CASE("Borrowed provider replacement waits for both canonical operations and cache ownership",
                  "[unit][world_streaming][feature_budget]") {
            ResidentFixture fixture{12};
            auto &ledger = fixture.ledger;
            const auto &reservation = fixture.reservation;
            const auto &lease = fixture.lease;
            RequireError(ledger.ReplaceServices(ledger.Context(), IdentityFrom<StreamingRuntimeCompositionRevision>(2),
                                                fixture.host.Bindings()),
                         FeatureBudgetLifecycleUnavailable);
            Complete(ledger, reservation);
            RequireError(ledger.ReplaceServices(ledger.Context(), IdentityFrom<StreamingRuntimeCompositionRevision>(2),
                                                fixture.host.Bindings()),
                         FeatureBudgetLifecycleUnavailable);
            RetireCache(ledger, lease);
            const auto before = ledger.Context();
            RequireError(ledger.ReplaceServices(ledger.Context(), IdentityFrom<StreamingRuntimeCompositionRevision>(1),
                                                fixture.host.Bindings()),
                         RuntimeCompositionRevisionStale);
            REQUIRE(ledger.Context().revision == before.revision);
            REQUIRE(ledger.ReplaceServices(ledger.Context(), IdentityFrom<StreamingRuntimeCompositionRevision>(2), fixture.host.Bindings())
                        .HasValue());
            REQUIRE(ledger.Runtime().Revision() == IdentityFrom<StreamingRuntimeCompositionRevision>(2));
        }

        TEST_CASE("Shutdown closes admission but drains old exact operations and cache charges",
                  "[unit][world_streaming][feature_budget]") {
            ResidentFixture fixture{20};
            auto &ledger = fixture.ledger;
            const auto &reservation = fixture.reservation;
            const auto &lease = fixture.lease;
            REQUIRE(ledger.BeginShutdown(ledger.Context()).HasValue());
            REQUIRE(ledger.BeginShutdown(ledger.Context()).HasValue());
            REQUIRE(ledger.State() == StreamingSchedulerAdmissionState::Draining);
            RequireError(Admit(ledger, Plan(), 2), SchedulerLifecycleUnavailable);
            REQUIRE(ledger.Advance(ledger.Context(), reservation, StreamingCellOperationTransition::Shutdown).HasValue());
            AcknowledgeOperationRetirement(ledger, reservation);
            REQUIRE(ledger.Runtime().State() == WorldStreamingRuntimeCompositionState::Closed);
            REQUIRE(ledger.State() == StreamingSchedulerAdmissionState::Draining);
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 12);
            RetireCache(ledger, lease);
            REQUIRE(ledger.State() == StreamingSchedulerAdmissionState::Closed);
            REQUIRE(ledger.Used(Axis::CpuResidentBytes).Value() == 0);
            REQUIRE(ledger.BeginShutdown(ledger.Context()).HasValue());
        }
    }  // namespace
}  // namespace Horo::WorldStreaming
