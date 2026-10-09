#include "Horo/Terrain/TerrainPayloadManifest.h"
#include "support/TypedIdentityTestSupport.h"

#include <algorithm>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <thread>

namespace Horo::Terrain {
    namespace {
        template <typename Identity> Identity Id(const std::uint8_t marker) {
            return Tests::ByteIdentity<Identity>(marker);
        }

        template <typename Revision> Revision Rev(const std::uint64_t value) {
            return Tests::IdentityValue<Revision>(value);
        }

        Sha256Digest Digest(const std::uint8_t marker) {
            Sha256Digest digest;
            digest.bytes[0] = marker;
            return digest;
        }

        TerrainTileCookDependency Dependency(const std::uint8_t marker) {
            SerializedTerrainIdentity bytes{};
            bytes[0] = marker;
            return {Assets::AssetId::FromBytes(bytes), Digest(marker)};
        }

        TerrainCanonicalSource Source() {
            TerrainCanonicalSource source;
            source.dataset = Id<TerrainDatasetId>(1);
            source.sourceAsset = Dependency(2).asset;
            source.revision = Rev<TerrainSourceRevision>(1);
            source.capability = Rev<TerrainCapabilityRevision>(1);
            source.width = 5;
            source.height = 5;
            source.coordinates.originX = -20;
            source.coordinates.originZ = -20;
            source.coordinates.spacingX = 10;
            source.coordinates.spacingZ = 10;
            source.heightsMeters.assign(25, 1);
            return source;
        }

        TerrainSourceArtifactProfile Profile() {
            TerrainSourceArtifactProfile profile;
            profile.tiles.interiorQuads = 2;
            profile.tiles.lodLevels = 2;
            profile.tiles.targetDigest = Digest(7);
            profile.tiles.toolchainDigest = Digest(8);
            return profile;
        }

        TerrainConfigurationSnapshot Configuration() {
            return TerrainConfigurationSnapshot::Create({.configuration = Rev<TerrainConfigurationRevision>(1),
                                                         .capability = Rev<TerrainCapabilityRevision>(1),
                                                         .limits = GetTerrainTierProfile(TerrainFeatureTier::Baseline).Value().limits})
                .Value();
        }

        CookedFoliageClusterSet Foliage(const TerrainTileId tile, const std::uint64_t content = 1, const std::uint64_t sourceRevision = 1,
                                        const bool empty = false) {
            const auto configuration = Configuration();
            constexpr FoliageDefinitionCapabilitySet capabilities{FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::CpuCulling>};
            FoliageTypeDefinitionData data;
            data.type = Id<FoliageTypeId>(3);
            data.revision = Rev<FoliageDefinitionRevision>(1);
            data.assets.meshLods[0] = Id<FoliageMeshAssetId>(4);
            data.assets.meshLodCount = 1;
            data.assets.material = Id<FoliageMaterialAssetId>(5);
            data.placement.densityPerSquareKilometer = 100'000;
            data.placement.maximumAltitudeMillimeters = 2'000;
            data.placement.maximumSlopeMilliDegrees = 40'000;
            data.placement.minimumSeparationMillimeters = 100;
            data.placement.coordinateQuantumMillimeters = 10;
            data.culling.meshLodDistanceMillimeters[0] = 10'000;
            data.culling.cullDistanceMillimeters = 20'000;
            data.maximumInstances = 1'000;
            auto definition = FoliageTypeDefinition::Create(data, configuration, capabilities);
            REQUIRE(definition.HasValue());
            std::vector<FoliagePlacementSample> samples(9);
            for (auto &sample : samples) {
                sample.altitudeMillimeters = 1'000;
                sample.density = empty ? 0 : 65'535;
            }
            FoliagePlacementGrid grid{.tile = tile,
                                      .sourceRevision = Rev<TerrainSourceRevision>(sourceRevision),
                                      .capability = Rev<TerrainCapabilityRevision>(1),
                                      .originXMillimeters = -20'000,
                                      .originZMillimeters = -20'000,
                                      .spacingXMillimeters = 10'000,
                                      .spacingZMillimeters = 10'000,
                                      .width = 3,
                                      .height = 3,
                                      .samples = samples};
            FoliagePlacementCookRequest placementRequest{grid,         definition.Value(),
                                                         capabilities, {},
                                                         {},           Rev<TerrainContentRevision>(content),
                                                         Digest(7),    Digest(8)};
            const auto placement = CookFoliagePlacement(placementRequest, {});
            REQUIRE(placement.HasValue());
            const FoliageClusterCookSource source{&placement.Value(), Digest(9), 100};
            const FoliageClusterCookRequest request{tile.dataset,
                                                    Rev<TerrainContentRevision>(content),
                                                    {configuration, Digest(7), Digest(8)},
                                                    std::span{&source, 1}};
            auto clusters = CookFoliageClusters(request, {});
            REQUIRE(clusters.HasValue());
            return std::move(clusters).Value();
        }

