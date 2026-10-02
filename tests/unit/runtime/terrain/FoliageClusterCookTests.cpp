#include "Horo/Terrain/FoliageClusterCook.h"

#include <algorithm>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>
#include <utility>

namespace Horo::Terrain {
    namespace {
        template <typename Identity> Identity Id(const std::uint8_t marker) {
            SerializedTerrainIdentity bytes{};
            bytes[0] = marker;
            return Identity::Create(bytes).Value();
        }

        template <typename Revision> Revision Rev(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        Sha256Digest Digest(const std::uint8_t marker) {
            Sha256Digest digest;
            digest.bytes[0] = marker;
            return digest;
        }

        TerrainConfigurationSnapshot Configuration(const TerrainDescriptorLimits &limits) {
            auto result = TerrainConfigurationSnapshot::Create(
                {.configuration = Rev<TerrainConfigurationRevision>(1), .capability = Rev<TerrainCapabilityRevision>(1), .limits = limits});
            REQUIRE(result.HasValue());
            return result.Value();
        }

        FoliageClusterCookProfile Profile() {
            return {Configuration(GetTerrainTierProfile(TerrainFeatureTier::Baseline).Value().limits), Digest(7), Digest(8)};
        }

        struct PlacementTarget final {
            Sha256Digest target = Digest(7);
            Sha256Digest toolchain = Digest(8);
            TerrainFeatureTier tier = TerrainFeatureTier::Baseline;
        };

        FoliageTypeDefinition Definition(const std::uint8_t type) {
            const auto configuration = Profile().configuration;
            constexpr FoliageDefinitionCapabilitySet capabilities{FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::CpuCulling>};
            FoliageTypeDefinitionData data;
            data.type = Id<FoliageTypeId>(type);
            data.revision = Rev<FoliageDefinitionRevision>(1);
            data.assets.meshLods[0] = Id<FoliageMeshAssetId>(3);
            data.assets.meshLodCount = 1;
            data.assets.material = Id<FoliageMaterialAssetId>(4);
            data.placement.seed = 7;
            data.placement.densityPerSquareKilometer = 10'000;
            data.placement.minimumAltitudeMillimeters = 0;
            data.placement.maximumAltitudeMillimeters = 2'000;
            data.placement.maximumSlopeMilliDegrees = 40'000;
            data.placement.minimumSeparationMillimeters = 100;
            data.placement.coordinateQuantumMillimeters = 10;
            data.placement.alignment = FoliageSurfaceAlignment::SurfaceNormal;
            data.culling.meshLodDistanceMillimeters[0] = 10'000;
            data.culling.cullDistanceMillimeters = 20'000;
            data.maximumInstances = 1'000;
            data.minimumScalePermille = 1'101;
            data.maximumScalePermille = 1'101;
            auto definition = FoliageTypeDefinition::Create(data, configuration, capabilities);
            REQUIRE(definition.HasValue());
            return definition.Value();
        }

        CookedFoliagePlacement Placement(const std::int32_t tileX = 0, const std::uint64_t revision = 1,
                                         const std::uint64_t sourceRevision = 1, const bool empty = false, const std::uint8_t type = 2,
                                         const PlacementTarget &target = {}) {
            const auto definition = Definition(type);
            constexpr FoliageDefinitionCapabilitySet capabilities{FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::CpuCulling>};
            std::vector<FoliagePlacementSample> samples(25);
            for (auto &sample : samples) {
                sample.altitudeMillimeters = 1'000;
                sample.density = empty ? 0 : 65'535;
                sample.slopeMilliDegrees = 10'000;
                sample.normalXPermille = -100;
                sample.normalYPermille = 995;
            }
            FoliagePlacementGrid grid{.tile = {Id<TerrainDatasetId>(1), {tileX, 0, 0}},
                                      .sourceRevision = Rev<TerrainSourceRevision>(sourceRevision),
                                      .capability = Rev<TerrainCapabilityRevision>(1),
                                      .originXMillimeters = -20'000 + std::int64_t{tileX} * 40'000,
                                      .originZMillimeters = -20'000,
                                      .spacingXMillimeters = 10'000,
                                      .spacingZMillimeters = 10'000,
                                      .width = 5,
                                      .height = 5,
                                      .samples = samples};
            FoliagePlacementCookRequest
                request{grid,          definition,       capabilities, {.cellsPerCluster = 2}, {}, Rev<TerrainContentRevision>(revision),
                        target.target, target.toolchain, target.tier};
            auto result = CookFoliagePlacement(request, {});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        struct Fixture final {
            CookedFoliagePlacement first = Placement();
            CookedFoliagePlacement second = Placement(1);
            std::array<FoliageClusterCookSource, 2> sources{{{&first, Digest(9), 101}, {&second, Digest(10), 200}}};
            FoliageClusterCookProfile profile = Profile();

            FoliageClusterCookRequest Request(const std::uint64_t content = 1, const CookedFoliageClusterSet *previous = nullptr) const {
                return {Id<TerrainDatasetId>(1), Rev<TerrainContentRevision>(content), profile, sources, previous};
            }

            void Limits(const TerrainDescriptorLimits &limits) {
                profile.configuration = Configuration(limits);
            }
        };

        template <typename T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
            CHECK(result.ErrorValue().severity == expected.defaultSeverity);
        }

        std::vector<FoliageInstanceId> InstanceIds(const CookedFoliageClusterSet &set) {
            std::vector<FoliageInstanceId> ids;
            for (const auto &cluster : set.Clusters())
                for (const auto &instance : cluster.instances)
                    ids.push_back(instance.id);
            std::ranges::sort(ids);
            return ids;
        }
    }  // namespace

