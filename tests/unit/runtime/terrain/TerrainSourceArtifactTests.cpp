#include "../../../support/TypedIdentityTestSupport.h"
#include "Horo/Terrain/TerrainSourceArtifacts.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string_view>

namespace Horo::Terrain {
    namespace {
        TerrainCanonicalSource Source(const std::uint32_t width = 5, const std::uint32_t height = 5) {
            SerializedTerrainIdentity bytes{};
            bytes[0] = 17;
            const auto project = TerrainProjectId::Create(bytes).Value();
            constexpr std::string_view key = "terrain/source-artifacts";
            TerrainCanonicalSource source;
            source.dataset = DeriveTerrainDatasetId(project, std::as_bytes(std::span{key.data(), key.size()})).Value();
            std::array<std::uint8_t, 16> asset{};
            asset[0] = 9;
            source.sourceAsset = Assets::AssetId::FromBytes(asset);
            source.revision = TerrainSourceRevision::Create(1).Value();
            source.capability = TerrainCapabilityRevision::Create(3).Value();
            source.width = width;
            source.height = height;
            source.coordinates.originX = -10;
            source.coordinates.originZ = 20;
            source.coordinates.spacingX = 0.5;
            source.coordinates.spacingZ = 2;
            for (std::uint32_t z = 0; z < height; ++z)
                for (std::uint32_t x = 0; x < width; ++x)
                    source.heightsMeters.push_back(static_cast<float>(x + 2 * z));
            return source;
        }

        TerrainSourceArtifactProfile Profile() {
            TerrainSourceArtifactProfile profile;
            profile.tiles.interiorQuads = 2;
            profile.tiles.lodLevels = 2;
            profile.tiles.targetDigest.bytes[0] = 1;
            profile.tiles.toolchainDigest.bytes[0] = 2;
            return profile;
        }

        template <typename T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &code) {
            Tests::RequireFailureIdentity(result, code);
        }

