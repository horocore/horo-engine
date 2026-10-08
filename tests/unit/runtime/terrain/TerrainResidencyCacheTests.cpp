#include "../world_streaming/WorldStreamingTestUtils.h"
#include "Horo/TerrainStreaming/TerrainResidencyCache.h"

#include <chrono>
#include <limits>

namespace Horo::Terrain {
    namespace {
        namespace WST = WorldStreaming;
        using WST::TestSupport::IdentityFrom;
        using WST::TestSupport::RequireError;
        using Feature = WST::StreamingBudgetFeature;
        using Axis = WST::StreamingBudgetDimension;

        WST::StreamingBudgetAmounts Bytes(std::uint64_t cpu = 0, std::uint64_t gpu = 0) {
            std::array<WST::StreamingBudgetAmount, WST::StreamingBudgetDimensionCount> values{};
            for (std::size_t i = 0; i < values.size(); ++i)
                values[i] = {static_cast<Axis>(i), i == 0 ? cpu : i == 1 ? gpu : 0};
            return WST::StreamingBudgetAmounts::Create(values).Value();
        }

        WST::StreamingFeatureBudgetPlan Plan(std::uint64_t terrain = 0, std::uint64_t foliage = 0) {
            std::array<WST::StreamingFeatureBudgetAmount, WST::StreamingBudgetFeatureCount> values{{{Feature::Terrain, Bytes(terrain)},
                                                                                                    {Feature::Foliage, Bytes(foliage)},
                                                                                                    {Feature::Navigation, Bytes()},
                                                                                                    {Feature::Physics, Bytes()},
                                                                                                    {Feature::General, Bytes()}}};
            return WST::StreamingFeatureBudgetPlan::Create(values).Value();
        }

        struct Fixture {
            int service{};
            WST::StreamingFeatureBudgetReservations authority;
            std::unique_ptr<TerrainResidencyCache> cache;
            std::vector<TerrainResidencyLease> readers;
            std::uint32_t evictionLimit;

            WST::StreamingFeatureBudgetReservations Authority() {
                std::array<WST::StreamingBudgetLimit, WST::StreamingBudgetDimensionCount> axes{};
                for (std::size_t i = 0; i < axes.size(); ++i)
                    axes[i] = {static_cast<Axis>(i), 100, 100};
                const auto aggregate = WST::StreamingBudgetPolicy::Create(IdentityFrom<WST::StreamingBudgetPolicyRevision>(1), axes,
                                                                          std::chrono::milliseconds{1})
                                           .Value();
                const std::array slices{WST::StreamingFeatureBudgetAmount{Feature::Terrain, Bytes(50)},
                                        WST::StreamingFeatureBudgetAmount{Feature::Foliage, Bytes(40)},
                                        WST::StreamingFeatureBudgetAmount{Feature::Navigation, Bytes()},
                                        WST::StreamingFeatureBudgetAmount{Feature::Physics, Bytes()}};
                std::array<WST::StreamingRuntimeServiceBinding, 4> bindings{};
                for (std::size_t i = 0; i < bindings.size(); ++i)
                    bindings[i] = {IdentityFrom<WST::StreamingRuntimeServiceId>(i + 1),
                                   IdentityFrom<WST::StreamingRuntimeServiceRevision>(1), static_cast<WST::StreamingRuntimeServiceRole>(i),
                                   &service};
                WST::StreamingFeatureBudgetConfig config{1,
                                                         {WST::TestSupport::WorldOwner(),
                                                          IdentityFrom<WST::StreamingRuntimeCompositionRevision>(1),
                                                          IdentityFrom<WST::StreamingSchedulerLedgerId>(1),
                                                          {8,
                                                           10,
                                                           {.profile = WST::WorldPartitionProjectProfile::Standalone,
                                                            .revision = IdentityFrom<WST::StreamingConcurrencyRevision>(1),
                                                            .loads = 8,
                                                            .activations = 8,
                                                            .retirements = 8}},
                                                          4},
                                                         16,
                                                         32};
                return WST::StreamingFeatureBudgetReservations::Create(config, bindings,
                                                                       WST::StreamingFeatureBudgetPolicy::Create(aggregate, slices).Value())
                    .Value();
            }

            explicit Fixture(TerrainResidencyLimits limits = {8, 16, 50})
                : authority(Authority()),
                  cache(TerrainResidencyCache::Create(IdentityFrom<TerrainResidencyOwnerId>(1), authority, limits).Value()),
                  evictionLimit(limits.entries) {}