    TEST_CASE("Foliage clusters canonicalize complete source order and preserve placement identities", "[terrain][foliage][cluster]") {
        Fixture fixture;
        auto cooked = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(cooked.HasValue());
        std::ranges::reverse(fixture.sources);
        auto repeated = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(repeated.HasValue());
        CHECK(cooked.Value().Fingerprint() == repeated.Value().Fingerprint());
        CHECK(cooked.Value().ManifestDigest() == repeated.Value().ManifestDigest());
        REQUIRE(cooked.Value().Clusters().size() == 8);
        CHECK(cooked.Value().Footprint().activeFoliageInstances == fixture.first.Instances().size() + fixture.second.Instances().size());
        for (std::size_t index = 0; index < cooked.Value().Clusters().size(); ++index) {
            const auto &cluster = cooked.Value().Clusters()[index];
            CHECK(cluster.payload == repeated.Value().Clusters()[index].payload);
            CHECK(std::ranges::is_sorted(cluster.instances, {}, &CookedFoliageInstance::id));
            CHECK(VerifyFoliageClusterPayload(cluster, cluster.payload).HasValue());
            const auto &placement = cluster.tile == fixture.first.Tile() ? fixture.first : fixture.second;
            for (const auto &instance : cluster.instances)
                CHECK(std::ranges::find(placement.Instances(), instance) != placement.Instances().end());
        }
    }