        struct Fixture final {
            TerrainCanonicalSource source = Source();
            std::array<TerrainTileCookDependency, 2> dependencies{Dependency(10), Dependency(11)};
            std::array<TerrainTileCookDependency, 2> foliageDependencies{Dependency(9), Dependency(12)};
            CookedTerrainSourceArtifacts terrain = CookTerrainSourceArtifacts(source, Profile(), dependencies, {}).Value();
            CookedFoliageClusterSet foliage = Foliage(terrain.Tiles().tiles.front().id);

            TerrainPayloadManifestRequest Request(const std::uint64_t content = 1) const {
                TerrainPayloadManifestRequest request;
                request.terrain = &terrain;
                request.foliage = &foliage;
                request.content = Rev<TerrainContentRevision>(content);
                request.terrainDependencies = dependencies;
                request.foliageDependencies = foliageDependencies;
                request.verifiedFoliageDependencyClosure = TerrainPayloadDependencyClosureDigest(foliageDependencies).Value();
                request.terrainRequirements = {TerrainPayloadRequirement::Optional, TerrainPayloadRequirement::Required,
                                               TerrainPayloadRequirement::Required};
                request.foliageRequirements.visual = TerrainPayloadRequirement::Optional;
                return request;
            }

            TerrainPayloadManifestRequest TerrainOnly(const std::uint64_t content = 1) const {
                auto request = Request(content);
                request.foliage = nullptr;
                request.foliageDependencies = {};
                request.verifiedFoliageDependencyClosure = {};
                request.foliageRequirements = {};
                return request;
            }
        };