            ~Fixture() {
                for (const auto &lease : readers)
                    if (cache->Read(lease).HasValue())
                        REQUIRE(cache->Release(lease).HasValue());
                cache->BeginShutdown();
                REQUIRE(cache->EvictTo(0, evictionLimit).HasValue());
                REQUIRE(cache->IsDrained());
            }

            WST::StreamingFeatureBudgetReservation Admit(std::uint64_t id = 1, std::uint64_t terrain = 40, std::uint64_t foliage = 20) {
                const auto operation =
                    WST::StreamingCellOperation::Create({IdentityFrom<WST::StreamingCellOperationId>(id),
                                                         {WST::TestSupport::World(),
                                                          IdentityFrom<WST::PartitionEpoch>(1),
                                                          {static_cast<std::int32_t>(id), 0, 0, 0, WST::TestSupport::Layer()},
                                                          IdentityFrom<WST::StreamingGeneration>(1)}},
                                                        WST::StreamingCellOperationKind::Load)
                        .Value();
                return authority.TryAdmit(authority.Context(), operation, Plan(terrain, foliage), 1, operation.Handle().fence).Value();
            }

            TerrainResidencyKey Key(std::int32_t x = 0, std::uint64_t content = 1) const {
                SerializedTerrainIdentity bytes{};
                bytes.back() = 1;
                const auto dataset = TerrainDatasetId::Create(bytes).Value();
                return {{dataset, {1, 1}},
                        IdentityFrom<TerrainContentRevision>(content),
                        IdentityFrom<TerrainCapabilityRevision>(1),
                        TerrainTileId{dataset, {x, 0, 0}}};
            }

            WST::SharedAssetKey Allocation(std::uint8_t id = 1) const {
                return {WST::TestSupport::Asset(id), IdentityFrom<WST::SharedAssetRevision>(1)};
            }

            TerrainResidencyLease Insert(const WST::StreamingFeatureBudgetReservation &reservation, std::int32_t x = 0,
                                         std::uint8_t allocation = 1, std::uint64_t content = 1,
                                         TerrainResidencyRetention reason = TerrainResidencyRetention::Cell) {
                auto result = cache->Insert(Key(x, content), reservation, Allocation(allocation), Bytes(10),
                                            std::vector<std::uint8_t>(10, allocation), reason);
                REQUIRE(result.HasValue());
                readers.push_back(result.Value());
                return result.Value();
            }
        };

        TEST_CASE("Terrain cache realizes exact reserved CPU capacity once across bounded reader leases", "[terrain][residency]") {
            Fixture f;
            const auto reservation = f.Admit();
            const auto first = f.Insert(reservation);
            REQUIRE(f.cache->Read(first).Value().size() == 10);
            REQUIRE(f.cache->CpuBytes() == 10);
            REQUIRE(f.authority.Used(Feature::Terrain, Axis::CpuResidentBytes).Value() == 40);
            const auto hit = f.cache->Acquire(f.Key(), reservation, Bytes(), TerrainResidencyRetention::Snapshot).Value();
            f.readers.push_back(hit);
            REQUIRE(f.cache->LeaseCount() == 2);
            REQUIRE(hit.reader != first.reader);
            REQUIRE(f.authority.SharedAssets().LeaseCount() == 1);
            REQUIRE(f.authority.SharedAssets().ChargedCount() == 1);
            REQUIRE(f.cache->Release(first).HasValue());
            REQUIRE(f.authority.SharedAssets().LeaseCount() == 1);
            REQUIRE(f.cache->Read(hit).HasValue());
            REQUIRE(f.cache->EvictTo(0, 1).Value().evicted == 0);
            REQUIRE(f.cache->Release(hit).HasValue());
            REQUIRE(f.cache->EvictTo(0, 1).Value().releasedCpuBytes == 10);
            REQUIRE(f.authority.Used(Feature::Terrain, Axis::CpuResidentBytes).Value() == 30);
            RequireError(f.cache->Read(first), TerrainErrors::GenerationStale);
            RequireError(f.cache->Release(hit), TerrainErrors::GenerationStale);
        }