    TEST_CASE("Foliage cluster schema bytes have fixed network order and no padding", "[terrain][foliage][cluster]") {
        Fixture fixture;
        const auto cooked = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(cooked.HasValue());
        const auto &cluster = cooked.Value().Clusters().front();
        REQUIRE(cluster.payload.size() == 289 + 128 * cluster.instances.size());
        constexpr std::array<std::uint8_t, 8> version{0, 0, 0, 0, 0, 0, 0, 1};
        CHECK(std::ranges::equal(std::span{cluster.payload}.first(8), version));
        CHECK(cluster.payload[8] == 1);  // Dataset identity starts the canonical 25-byte tile.
        const auto &instance = cluster.instances.front();
        CHECK(std::ranges::equal(std::span{cluster.payload}.subspan(289, 16), instance.id.Bytes()));
        const auto x = static_cast<std::uint64_t>(instance.xMillimeters);
        for (std::size_t byte = 0; byte < 8; ++byte)
            CHECK(std::as_bytes(std::span{cluster.payload})[289 + 48 + byte] == static_cast<std::byte>(x >> ((7 - byte) * 8)));
        CHECK(cluster.payload[289 + 48 + 7 * 8 + 6] == 0xFF);
        CHECK(cluster.payload[289 + 48 + 7 * 8 + 7] == 0x9C);  // Canonical uint16 bit pattern for normal X = -100.
        auto damaged = cluster.payload;
        damaged.back() = std::to_integer<std::uint8_t>(static_cast<std::byte>(damaged.back()) ^ std::byte{1});
        ErrorIs(VerifyFoliageClusterPayload(cluster, damaged), FoliageClusterCookErrors::InvalidInput);
        ErrorIs(VerifyFoliageClusterPayload(cluster, std::span{cluster.payload}.first(cluster.payload.size() - 1)),
                FoliageClusterCookErrors::InvalidInput);
        damaged.push_back(0);
        ErrorIs(VerifyFoliageClusterPayload(cluster, damaged), FoliageClusterCookErrors::InvalidInput);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        ErrorIs(VerifyFoliageClusterPayload(cluster, cluster.payload, cancellation.Token()), FoliageClusterCookErrors::Cancelled);
    }

    TEST_CASE("Foliage cluster bounds enclose scaled geometry at negative coordinates", "[terrain][foliage][cluster]") {
        Fixture fixture;
        const auto cooked = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(cooked.HasValue());
        bool hasNegative{};
        for (const auto &cluster : cooked.Value().Clusters()) {
            FoliageClusterBounds exact;
            bool first = true;
            for (const auto &instance : cluster.instances) {
                const auto radius =
                    static_cast<std::int64_t>((std::uint64_t{cluster.geometryRadiusMillimeters} * instance.scalePermille + 999) / 1'000);
                CHECK(radius == (cluster.geometryRadiusMillimeters == 101 ? 112 : 221));
                const std::array coordinates{instance.xMillimeters, instance.altitudeMillimeters, instance.zMillimeters};
                hasNegative |= coordinates[0] < 0;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    exact.minimum[axis] = first ? coordinates[axis] - radius : std::min(exact.minimum[axis], coordinates[axis] - radius);
                    exact.maximum[axis] = first ? coordinates[axis] + radius : std::max(exact.maximum[axis], coordinates[axis] + radius);
                }
                first = false;
            }
            CHECK(cluster.bounds == exact);
        }
        CHECK(hasNegative);
    }

    TEST_CASE("Incremental cluster replacement leaves unrelated tile payloads and IDs unchanged", "[terrain][foliage][cluster]") {
        Fixture fixture;
        auto original = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(original.HasValue());
        fixture.first = Placement(0, 2, 2);
        fixture.sources[0].geometryDigest = Digest(11);
        auto replacement = CookFoliageClusters(fixture.Request(2, &original.Value()), {});
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value().ManifestDigest() != original.Value().ManifestDigest());
        CHECK(InstanceIds(replacement.Value()) == InstanceIds(original.Value()));
        for (const auto &old : original.Value().Clusters()) {
            const auto found = std::ranges::find(replacement.Value().Clusters(), old.id, &CookedFoliageCluster::id);
            REQUIRE(found != replacement.Value().Clusters().end());
            if (old.tile == fixture.second.Tile()) {
                CHECK(found->payload == old.payload);
                CHECK(found->digest == old.digest);
            } else
                CHECK(found->digest != old.digest);
        }
    }