        template <typename T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &error) {
            Tests::RequireFailureIdentity(result, error);
        }
    }  // namespace

    TEST_CASE("Payload manifest binds actual tile geometry clusters dependencies and requirements", "[terrain][payload-manifest]") {
        Fixture fixture;
        const auto result = GenerateTerrainPayloadManifest(fixture.Request());
        REQUIRE(result.HasValue());
        const auto &manifest = result.Value();
        CHECK(manifest.Provenance().dataset == fixture.source.dataset);
        CHECK(manifest.Provenance().sourceAsset == fixture.source.sourceAsset);
        CHECK(manifest.Provenance().source == fixture.source.revision);
        CHECK(manifest.Provenance().capability == fixture.source.capability);
        CHECK(manifest.Provenance().tileFingerprint == fixture.terrain.Tiles().fingerprint);
        CHECK(manifest.Provenance().geometryManifestDigest == fixture.terrain.ManifestDigest());
        CHECK(manifest.Provenance().foliageManifestDigest == fixture.foliage.ManifestDigest());
        CHECK(manifest.Provenance().foliageDependencies == fixture.Request().verifiedFoliageDependencyClosure);
        REQUIRE(manifest.Tiles().size() == 5);
        REQUIRE(manifest.Clusters().size() == fixture.foliage.Clusters().size());
        CHECK_FALSE(manifest.Clusters().empty());
        CHECK(manifest.Bounds().minimum[0] <= -20);
        CHECK(manifest.Bounds().maximum[0] >= 20);
        CHECK(manifest.TerrainRequirements().collision == TerrainPayloadRequirement::Required);
        CHECK(manifest.Tiles().front().consumers[1]->schema == CurrentTerrainSourceArtifactSchema);
        CHECK(manifest.Tiles().back().consumers[1] == std::nullopt);  // Collision is cooked at the explicit LOD 0.
        CHECK(manifest.Digest() == ComputeSha256(std::as_bytes(manifest.Bytes())));
        CHECK(VerifyTerrainPayloadManifest(manifest, manifest.Bytes()).HasValue());
        CHECK(ValidateTerrainPayloadManifestPublication(manifest, nullptr, std::nullopt, TerrainRuntimeLifecycle::Active).HasValue());
        for (std::size_t index = 0; index < manifest.Clusters().size(); ++index) {
            const auto &entry = manifest.Clusters()[index];
            const auto &cluster = fixture.foliage.Clusters()[index];
            CHECK(entry.cluster == cluster.id);
            CHECK(entry.bounds == cluster.bounds);
            CHECK(entry.instances.digest == cluster.digest);
            CHECK(entry.instanceCount == cluster.instances.size());
            CHECK(entry.placementFingerprint == cluster.placementFingerprint);
        }
    }

    TEST_CASE("Payload manifest encoding is canonical across dependency order and independent workers", "[terrain][payload-manifest]") {
        Fixture fixture;
        const auto baseline = GenerateTerrainPayloadManifest(fixture.Request());
        REQUIRE(baseline.HasValue());
        std::ranges::reverse(fixture.dependencies);
        std::ranges::reverse(fixture.foliageDependencies);
        const auto reordered = GenerateTerrainPayloadManifest(fixture.Request());
        REQUIRE(reordered.HasValue());
        CHECK(std::ranges::equal(baseline.Value().Bytes(), reordered.Value().Bytes()));
        const std::array<std::uint8_t, 10> prefix{4, 0, 'H', 'T', 'P', 'M', 1, 0, 0, 0};
        CHECK(std::ranges::equal(baseline.Value().Bytes().first(10), prefix));
        std::optional<TerrainPayloadManifest> worker;
        std::jthread thread([&fixture, &worker] {
            auto result = GenerateTerrainPayloadManifest(fixture.Request());
            if (result.HasValue())
                worker.emplace(std::move(result).Value());
        });
        thread.join();
        REQUIRE(worker.has_value());
        CHECK(worker->Digest() == baseline.Value().Digest());
    }

    TEST_CASE("Payload manifest rejects missing conflicting and stale exact dependency closure", "[terrain][payload-manifest]") {
        Fixture fixture;
        auto request = fixture.Request();
        request.terrainDependencies = {};
        ErrorIs(GenerateTerrainPayloadManifest(request), TerrainPayloadManifestErrors::Invalid);
        request = fixture.Request();
        fixture.dependencies[1] = fixture.dependencies[0];
        ErrorIs(GenerateTerrainPayloadManifest(request), TerrainPayloadManifestErrors::Invalid);
        fixture.dependencies[1] = Dependency(11);
        request = fixture.Request();
        request.verifiedFoliageDependencyClosure = Digest(55);
        ErrorIs(GenerateTerrainPayloadManifest(request), TerrainPayloadManifestErrors::Invalid);
        request.verifiedFoliageDependencyClosure = {};
        ErrorIs(GenerateTerrainPayloadManifest(request), TerrainPayloadManifestErrors::Invalid);
        fixture.foliageDependencies[0] = Dependency(99);  // Self-consistent external closure still lacks required geometry.
        ErrorIs(GenerateTerrainPayloadManifest(fixture.Request()), TerrainPayloadManifestErrors::Invalid);
        fixture.foliageDependencies[0] = Dependency(9);
        fixture.foliage = Foliage(fixture.terrain.Tiles().tiles.front().id, 1, 2);
        ErrorIs(GenerateTerrainPayloadManifest(fixture.Request()), TerrainPayloadManifestErrors::Invalid);
    }

    TEST_CASE("Payload manifest rejects foreign cluster tile target and generation membership", "[terrain][payload-manifest]") {
        Fixture fixture;
        auto tile = fixture.terrain.Tiles().tiles.front().id;
        ++tile.tile.x;
        tile.tile.lod = 3;
        fixture.foliage = Foliage(tile);
        ErrorIs(GenerateTerrainPayloadManifest(fixture.Request()), TerrainPayloadManifestErrors::Invalid);
        fixture.foliage = Foliage(fixture.terrain.Tiles().tiles.front().id, 2);
        ErrorIs(GenerateTerrainPayloadManifest(fixture.Request()), TerrainPayloadManifestErrors::Invalid);
        auto profile = Profile();
        profile.tiles.targetDigest = Digest(88);
        auto changed = CookTerrainSourceArtifacts(fixture.source, profile, fixture.dependencies, {});
        REQUIRE(changed.HasValue());
        auto request = fixture.Request();
        request.terrain = &changed.Value();
        ErrorIs(GenerateTerrainPayloadManifest(request), TerrainPayloadManifestErrors::Invalid);
    }

    TEST_CASE("Payload manifest finite admission rejects work bytes counts and invalid limits before output",
              "[terrain][payload-manifest]") {
        Fixture fixture;
        for (const auto boundary : {0, 1, 2, 3, 4, 5}) {
            auto request = fixture.Request();
            if (boundary == 0)
                request.limits.maximumTiles = 1;
            if (boundary == 1)
                request.limits.maximumClusters = 1;
            if (boundary == 2)
                request.limits.maximumDependencies = 1;
            if (boundary == 3)
                request.limits.maximumInputBytes = 1;
            if (boundary == 4)
                request.limits.maximumOwnedBytes = 1;
            if (boundary == 5)
                request.limits.maximumWorkItems = 1;
            ErrorIs(GenerateTerrainPayloadManifest(request), TerrainPayloadManifestErrors::LimitExceeded);
        }
        auto request = fixture.Request();
        request.limits.maximumOwnedBytes = 0;
        ErrorIs(GenerateTerrainPayloadManifest(request), TerrainPayloadManifestErrors::Invalid);
        request = fixture.Request();
        request.limits.maximumWorkItems = std::numeric_limits<std::uint64_t>::max();
        ErrorIs(GenerateTerrainPayloadManifest(request), TerrainPayloadManifestErrors::Invalid);
        request = fixture.Request();
        request.terrainRequirements.visual = TerrainPayloadRequirement::Count;
        ErrorIs(GenerateTerrainPayloadManifest(request), TerrainPayloadManifestErrors::Invalid);
        request.terrain = nullptr;
        ErrorIs(GenerateTerrainPayloadManifest(request), TerrainPayloadManifestErrors::Invalid);
    }

    TEST_CASE("Payload manifest exact input and count boundaries retain complete membership", "[terrain][payload-manifest]") {
        Fixture fixture;
        auto request = fixture.Request();
        request.limits.maximumTiles = static_cast<std::uint32_t>(fixture.terrain.Tiles().tiles.size());
        request.limits.maximumClusters = static_cast<std::uint32_t>(fixture.foliage.Clusters().size());
        request.limits.maximumDependencies = 2;
        request.limits.maximumInputBytes = fixture.terrain.Manifest().size();
        for (const auto &tile : fixture.terrain.Tiles().tiles)
            request.limits.maximumInputBytes += tile.payload.size();
        for (const auto &artifact : fixture.terrain.Artifacts())
            request.limits.maximumInputBytes += artifact.payload.size();
        for (const auto &cluster : fixture.foliage.Clusters())
            request.limits.maximumInputBytes += cluster.payload.size();
        CHECK(GenerateTerrainPayloadManifest(request).HasValue());
        --request.limits.maximumInputBytes;
        ErrorIs(GenerateTerrainPayloadManifest(request), TerrainPayloadManifestErrors::LimitExceeded);
    }

    TEST_CASE("Payload manifest independently loaded bytes reject corruption truncation schema drift and cancellation",
              "[terrain][payload-manifest]") {
        Fixture fixture;
        const auto result = GenerateTerrainPayloadManifest(fixture.Request());
        REQUIRE(result.HasValue());
        const auto &manifest = result.Value();
        std::vector bytes(manifest.Bytes().begin(), manifest.Bytes().end());
        bytes.back() ^= 1;
        ErrorIs(VerifyTerrainPayloadManifest(manifest, bytes), TerrainPayloadManifestErrors::Invalid);
        bytes.assign(manifest.Bytes().begin(), manifest.Bytes().end());
        bytes[6] = 2;
        ErrorIs(VerifyTerrainPayloadManifest(manifest, bytes), TerrainPayloadManifestErrors::Invalid);
        ErrorIs(VerifyTerrainPayloadManifest(manifest, manifest.Bytes().first(5)), TerrainPayloadManifestErrors::Invalid);
        bytes.assign(manifest.Bytes().begin(), manifest.Bytes().end());
        bytes.push_back(0);
        ErrorIs(VerifyTerrainPayloadManifest(manifest, bytes), TerrainPayloadManifestErrors::Invalid);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        ErrorIs(GenerateTerrainPayloadManifest(fixture.Request(), cancellation.Token()), TerrainPayloadManifestErrors::Cancelled);
        ErrorIs(VerifyTerrainPayloadManifest(manifest, manifest.Bytes(), cancellation.Token()), TerrainPayloadManifestErrors::Cancelled);
    }

    TEST_CASE("Payload manifest replacement gates preserve detached prior roots and reject shutdown and ABA",
              "[terrain][payload-manifest]") {
        Fixture fixture;
        const auto first = GenerateTerrainPayloadManifest(fixture.TerrainOnly());
        REQUIRE(first.HasValue());
        const auto originalDigest = first.Value().Digest();
        auto request = fixture.TerrainOnly(2);
        request.previous = &first.Value();
        const auto next = GenerateTerrainPayloadManifest(request);
        REQUIRE(next.HasValue());
        CHECK(next.Value().Digest() != originalDigest);
        CHECK(ValidateTerrainPayloadManifestPublication(next.Value(), &first.Value(), originalDigest, TerrainRuntimeLifecycle::Active)
                  .HasValue());
        ErrorIs(ValidateTerrainPayloadManifestPublication(next.Value(), &first.Value(), Digest(99), TerrainRuntimeLifecycle::Active),
                TerrainPayloadManifestErrors::Stale);
        ErrorIs(ValidateTerrainPayloadManifestPublication(first.Value(), &first.Value(), originalDigest, TerrainRuntimeLifecycle::Active),
                TerrainPayloadManifestErrors::Stale);
        ErrorIs(ValidateTerrainPayloadManifestPublication(first.Value(), nullptr, originalDigest, TerrainRuntimeLifecycle::Active),
                TerrainPayloadManifestErrors::Stale);
        for (const auto lifecycle : {TerrainRuntimeLifecycle::Closing, TerrainRuntimeLifecycle::Closed})
            ErrorIs(ValidateTerrainPayloadManifestPublication(next.Value(), &first.Value(), originalDigest, lifecycle),
                    TerrainPayloadManifestErrors::Closed);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        ErrorIs(ValidateTerrainPayloadManifestPublication(next.Value(), &first.Value(), originalDigest, TerrainRuntimeLifecycle::Active,
                                                          cancellation.Token()),
                TerrainPayloadManifestErrors::Cancelled);
        request.content = Rev<TerrainContentRevision>(3);
        ErrorIs(GenerateTerrainPayloadManifest(request), TerrainPayloadManifestErrors::Stale);
        CHECK(first.Value().Digest() == originalDigest);
        CHECK(VerifyTerrainPayloadManifest(first.Value(), first.Value().Bytes()).HasValue());
    }

    TEST_CASE("Payload manifest moved-from inputs fail closed while retained roots outlive sources", "[terrain][payload-manifest]") {
        Fixture fixture;
        auto result = GenerateTerrainPayloadManifest(fixture.Request());
        REQUIRE(result.HasValue());
        auto moved = std::move(result).Value();
        ErrorIs(VerifyTerrainPayloadManifest(result.Value(), result.Value().Bytes()), TerrainPayloadManifestErrors::Invalid);
        ErrorIs(ValidateTerrainPayloadManifestPublication(result.Value(), nullptr, std::nullopt, TerrainRuntimeLifecycle::Active),
                TerrainPayloadManifestErrors::Invalid);
        auto stolenFoliage = std::move(fixture.foliage);
        ErrorIs(GenerateTerrainPayloadManifest(fixture.Request()), TerrainPayloadManifestErrors::Invalid);
        auto stolenTerrain = std::move(fixture.terrain);
        CHECK(GenerateTerrainPayloadManifest(fixture.TerrainOnly()).HasError());
        fixture.source.heightsMeters.clear();
        CHECK(VerifyTerrainPayloadManifest(moved, moved.Bytes()).HasValue());
        CHECK(stolenFoliage.Validate().HasValue());
        CHECK_FALSE(stolenTerrain.Tiles().tiles.empty());
    }

    TEST_CASE("Payload manifest empty foliage is explicit and distinct from an omitted contribution", "[terrain][payload-manifest]") {
        Fixture fixture;
        fixture.foliage = Foliage(fixture.terrain.Tiles().tiles.front().id, 1, 1, true);
        const auto empty = GenerateTerrainPayloadManifest(fixture.Request());
        const auto omitted = GenerateTerrainPayloadManifest(fixture.TerrainOnly());
        REQUIRE(empty.HasValue());
        REQUIRE(omitted.HasValue());
        CHECK(empty.Value().Clusters().empty());
        CHECK(empty.Value().Digest() != omitted.Value().Digest());
        auto invalid = fixture.Request();
        invalid.foliage = nullptr;
        ErrorIs(GenerateTerrainPayloadManifest(invalid), TerrainPayloadManifestErrors::Invalid);
    }

    TEST_CASE("Payload manifest flat terrain projected coordinates coarse error and holes retain bounds", "[terrain][payload-manifest]") {
        Fixture fixture;
        fixture.source.heightsMeters[6] = 100;
        fixture.source.holes.assign(25, 1);
        fixture.source.coordinates.space = TerrainCoordinateSpace::ProjectedMeters;
        fixture.source.coordinates.projectedCrs = "EPSG:32632";
        const auto terrain = CookTerrainSourceArtifacts(fixture.source, Profile(), fixture.dependencies, {});
        REQUIRE(terrain.HasValue());
        auto request = fixture.TerrainOnly();
        request.terrain = &terrain.Value();
        const auto manifest = GenerateTerrainPayloadManifest(request);
        REQUIRE(manifest.HasValue());
        CHECK(manifest.Value().Coordinates().projectedCrs == "EPSG:32632");
        CHECK(manifest.Value().Bounds().maximum[1] >= 100);
        CHECK(manifest.Value().Tiles().front().consumers[1]->workItems == 9);  // Zero triangles remains explicit hole geometry.
    }

    TEST_CASE("Payload manifest bulk work observes cancellation requested after work starts", "[terrain][payload-manifest]") {
        auto source = Source();
        source.width = source.height = 129;
        source.heightsMeters.assign(129 * 129, 1);
        auto profile = Profile();
        profile.tiles.interiorQuads = 128;
        profile.tiles.lodLevels = 1;
        const std::array dependencies{Dependency(10), Dependency(11)};
        const auto cooked = CookTerrainSourceArtifacts(source, profile, dependencies, {});
        REQUIRE(cooked.HasValue());
        TerrainPayloadManifestRequest request;
        request.terrain = &cooked.Value();
        request.content = Rev<TerrainContentRevision>(1);
        request.terrainDependencies = dependencies;
        // Real multi-megabyte records exercise hashing, vertex validation and detached assembly,
        // with no production checkpoint callback or injected verifier.
        for (const bool assembly : {false, true}) {
            CancellationSource cancellation;
            std::barrier started(2);
            std::jthread canceller([&] {
                started.arrive_and_wait();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                cancellation.RequestCancellation();
            });
            started.arrive_and_wait();
            CHECK_FALSE(cancellation.Token().IsCancellationRequested());
            if (assembly) {
                ErrorIs(GenerateTerrainPayloadManifest(request, cancellation.Token()), TerrainPayloadManifestErrors::Cancelled);
            } else {
                const auto &artifact = cooked.Value().Artifacts().front();
                const auto checked = VerifyTerrainSourceArtifactPayload(artifact.payload, artifact.digest, cancellation.Token());
                CHECK(checked.HasError());
                CHECK(cancellation.Token().IsCancellationRequested());
            }
        }
    }

    TEST_CASE("Payload manifest revision exhaustion and rollback cannot revalidate a prior publication", "[terrain][payload-manifest]") {
        Fixture fixture;
        const auto terminal = GenerateTerrainPayloadManifest(fixture.TerrainOnly(std::numeric_limits<std::uint64_t>::max()));
        const auto fresh = GenerateTerrainPayloadManifest(fixture.TerrainOnly());
        REQUIRE(terminal.HasValue());
        REQUIRE(fresh.HasValue());
        ErrorIs(ValidateTerrainPayloadManifestPublication(fresh.Value(), &terminal.Value(), terminal.Value().Digest(),
                                                          TerrainRuntimeLifecycle::Active),
                TerrainPayloadManifestErrors::Stale);
        auto request = fixture.TerrainOnly();
        request.previous = &terminal.Value();
        ErrorIs(GenerateTerrainPayloadManifest(request), TerrainPayloadManifestErrors::Stale);
    }
}  // namespace Horo::Terrain