        TEST_CASE("Terrain cache rejects admitted retirement operations with and without shared readers", "[terrain][residency]") {
            Fixture f;
            const auto reservation = f.Admit();
            const auto first = f.Insert(reservation);
            const auto operation =
                WST::StreamingCellOperation::Create({IdentityFrom<WST::StreamingCellOperationId>(2), reservation.scheduler.operation.fence},
                                                    WST::StreamingCellOperationKind::Retire)
                    .Value();
            const auto retirement = f.authority.TryAdmit(f.authority.Context(), operation, Plan(), 1, operation.Handle().fence).Value();
            REQUIRE(f.authority.Runtime().Scheduler().Inspect(retirement.scheduler).Value().State() ==
                    WST::StreamingCellOperationState::Admitted);
            const auto before = f.authority.Context().revision;
            const auto acquired = f.cache->Acquire(f.Key(), retirement, Bytes(), TerrainResidencyRetention::Cell);
            if (acquired.HasValue())
                f.readers.push_back(acquired.Value());
            RequireError(acquired, WST::WorldStreamingErrors::FeatureBudgetLifecycleUnavailable);
            REQUIRE(f.authority.Context().revision == before);
            REQUIRE(f.cache->LeaseCount() == 1);
            REQUIRE(f.authority.SharedAssets().LeaseCount() == 1);
            REQUIRE(f.cache->Read(first).Value().front() == 1);
            REQUIRE(f.cache->Release(first).HasValue());
            const auto unleasedBefore = f.authority.Context().revision;
            RequireError(f.cache->Acquire(f.Key(), retirement, Bytes(), TerrainResidencyRetention::Cell),
                         WST::WorldStreamingErrors::FeatureBudgetLifecycleUnavailable);
            REQUIRE(f.authority.Context().revision == unleasedBefore);
            REQUIRE(f.cache->LeaseCount() == 0);
            REQUIRE(f.authority.SharedAssets().LeaseCount() == 0);
        }

        TEST_CASE("Terrain LRU follows accepted access order and bounds eviction work", "[terrain][residency]") {
            Fixture f;
            const auto reservation = f.Admit();
            const auto a = f.Insert(reservation, 0, 1);
            const auto b = f.Insert(reservation, 1, 2);
            const auto c = f.Insert(reservation, 2, 3);
            for (const auto &lease : {a, b, c})
                REQUIRE(f.cache->Release(lease).HasValue());
            const auto recent = f.cache->Acquire(f.Key(0), reservation, Bytes(), TerrainResidencyRetention::Snapshot).Value();
            REQUIRE(f.cache->Release(recent).HasValue());
            const auto bounded = f.cache->EvictTo(0, 1).Value();
            REQUIRE(bounded.evicted == 1);
            REQUIRE_FALSE(bounded.targetReached);
            RequireError(f.cache->Acquire(f.Key(1), reservation, Bytes(), TerrainResidencyRetention::Cell), TerrainErrors::IdentityUnknown);
            REQUIRE(f.authority.SharedAssets().Inspect(f.Allocation(2)).Value() == std::nullopt);
            const auto done = f.cache->EvictTo(0, 2).Value();
            REQUIRE(done.evicted == 2);
            REQUIRE(done.targetReached);
        }

        TEST_CASE("Pinned workers and replacement generations retain independent bounded charges", "[terrain][residency]") {
            Fixture f;
            const auto reservation = f.Admit();
            const auto old = f.Insert(reservation, 0, 1, 1, TerrainResidencyRetention::Worker);
            const auto candidate = f.Insert(reservation, 0, 2, 2, TerrainResidencyRetention::Replacement);
            REQUIRE(f.cache->EntryCount() == 2);
            REQUIRE(f.cache->EvictTo(0, 8).Value().evicted == 0);
            REQUIRE(f.authority.Advance(f.authority.Context(), reservation, WST::StreamingCellOperationTransition::Cancel).HasValue());
            RequireError(f.cache->Acquire(f.Key(), reservation, Bytes(), TerrainResidencyRetention::Cell),
                         WST::WorldStreamingErrors::FeatureBudgetLifecycleUnavailable);
            REQUIRE(f.cache->Read(old).Value().front() == 1);
            REQUIRE(f.cache->Read(candidate).Value().front() == 2);
            REQUIRE(f.cache->Release(candidate).HasValue());
            REQUIRE(f.cache->EvictTo(0, 8).Value().releasedCpuBytes == 10);
            REQUIRE(f.cache->Read(old).HasValue());
            f.cache->BeginShutdown();
            f.cache->BeginShutdown();
            REQUIRE_FALSE(f.cache->IsDrained());
            RequireError(f.cache->Acquire(f.Key(), reservation, Bytes(), TerrainResidencyRetention::Snapshot),
                         TerrainErrors::LifecycleUnavailable);
            REQUIRE(f.cache->Release(old).HasValue());
            REQUIRE(f.cache->EvictTo(0, 8).Value().targetReached);
        }