    TEST_CASE("Foliage cluster identity separates types and replacement removal does not renumber survivors",
              "[terrain][foliage][cluster]") {
        Fixture fixture;
        fixture.second = Placement(0, 1, 1, false, 5);
        auto original = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(original.HasValue());
        fixture.first = Placement(0, 2, 2, true);
        auto replacement = CookFoliageClusters(fixture.Request(2, &original.Value()), {});
        REQUIRE(replacement.HasValue());
        REQUIRE(replacement.Value().Clusters().size() == 4);
        for (const auto &cluster : replacement.Value().Clusters()) {
            CHECK(cluster.type == fixture.second.Type());
            auto old = std::ranges::find(original.Value().Clusters(), cluster.id, &CookedFoliageCluster::id);
            REQUIRE(old != original.Value().Clusters().end());
            CHECK(cluster.payload == old->payload);
        }
    }

    TEST_CASE("Foliage cluster fingerprint invalidates target geometry profile and empty-source provenance",
              "[terrain][foliage][cluster]") {
        Fixture fixture;
        const auto baseline = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(baseline.HasValue());
        for (int mutation = 0; mutation < 4; ++mutation) {
            Fixture changed;
            if (mutation == 0) {
                changed.profile.targetDigest = Digest(12);
                changed.first = Placement(0, 1, 1, false, 2, {.target = Digest(12)});
                changed.second = Placement(1, 1, 1, false, 2, {.target = Digest(12)});
            }
            if (mutation == 1) {
                changed.profile.toolchainDigest = Digest(12);
                changed.first = Placement(0, 1, 1, false, 2, {.toolchain = Digest(12)});
                changed.second = Placement(1, 1, 1, false, 2, {.toolchain = Digest(12)});
            }
            if (mutation == 2)
                changed.sources[0].geometryRadiusMillimeters += 1;
            if (mutation == 3) {
                auto limits = changed.profile.configuration.Data().limits;
                --limits.maximumWorkItems;
                changed.Limits(limits);
            }
            const auto result = CookFoliageClusters(changed.Request(), {});
            REQUIRE(result.HasValue());
            CHECK(result.Value().Fingerprint() != baseline.Value().Fingerprint());
            CHECK(result.Value().Clusters().front().digest != baseline.Value().Clusters().front().digest);
        }
        fixture.first = Placement(0, 1, 1, true);
        const auto emptyBase = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(emptyBase.HasValue());
        fixture.first = Placement(0, 1, 2, true);
        const auto changedEmpty = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(changedEmpty.HasValue());
        CHECK(changedEmpty.Value().Fingerprint() != emptyBase.Value().Fingerprint());
        CHECK(changedEmpty.Value().Clusters().front().payload == emptyBase.Value().Clusters().front().payload);
    }

    TEST_CASE("Foliage cluster rejects missing duplicate foreign and moved-from source evidence", "[terrain][foliage][cluster]") {
        Fixture fixture;
        for (int mutation = 0; mutation < 7; ++mutation) {
            auto request = fixture.Request();
            auto sources = fixture.sources;
            request.sources = sources;
            if (mutation == 0)
                sources[0].placement = nullptr;
            if (mutation == 1)
                sources[0].geometryDigest = {};
            if (mutation == 2)
                sources[0].geometryRadiusMillimeters = 0;
            if (mutation == 3)
                sources[0].geometryRadiusMillimeters = 1'000'000'001;
            if (mutation == 4)
                sources[1] = sources[0];
            if (mutation == 5)
                request.dataset = Id<TerrainDatasetId>(42);
            if (mutation == 6)
                request.profile.targetDigest = {};
            ErrorIs(CookFoliageClusters(request, {}), FoliageClusterCookErrors::InvalidInput);
        }
        auto retained = std::move(fixture.first);
        ErrorIs(CookFoliageClusters(fixture.Request(), {}), FoliageClusterCookErrors::InvalidInput);
        CHECK_FALSE(retained.Instances().empty());
    }