        const TerrainSourceArtifact &Find(const CookedTerrainSourceArtifacts &cooked, const TerrainSourceArtifactRole role,
                                          const std::uint8_t lod) {
            const auto found = std::ranges::find_if(cooked.Artifacts(), [role, lod](const auto &artifact) {
                return artifact.role == role && artifact.tile.tile.lod == lod;
            });
            REQUIRE(found != cooked.Artifacts().end());
            return *found;
        }
    }  // namespace

    TEST_CASE("Offline terrain emits actual upward wound geometry for every requested role", "[terrain][source-artifacts]") {
        const auto source = Source();
        const auto cooked = CookTerrainSourceArtifacts(source, Profile(), {}, {});
        REQUIRE(cooked.HasValue());
        const auto &candidate = cooked.Value();
        CHECK(candidate.Artifacts().size() == 13);  // Five visual LOD tiles and four tiles for each consumer.
        CHECK(candidate.Capability() == source.capability);
        CHECK(VerifyCookedTerrainTiles(candidate.Tiles()).HasValue());
        CHECK(candidate.ValidateCurrent(source).HasValue());
        for (const auto &artifact : candidate.Artifacts()) {
            REQUIRE(artifact.vertices.size() == 9);
            REQUIRE(artifact.triangles.size() == 8);
            CHECK(artifact.requiresSameLodNeighbors);
            CHECK(artifact.digest == ComputeSha256(std::as_bytes(std::span{artifact.payload})));
            CHECK(VerifyTerrainSourceArtifactPayload(artifact.payload, artifact.digest).HasValue());
            CHECK(artifact.payload.size() > artifact.vertices.size() * 24);
            for (const auto &triangle : artifact.triangles) {
                const auto &a = artifact.vertices[triangle.indices[0]];
                const auto &b = artifact.vertices[triangle.indices[1]];
                const auto &c = artifact.vertices[triangle.indices[2]];
                CHECK((b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z) > 0);
            }
        }
        const auto &visual = Find(candidate, TerrainSourceArtifactRole::Visual, 0);
        CHECK(visual.maximumGeometricError == 0);
        CHECK((visual.vertices.front() == TerrainSourceVertex{-10, 0, 20}));
        CHECK((visual.vertices.back() == TerrainSourceVertex{-9, 6, 24}));
        CHECK(visual.vertices == Find(candidate, TerrainSourceArtifactRole::Collision, 0).vertices);
        CHECK(visual.triangles == Find(candidate, TerrainSourceArtifactRole::Navigation, 0).triangles);
        CHECK(candidate.ManifestDigest() == ComputeSha256(std::as_bytes(candidate.Manifest())));
    }

    TEST_CASE("Unsampled holes exclude complete coarse collision navigation and visual quads", "[terrain][source-artifacts]") {
        auto source = Source();
        source.holes.resize(25);
        source.holes[6] = 1;  // (1,1) is not a vertex in LOD 1.
        auto profile = Profile();
        profile.collisionLod = 1;
        profile.navigationLod = 1;
        const auto cooked = CookTerrainSourceArtifacts(source, profile, {}, {});
        REQUIRE(cooked.HasValue());
        for (const auto role :
             {TerrainSourceArtifactRole::Visual, TerrainSourceArtifactRole::Collision, TerrainSourceArtifactRole::Navigation}) {
            const auto &artifact = Find(cooked.Value(), role, 1);
            CHECK(artifact.vertices.size() == 9);
            CHECK(artifact.triangles.size() == 6);
            for (const auto &triangle : artifact.triangles)
                CHECK_FALSE((triangle.beginX <= 1 && triangle.endX >= 1 && triangle.beginZ <= 1 && triangle.endZ >= 1));
        }
    }

    TEST_CASE("Partial tiles preserve exact edge positions and adjacent source seam evidence", "[terrain][source-artifacts]") {
        const auto cooked = CookTerrainSourceArtifacts(Source(6, 4), Profile(), {}, {});
        REQUIRE(cooked.HasValue());
        const auto artifacts = cooked.Value().Artifacts();
        CHECK(artifacts[0].seams[1] == artifacts[3].seams[0]);
        const auto last = std::ranges::find_if(artifacts, [](const auto &artifact) {
            return artifact.role == TerrainSourceArtifactRole::Visual && artifact.tile.tile.lod == 1;
        });
        REQUIRE(last != artifacts.end());
        CHECK(last->vertices.back().z == 26);
        CHECK(std::ranges::any_of(artifacts, [](const auto &artifact) {
            return artifact.vertices.back().x == -7.5;
        }));
    }

    TEST_CASE("Coarse terrain geometry retains a conservative error bound for omitted heights", "[terrain][source-artifacts]") {
        auto source = Source();
        source.heightsMeters[6] = 100;
        const auto cooked = CookTerrainSourceArtifacts(source, Profile(), {}, {});
        REQUIRE(cooked.HasValue());
        const auto &coarse = Find(cooked.Value(), TerrainSourceArtifactRole::Visual, 1);
        CHECK(coarse.maximumGeometricError >= 100);
        CHECK(std::ranges::none_of(coarse.vertices, [](const auto &vertex) {
            return vertex.y == 100;
        }));
        source.heightsMeters.assign(25, 7);
        const auto flat = CookTerrainSourceArtifacts(source, Profile(), {}, {});
        REQUIRE(flat.HasValue());
        CHECK(Find(flat.Value(), TerrainSourceArtifactRole::Visual, 1).maximumGeometricError == 0);
    }

    TEST_CASE("Source artifact bytes and manifests close over dependency order policy and capability", "[terrain][source-artifacts]") {
        const auto source = Source();
        std::array<TerrainTileCookDependency, 2> dependencies{};
        for (std::uint8_t index = 0; index < 2; ++index) {
            std::array<std::uint8_t, 16> asset{};
            asset[0] = index + 1;
            dependencies[index].asset = Assets::AssetId::FromBytes(asset);
            dependencies[index].artifactDigest.bytes[0] = index + 1;
        }
        const auto first = CookTerrainSourceArtifacts(source, Profile(), dependencies, {});
        REQUIRE(first.HasValue());
        std::ranges::reverse(dependencies);
        const auto reordered = CookTerrainSourceArtifacts(source, Profile(), dependencies, {});
        REQUIRE(reordered.HasValue());
        CHECK(first.Value().ManifestDigest() == reordered.Value().ManifestDigest());
        for (std::size_t index = 0; index < first.Value().Artifacts().size(); ++index)
            CHECK(first.Value().Artifacts()[index].payload == reordered.Value().Artifacts()[index].payload);
        auto profile = Profile();
        profile.navigationLod = 1;
        const auto changed = CookTerrainSourceArtifacts(source, profile, dependencies, {});
        REQUIRE(changed.HasValue());
        CHECK(changed.Value().Fingerprint() != first.Value().Fingerprint());
        auto newer = source;
        newer.capability = TerrainCapabilityRevision::Create(4).Value();
        ErrorIs(first.Value().ValidateCurrent(newer), TerrainSourceErrors::RevisionStale);
    }

    TEST_CASE("Replacement cancellation and source retirement cannot mutate prior artifact ownership", "[terrain][source-artifacts]") {
        auto source = Source();
        const auto prior = CookTerrainSourceArtifacts(source, Profile(), {}, {});
        REQUIRE(prior.HasValue());
        const auto digest = prior.Value().ManifestDigest();
        source.revision = TerrainSourceRevision::Create(2).Value();
        source.heightsMeters[0] = -40;
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        ErrorIs(CookTerrainSourceArtifacts(source, Profile(), {}, cancellation.Token()), TerrainTileCookErrors::Cancelled);
        const auto replacement = CookTerrainSourceArtifacts(source, Profile(), {}, {});
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value().ManifestDigest() != digest);
        CHECK(prior.Value().ManifestDigest() == digest);
        ErrorIs(prior.Value().ValidateCurrent(source), TerrainSourceErrors::RevisionStale);
        source.heightsMeters.clear();
        CHECK(prior.Value().Artifacts().front().vertices.front().y == 0);
        CHECK(replacement.Value().Artifacts().front().vertices.front().y == -40);
    }

    TEST_CASE("All geometry roles charge vertex triangle byte and work ceilings without partial output", "[terrain][source-artifacts]") {
        const auto source = Source();
        for (const auto ceiling : {0, 1, 2, 3}) {
            auto profile = Profile();
            if (ceiling == 0)
                profile.maximumVertices = 1;
            if (ceiling == 1)
                profile.maximumTriangles = 1;
            if (ceiling == 2)
                profile.maximumOwnedBytes = 1;
            if (ceiling == 3)
                profile.maximumWorkItems = 1;
            ErrorIs(CookTerrainSourceArtifacts(source, profile, {}, {}), TerrainTileCookErrors::LimitExceeded);
        }
        auto profile = Profile();
        profile.maximumWorkItems = TerrainDescriptorHardLimits::WorkItems + 1;
        ErrorIs(CookTerrainSourceArtifacts(source, profile, {}, {}), TerrainTileCookErrors::InvalidProfile);
        profile = Profile();
        profile.navigationLod = 2;
        ErrorIs(CookTerrainSourceArtifacts(source, profile, {}, {}), TerrainTileCookErrors::InvalidProfile);
    }

    TEST_CASE("Invalid canonical channels coordinates and freshness are rejected before geometry reads", "[terrain][source-artifacts]") {
        auto source = Source();
        source.heightsMeters.pop_back();
        ErrorIs(CookTerrainSourceArtifacts(source, Profile(), {}, {}), TerrainTileCookErrors::InvalidSource);
        source = Source();
        source.heightsMeters[0] = std::numeric_limits<float>::infinity();
        ErrorIs(CookTerrainSourceArtifacts(source, Profile(), {}, {}), TerrainTileCookErrors::InvalidSource);
        source = Source();
        const auto cooked = CookTerrainSourceArtifacts(source, Profile(), {}, {});
        REQUIRE(cooked.HasValue());
        source.heightsMeters[0] = 4;  // Same revision but different source bytes must still fail.
        ErrorIs(cooked.Value().ValidateCurrent(source), TerrainSourceErrors::RevisionStale);
        source.heightsMeters.clear();
        ErrorIs(cooked.Value().ValidateCurrent(source), TerrainTileCookErrors::InvalidSource);
        source = Source();
        source.coordinates.space = TerrainCoordinateSpace::GeographicDegrees;
        ErrorIs(CookTerrainSourceArtifacts(source, Profile(), {}, {}), TerrainTileCookErrors::InvalidSource);
    }

    TEST_CASE("Canonical meter heights are not rescaled and all hole sources remain explicit", "[terrain][source-artifacts]") {
        auto source = Source();
        source.coordinates.heightScale = 100;
        source.coordinates.heightOffset = 1000;
        source.holes.assign(25, 1);
        const auto cooked = CookTerrainSourceArtifacts(source, Profile(), {}, {});
        REQUIRE(cooked.HasValue());
        for (const auto &artifact : cooked.Value().Artifacts()) {
            CHECK(artifact.triangles.empty());
            CHECK(artifact.vertices.front().y < 100);
            CHECK_FALSE(artifact.payload.empty());
        }
    }

    TEST_CASE("Exact aggregate byte admission includes retained and temporary geometry allocations", "[terrain][source-artifacts]") {
        const auto source = Source();
        auto profile = Profile();
        std::uint64_t low = 1;
        std::uint64_t high = 1'048'576;
        profile.maximumOwnedBytes = high;
        REQUIRE(CookTerrainSourceArtifacts(source, profile, {}, {}).HasValue());
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            profile.maximumOwnedBytes = middle;
            const auto candidate = CookTerrainSourceArtifacts(source, profile, {}, {});
            if (candidate.HasValue())
                high = middle;
            else {
                ErrorIs(candidate, TerrainTileCookErrors::LimitExceeded);
                low = middle + 1;
            }
        }
        profile.maximumOwnedBytes = low;
        const auto exact = CookTerrainSourceArtifacts(source, profile, {}, {});
        REQUIRE(exact.HasValue());
        std::uint64_t payloadBytes{};
        for (const auto &tile : exact.Value().Tiles().tiles)
            payloadBytes += tile.payload.capacity();
        for (const auto &artifact : exact.Value().Artifacts())
            payloadBytes += artifact.payload.capacity();
        CHECK(low > payloadBytes);  // Geometry arrays, object vectors, CRS, manifest and scratch cannot be ignored.
        profile.maximumOwnedBytes = low - 1;
        ErrorIs(CookTerrainSourceArtifacts(source, profile, {}, {}), TerrainTileCookErrors::LimitExceeded);
    }

    TEST_CASE("Loaded neutral artifacts reject truncation tampering and malformed topology even with a matching byte hash",
              "[terrain][source-artifacts]") {
        const auto cooked = CookTerrainSourceArtifacts(Source(), Profile(), {}, {});
        REQUIRE(cooked.HasValue());
        const auto &artifact = cooked.Value().Artifacts().front();
        auto bytes = artifact.payload;
        bytes.back() ^= 1;
        ErrorIs(VerifyTerrainSourceArtifactPayload(bytes, artifact.digest), TerrainTileCookErrors::CorruptPrevious);
        bytes = artifact.payload;
        bytes.resize(407);
        ErrorIs(VerifyTerrainSourceArtifactPayload(bytes, ComputeSha256(std::as_bytes(std::span{bytes}))),
                TerrainTileCookErrors::CorruptPrevious);
        bytes = artifact.payload;
        bytes[6] = 2;
        ErrorIs(VerifyTerrainSourceArtifactPayload(bytes, ComputeSha256(std::as_bytes(std::span{bytes}))),
                TerrainTileCookErrors::CorruptPrevious);
        bytes = artifact.payload;
        const auto firstTriangle = 408 + artifact.vertices.size() * 24;  // Fixture uses an empty local-meter CRS.
        for (std::size_t byte = 0; byte < 4; ++byte)
            bytes[firstTriangle + byte] = 255;
        ErrorIs(VerifyTerrainSourceArtifactPayload(bytes, ComputeSha256(std::as_bytes(std::span{bytes}))),
                TerrainTileCookErrors::CorruptPrevious);
    }

    TEST_CASE("Projected precision and nonfinite meter geometry cannot enter source artifacts", "[terrain][source-artifacts]") {
        auto source = Source();
        source.coordinates.space = TerrainCoordinateSpace::ProjectedMeters;
        source.coordinates.projectedCrs = "EPSG:32632";
        const auto projected = CookTerrainSourceArtifacts(source, Profile(), {}, {});
        REQUIRE(projected.HasValue());
        for (const auto &artifact : projected.Value().Artifacts())
            CHECK(VerifyTerrainSourceArtifactPayload(artifact.payload, artifact.digest).HasValue());
        // A matching hash does not authorize an invalid coordinate policy or CRS identifier.
        const auto &payload = projected.Value().Artifacts().front().payload;
        constexpr std::array<std::size_t, 3> policyOffsets{196, 199, 204};
        for (const auto offset : policyOffsets) {
            auto invalid = payload;
            invalid[offset] = 255;
            ErrorIs(VerifyTerrainSourceArtifactPayload(invalid, ComputeSha256(std::as_bytes(std::span{invalid}))),
                    TerrainTileCookErrors::CorruptPrevious);
        }
        auto invalidLocal = payload;
        invalidLocal[196] = static_cast<std::uint8_t>(TerrainCoordinateSpace::LocalMeters);
        ErrorIs(VerifyTerrainSourceArtifactPayload(invalidLocal, ComputeSha256(std::as_bytes(std::span{invalidLocal}))),
                TerrainTileCookErrors::CorruptPrevious);
        source.coordinates.originX = 1e18;
        source.coordinates.spacingX = 1e9;  // Valid signed tile addressing and distinguishable double coordinates.
        const auto large = CookTerrainSourceArtifacts(source, Profile(), {}, {});
        REQUIRE(large.HasValue());
        CHECK(large.Value().Artifacts().front().vertices[1].x > large.Value().Artifacts().front().vertices[0].x);
        source.coordinates.spacingX = 1e300;
        source.coordinates.spacingZ = 1e300;
        source.coordinates.originX = 0;
        source.coordinates.originZ = 0;
        ErrorIs(CookTerrainSourceArtifacts(source, Profile(), {}, {}), TerrainTileCookErrors::CorruptPrevious);
    }
}  // namespace Horo::Terrain