        TEST_CASE("Foliage payloads use only the Foliage reservation slice", "[terrain][residency]") {
            Fixture f;
            const auto reservation = f.Admit(1, 0, 20);
            auto key = f.Key();
            SerializedTerrainIdentity bytes{};
            bytes.back() = 7;
            key.payload = FoliageClusterId::Create(bytes).Value();
            const auto result = f.cache->Insert(key, reservation, f.Allocation(), Bytes(20), std::vector<std::uint8_t>(20, 7),
                                                TerrainResidencyRetention::Consumer);
            REQUIRE(result.HasValue());
            f.readers.push_back(result.Value());
            REQUIRE(f.authority.Used(Feature::Terrain, Axis::CpuResidentBytes).Value() == 0);
            REQUIRE(f.authority.Used(Feature::Foliage, Axis::CpuResidentBytes).Value() == 20);
            RequireError(f.cache->Insert(f.Key(), reservation, f.Allocation(2), Bytes(1), std::vector<std::uint8_t>(1),
                                         TerrainResidencyRetention::Cell),
                         WST::WorldStreamingErrors::FeatureBudgetCapacityExceeded);
            REQUIRE(f.cache->EntryCount() == 1);
        }

        TEST_CASE("Cache rejects unknown costs aliases invalid identities and excess reservation without moving candidates",
                  "[terrain][residency]") {
            Fixture f;
            const auto reservation = f.Admit(1, 10, 0);
            const auto accepted = f.Insert(reservation);
            auto payload = std::vector<std::uint8_t>(10);
            const auto before = f.authority.Context().revision;
            RequireError(f.cache->Insert(f.Key(1), reservation, f.Allocation(2), Bytes(10), std::move(payload),
                                         TerrainResidencyRetention::Cell),
                         WST::WorldStreamingErrors::FeatureBudgetCapacityExceeded);
            REQUIRE(payload.size() == 10);
            REQUIRE(f.authority.Context().revision == before);
            RequireError(f.cache->Insert(f.Key(), reservation, f.Allocation(2), Bytes(10), std::move(payload),
                                         TerrainResidencyRetention::Cell),
                         TerrainErrors::IdentityConflict);
            RequireError(f.cache->Insert(f.Key(1), reservation, f.Allocation(), Bytes(10), std::move(payload),
                                         TerrainResidencyRetention::Cell),
                         TerrainErrors::IdentityConflict);
            RequireError(f.cache->Insert(f.Key(1), reservation, f.Allocation(2), Bytes(9), std::move(payload),
                                         TerrainResidencyRetention::Cell),
                         TerrainErrors::CapacityExceeded);
            RequireError(f.cache->Insert(f.Key(1), reservation, f.Allocation(2), Bytes(10, 1), std::move(payload),
                                         TerrainResidencyRetention::Cell),
                         TerrainErrors::DescriptorInvalid);
            auto invalid = f.Key();
            invalid.runtime = {};
            RequireError(f.cache->Acquire(invalid, reservation, Bytes(), TerrainResidencyRetention::Cell),
                         TerrainErrors::DescriptorInvalid);
            auto stale = f.Key();
            stale.capability = IdentityFrom<TerrainCapabilityRevision>(2);
            RequireError(f.cache->Acquire(stale, reservation, Bytes(), TerrainResidencyRetention::Cell), TerrainErrors::IdentityUnknown);
            auto forged = accepted;
            forged.retention = TerrainResidencyRetention::Consumer;
            RequireError(f.cache->Release(forged), TerrainErrors::GenerationStale);
            REQUIRE(f.cache->LeaseCount() == 1);
        }

        TEST_CASE("Cache metadata ceilings reject admission atomically and shutdown drains under WST closure", "[terrain][residency]") {
            Fixture f({1, 1, 50});
            const auto reservation = f.Admit(1, 20, 0);
            const auto accepted = f.Insert(reservation);
            RequireError(f.cache->Acquire(f.Key(), reservation, Bytes(), TerrainResidencyRetention::Cell), TerrainErrors::CapacityExceeded);
            REQUIRE(f.cache->Release(accepted).HasValue());
            RequireError(f.cache->Insert(f.Key(1), reservation, f.Allocation(2), Bytes(10), std::vector<std::uint8_t>(10),
                                         TerrainResidencyRetention::Cell),
                         TerrainErrors::CapacityExceeded);
            REQUIRE(f.authority.BeginShutdown(f.authority.Context()).HasValue());
            f.cache->BeginShutdown();
            REQUIRE(f.cache->EvictTo(0, 1).Value().targetReached);
            REQUIRE(f.cache->IsDrained());
            RequireError(f.cache->EvictTo(0, 0), TerrainErrors::DescriptorInvalid);
            RequireError(TerrainResidencyCache::Create(IdentityFrom<TerrainResidencyOwnerId>(2), f.authority),
                         TerrainErrors::LifecycleUnavailable);
        }
    }  // namespace
}  // namespace Horo::Terrain