    TEST_CASE("Foliage cluster requires matching target toolchain and exact tier without fallback", "[terrain][foliage][cluster]") {
        Fixture fixture;
        auto request = fixture.Request();
        request.profile.targetDigest = Digest(12);
        ErrorIs(CookFoliageClusters(request, {}), FoliageClusterCookErrors::InvalidInput);
        request = fixture.Request();
        request.profile.toolchainDigest = Digest(12);
        ErrorIs(CookFoliageClusters(request, {}), FoliageClusterCookErrors::InvalidInput);
        for (const auto tier : {TerrainFeatureTier::Standard, TerrainFeatureTier::High, TerrainFeatureTier::Ultra}) {
            auto data = fixture.profile.configuration.Data();
            data.tier = tier;
            data.limits = GetTerrainTierProfile(tier).Value().limits;
            fixture.profile.configuration = TerrainConfigurationSnapshot::Create(data).Value();
            ErrorIs(CookFoliageClusters(fixture.Request(), {}), FoliageClusterCookErrors::InvalidInput);
            fixture.first = Placement(0, 1, 1, false, 2, {.tier = tier});
            fixture.second = Placement(1, 1, 1, false, 2, {.tier = tier});
            auto cooked = CookFoliageClusters(fixture.Request(), {});
            REQUIRE(cooked.HasValue());
            CHECK(cooked.Value().Profile().configuration.Data().tier == tier);
        }
    }

    TEST_CASE("Foliage cluster output count limits reject without truncation", "[terrain][foliage][cluster]") {
        Fixture fixture;
        const auto baseline = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(baseline.HasValue());
        auto limits = fixture.profile.configuration.Data().limits;
        limits.maximumActiveFoliageClusters = 7;
        fixture.Limits(limits);
        ErrorIs(CookFoliageClusters(fixture.Request(), {}), FoliageClusterCookErrors::LimitExceeded);
        limits = Profile().configuration.Data().limits;
        limits.maximumActiveFoliageClusters = baseline.Value().Footprint().activeFoliageClusters;
        limits.maximumActiveFoliageInstances = baseline.Value().Footprint().activeFoliageInstances - 1;
        fixture.Limits(limits);
        ErrorIs(CookFoliageClusters(fixture.Request(), {}), FoliageClusterCookErrors::LimitExceeded);
        limits.maximumActiveFoliageInstances += 1;
        fixture.Limits(limits);
        CHECK(CookFoliageClusters(fixture.Request(), {}).HasValue());
    }

    TEST_CASE("Foliage cluster bytes and work have exact positive boundaries", "[terrain][foliage][cluster]") {
        Fixture fixture;
        const auto baseline = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(baseline.HasValue());
        const auto footprint = baseline.Value().Footprint();
        for (int boundary = 0; boundary < 3; ++boundary) {
            auto limits = Profile().configuration.Data().limits;
            if (boundary == 0)
                limits.maximumResidentFoliageBytes = footprint.residentFoliageBytes;
            if (boundary == 1)
                limits.maximumStagingBytes = footprint.stagingBytes;
            if (boundary == 2) {
                limits.maximumWorkItems = footprint.workItems;
                limits.maximumActiveFoliageClusters = footprint.activeFoliageClusters;
                limits.maximumActiveFoliageInstances = footprint.activeFoliageInstances;
            }
            fixture.Limits(limits);
            CHECK(CookFoliageClusters(fixture.Request(), {}).HasValue());
            if (boundary == 0)
                --limits.maximumResidentFoliageBytes;
            if (boundary == 1)
                --limits.maximumStagingBytes;
            if (boundary == 2)
                --limits.maximumWorkItems;
            fixture.Limits(limits);
            ErrorIs(CookFoliageClusters(fixture.Request(), {}), FoliageClusterCookErrors::LimitExceeded);
        }
    }

    TEST_CASE("Foliage cluster accounts old generation retirement and fences stale successors", "[terrain][foliage][cluster]") {
        Fixture fixture;
        const auto original = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(original.HasValue());
        ErrorIs(CookFoliageClusters(fixture.Request(1, &original.Value()), {}), FoliageClusterCookErrors::Stale);
        ErrorIs(CookFoliageClusters(fixture.Request(3, &original.Value()), {}), FoliageClusterCookErrors::Stale);
        auto limits = fixture.profile.configuration.Data().limits;
        limits.maximumRetiringBytes = original.Value().Footprint().residentFoliageBytes - 1;
        fixture.Limits(limits);
        ErrorIs(CookFoliageClusters(fixture.Request(2, &original.Value()), {}), FoliageClusterCookErrors::LimitExceeded);
        ++limits.maximumRetiringBytes;
        fixture.Limits(limits);
        auto replacement = CookFoliageClusters(fixture.Request(2, &original.Value()), {});
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value().Footprint().retiringBytes == original.Value().Footprint().residentFoliageBytes);
    }

    TEST_CASE("Foliage cluster owner preserves roots across cancellation stale replacement and shutdown", "[terrain][foliage][cluster]") {
        Fixture fixture;
        auto first = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(first.HasValue());
        const auto &retained = first.Value();
        FoliageClusterCookOwner owner;
        CHECK(owner.Publish(first.Value(), std::nullopt).HasValue());
        auto next = CookFoliageClusters(fixture.Request(2, owner.Current()), {});
        REQUIRE(next.HasValue());
        auto limits = fixture.profile.configuration.Data().limits;
        limits.maximumRetiringBytes = retained.Footprint().residentFoliageBytes - 1;
        fixture.Limits(limits);
        auto withoutPredecessor = CookFoliageClusters(fixture.Request(2), {});
        REQUIRE(withoutPredecessor.HasValue());
        CHECK(withoutPredecessor.Value().Footprint().retiringBytes == 0);
        ErrorIs(owner.Publish(withoutPredecessor.Value(), Rev<TerrainContentRevision>(1)), FoliageClusterCookErrors::LimitExceeded);
        CHECK(owner.Current()->ManifestDigest() == retained.ManifestDigest());
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        ErrorIs(CookFoliageClusters(fixture.Request(), cancellation.Token()), FoliageClusterCookErrors::Cancelled);
        ErrorIs(owner.Publish(next.Value(), Rev<TerrainContentRevision>(1), cancellation.Token()), FoliageClusterCookErrors::Cancelled);
        CHECK(owner.Current()->ManifestDigest() == retained.ManifestDigest());
        ErrorIs(owner.Publish(next.Value(), Rev<TerrainContentRevision>(2)), FoliageClusterCookErrors::Stale);
        CHECK(owner.Publish(next.Value(), Rev<TerrainContentRevision>(1)).HasValue());
        CHECK(owner.Current()->ContentRevision() == Rev<TerrainContentRevision>(2));
        CHECK(VerifyFoliageClusterPayload(retained.Clusters().front(), retained.Clusters().front().payload).HasValue());
        owner.Close();
        owner.Close();
        ErrorIs(owner.Publish(next.Value(), Rev<TerrainContentRevision>(2)), FoliageClusterCookErrors::Closed);
        CHECK(owner.Current()->ManifestDigest() == next.Value().ManifestDigest());
    }

    TEST_CASE("Foliage cluster empty membership and explicit disabled foliage are distinct", "[terrain][foliage][cluster]") {
        Fixture fixture;
        auto request = fixture.Request();
        request.sources = {};
        auto empty = CookFoliageClusters(request, {});
        REQUIRE(empty.HasValue());
        CHECK(empty.Value().Clusters().empty());
        FoliageClusterCookOwner owner;
        CHECK(owner.Publish(empty.Value(), std::nullopt).HasValue());
        auto limits = fixture.profile.configuration.Data().limits;
        limits.maximumActiveFoliageClusters = 0;
        limits.maximumActiveFoliageInstances = 0;
        limits.maximumResidentFoliageBytes = 0;
        fixture.Limits(limits);
        ErrorIs(CookFoliageClusters(fixture.Request(), {}), FoliageClusterCookErrors::LimitExceeded);
    }

    TEST_CASE("Foliage cluster capability capture and moved-from roots fail without replacing current output",
              "[terrain][foliage][cluster]") {
        Fixture fixture;
        auto data = fixture.profile.configuration.Data();
        data.capability = Rev<TerrainCapabilityRevision>(2);
        fixture.profile.configuration = TerrainConfigurationSnapshot::Create(data).Value();
        ErrorIs(CookFoliageClusters(fixture.Request(), {}), FoliageClusterCookErrors::InvalidInput);
        fixture.profile = Profile();
        auto cooked = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(cooked.HasValue());
        auto retained = std::move(cooked).Value();
        auto moved = std::move(retained);
        FoliageClusterCookOwner owner;
        ErrorIs(CookFoliageClusters(fixture.Request(2, &retained), {}), FoliageClusterCookErrors::InvalidInput);
        CHECK(owner.Current() == nullptr);
        CHECK(owner.Publish(moved, std::nullopt).HasValue());
        ErrorIs(owner.Publish(moved, std::nullopt), FoliageClusterCookErrors::Stale);
    }

    TEST_CASE("Foliage cluster revision exhaustion cannot wrap owner replacement", "[terrain][foliage][cluster]") {
        Fixture fixture;
        auto terminal = CookFoliageClusters(fixture.Request(std::numeric_limits<std::uint64_t>::max()), {});
        REQUIRE(terminal.HasValue());
        FoliageClusterCookOwner owner;
        CHECK(owner.Publish(terminal.Value(), std::nullopt).HasValue());
        ErrorIs(CookFoliageClusters(fixture.Request(1, owner.Current()), {}), FoliageClusterCookErrors::Stale);
        auto fresh = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(fresh.HasValue());
        ErrorIs(owner.Publish(fresh.Value(), terminal.Value().ContentRevision()), FoliageClusterCookErrors::Stale);
        CHECK(owner.Current()->ManifestDigest() == terminal.Value().ManifestDigest());
    }

    TEST_CASE("Foliage cluster worker cooks match the owner-thread canonical artifact", "[terrain][foliage][cluster]") {
        Fixture fixture;
        const auto baseline = CookFoliageClusters(fixture.Request(), {});
        REQUIRE(baseline.HasValue());
        std::array<std::optional<CookedFoliageClusterSet>, 2> results;
        std::jthread first([&fixture, &results] {
            auto result = CookFoliageClusters(fixture.Request(), {});
            if (result.HasValue())
                results[0] = std::move(result).Value();
        });
        std::jthread second([&fixture, &results] {
            auto result = CookFoliageClusters(fixture.Request(), {});
            if (result.HasValue())
                results[1] = std::move(result).Value();
        });
        first.join();
        second.join();
        for (const auto &result : results) {
            REQUIRE(result.has_value());
            CHECK(result->ManifestDigest() == baseline.Value().ManifestDigest());
            CHECK(result->Fingerprint() == baseline.Value().Fingerprint());
            for (std::size_t index = 0; index < result->Clusters().size(); ++index)
                CHECK(result->Clusters()[index].payload == baseline.Value().Clusters()[index].payload);
        }
    }

    TEST_CASE("Foliage cluster concurrent cancellation returns a complete candidate or cancellation only", "[terrain][foliage][cluster]") {
        Fixture fixture;
        CancellationSource cancellation;
        std::barrier start{2};
        std::jthread cancel([&start, &cancellation] {
            start.arrive_and_wait();
            cancellation.RequestCancellation();
        });
        start.arrive_and_wait();
        auto result = CookFoliageClusters(fixture.Request(), cancellation.Token());
        cancel.join();
        if (result.HasError())
            ErrorIs(result, FoliageClusterCookErrors::Cancelled);
        else {
            CHECK(result.Value().Clusters().size() == 8);
            for (const auto &cluster : result.Value().Clusters())
                CHECK(VerifyFoliageClusterPayload(cluster, cluster.payload).HasValue());
        }
    }
}  // namespace Horo::Terrain
