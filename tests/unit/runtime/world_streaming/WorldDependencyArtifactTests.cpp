#include "Horo/WorldStreaming/WorldDependencyArtifact.h"
#include "WorldArtifactCodecTestSupport.h"

namespace Horo::WorldStreaming {
    namespace {
        using ArtifactCodecTestSupport::Read;
        using ArtifactCodecTestSupport::Write;

        /** @brief Keeps the complete dependency wire and independent closure scratch under explicit small ceilings. */
        WorldDependencyArtifactLimits Limits() {
            return {4096, 65536, 8, 8, 32, 32, 16, 16, 32, 512};
        }

        /** @brief Captures stable page-scoped identity independently from serialized record offsets. */
        WorldDependencyEndpoint Endpoint(const std::uint64_t object) {
            return {{TestSupport::Asset(1), object}, TestSupport::IdentityFrom<WorldAuthoringRevision>(object + 9)};
        }

        /** @brief Rehashes the proposed sibling envelope so malformed-record tests reach structural validation. */
        void Seal(std::vector<std::byte> &bytes) {
            std::ranges::fill(std::span{bytes}.subspan(144, 32), std::byte{});
            const auto digest = ComputeSha256(bytes);
            for (std::size_t position = 0; position < 32; ++position)
                bytes[144 + position] = static_cast<std::byte>(digest.bytes[position]);
        }

        /** @brief Actual negative/cross-cell plans with single-owner, direct and explicit nonspatial objects. */
        struct Fixture final {
            ArtifactCodecTestSupport::Fixture world;
            DecodedWorldIndex index{Index(world)};
            WorldSpatialAssignment assignments{Assignments(index)};
            WorldSpanningObjectPlan placements{Placements(assignments)};
            WorldDependencyPlan dependencies{Dependencies(assignments)};

            static DecodedWorldIndex Index(const ArtifactCodecTestSupport::Fixture &world) {
                const auto encoded = world.Encode();
                auto decoded = DecodeWorldIndexArtifact(encoded.bytes, ArtifactCodecTestSupport::Limits());
                REQUIRE(decoded.HasValue());
                return std::move(decoded).Value();
            }

            static WorldSpatialAssignment Assignments(const DecodedWorldIndex &index) {
                const WorldPartitionBounds spanning{Math::WorldCoordinate64::FromMillimeters(-1000, 0, 0),
                                                    Math::WorldCoordinate64::FromMillimeters(999, 0, 0)};
                const WorldPartitionBounds local{Math::WorldCoordinate64::FromMillimeters(0, 0, 0),
                                                 Math::WorldCoordinate64::FromMillimeters(0, 0, 0)};
                const std::array candidates{WorldSpatialAssignmentCandidate{Endpoint(3).address, Endpoint(3).revision, spanning,
                                                                            TestSupport::Layer(), 0},
                                            WorldSpatialAssignmentCandidate{Endpoint(1).address, Endpoint(1).revision, spanning,
                                                                            TestSupport::Layer(), 0},
                                            WorldSpatialAssignmentCandidate{Endpoint(2).address, Endpoint(2).revision, local,
                                                                            TestSupport::Layer(), 0}};
                auto result = WorldSpatialAssignment::Create(index.descriptor, candidates, {8, 2, 8});
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            static WorldSpanningObjectPlan Placements(const WorldSpatialAssignment &assignments) {
                const std::array directives{WorldSpanningObjectDirective{Endpoint(3).address,
                                                                         Endpoint(3).revision,
                                                                         WorldSpanningObjectPolicy::NonSpatial,
                                                                         {}},
                                            WorldSpanningObjectDirective{Endpoint(1).address, Endpoint(1).revision,
                                                                         WorldSpanningObjectPolicy::SingleCellOwner,
                                                                         CandidateTestSupport::Cell(-1)}};
                auto result = WorldSpanningObjectPlan::Create(assignments, directives, {8, 1, 8});
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            static WorldDependencyPlan Dependencies(const WorldSpatialAssignment &assignments) {
                const std::array edges{WorldDependencyCandidate{Endpoint(3), Endpoint(1), WorldDependencyKind::Hard},
                                       WorldDependencyCandidate{Endpoint(1), Endpoint(2), WorldDependencyKind::Hard},
                                       WorldDependencyCandidate{Endpoint(2), Endpoint(9), WorldDependencyKind::Soft}};
                auto result = WorldDependencyPlan::Create(assignments, edges, {8, 8, 8, 8});
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            std::vector<std::byte> Encode() const {
                auto result = EncodeWorldDependencyArtifact(index, assignments, placements, dependencies, Limits());
                if (result.HasError())
                    INFO(result.ErrorValue().message);
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }
        };

        /** @brief Independent endpoint golden fields, without calling either codec's record writer. */
        void GoldenEndpoint(std::vector<std::byte> &bytes, const std::size_t offset, const std::uint64_t object) {
            const auto endpoint = Endpoint(object);
            for (std::size_t position = 0; position < 16; ++position)
                bytes[offset + position] = static_cast<std::byte>(endpoint.address.page.Bytes()[position]);
            Write(bytes, offset + 16, object, 8);
            Write(bytes, offset + 24, object + 9, 8);
        }

        /** @brief Independent negative/zero cell representation from the documented existing tuple plus padding. */
        void GoldenCell(std::vector<std::byte> &bytes, const std::size_t offset, const std::int32_t x) {
            Write(bytes, offset, static_cast<std::uint32_t>(x), 4);
            Write(bytes, offset + 13, TestSupport::Layer().Value(), 2);
        }

        /** @brief Complete golden envelope and table layout, independently specifying every nonzero field. */
        std::vector<std::byte> Golden(const Fixture &fixture) {
            std::vector<std::byte> bytes(744);
            constexpr std::string_view magic{"HORODEPS"};
            for (std::size_t position = 0; position < magic.size(); ++position)
                bytes[position] = static_cast<std::byte>(magic[position]);
            Write(bytes, 8, 1, 2);
            Write(bytes, 32, 1, 2);
            for (std::size_t position = 0; position < 16; ++position)
                bytes[16 + position] = static_cast<std::byte>(fixture.index.descriptor.Partition().Bytes()[position]);
            for (std::size_t position = 0; position < 32; ++position)
                bytes[40 + position] = static_cast<std::byte>(fixture.index.artifactHash.bytes[position]);
            const std::array<std::size_t, 8> counts{3, 5, 2, 1, 3, 1, 2, 2};
            const std::array<std::size_t, 8> offsets{192, 384, 464, 496, 504, 600, 664, 712};
            for (std::size_t table = 0; table < counts.size(); ++table) {
                Write(bytes, 72 + table * 4, counts[table], 4);
                Write(bytes, 104 + table * 4, offsets[table], 4);
            }
            Write(bytes, 136, bytes.size(), 8);
            for (std::size_t object = 0; object < 3; ++object) {
                const auto offset = 192 + object * 64;
                GoldenEndpoint(bytes, offset, object + 1);
                Write(bytes, offset + 32, object == 0 ? 1 : object == 1 ? 0 : 3, 4);
                Write(bytes, offset + 36, object == 0 ? 0 : object == 1 ? 2 : 3, 4);
                Write(bytes, offset + 40, object == 1 ? 1 : 2, 4);
                Write(bytes, offset + 44, object, 4);
                Write(bytes, offset + 48, object == 2 ? 0 : 1, 4);
            }
            for (std::size_t position = 0; position < 5; ++position)
                GoldenCell(bytes, 384 + position * 16, position == 0 || position == 3 ? -1 : 0);
            GoldenCell(bytes, 464, -1);
            GoldenCell(bytes, 480, 0);
            Write(bytes, 500, 3, 4);
            for (std::size_t member = 0; member < 3; ++member)
                GoldenEndpoint(bytes, 504 + member * 32, member + 1);
            GoldenEndpoint(bytes, 600, 2);
            GoldenEndpoint(bytes, 632, 9);
            GoldenCell(bytes, 664, -1);
            Write(bytes, 684, 1, 4);
            GoldenCell(bytes, 688, 0);
            Write(bytes, 704, 1, 4);
            Write(bytes, 708, 1, 4);
            GoldenCell(bytes, 712, 0);
            GoldenCell(bytes, 728, -1);
            Seal(bytes);
            return bytes;
        }
    }  // namespace

    TEST_CASE("World dependency sibling has complete independent golden bytes and preserves owner projections",
              "[unit][world_streaming][world_artifact]") {
        const Fixture fixture;
        const auto bytes = fixture.Encode();
        CHECK(bytes == Golden(fixture));
        auto decoded = DecodeWorldDependencyArtifact(bytes, fixture.index, Limits());
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().objects.size() == 3);
        CHECK(decoded.Value().objects[2].placement == WorldSpanningObjectPlacement::NonSpatial);
        CHECK(decoded.Value().objects[2].cells.empty());
        REQUIRE(decoded.Value().bundles.size() == 1);
        CHECK(decoded.Value().bundles[0] == std::vector{Endpoint(1), Endpoint(2), Endpoint(3)});
        REQUIRE(decoded.Value().softReferences.size() == 1);
        CHECK(decoded.Value().softReferences[0].target == Endpoint(9));
        REQUIRE(decoded.Value().cells.size() == 2);
        CHECK(decoded.Value().cells[0].hardTargets == std::vector{CandidateTestSupport::Cell()});
        CHECK(decoded.Value().cells[1].hardTargets == std::vector{CandidateTestSupport::Cell(-1)});
        CHECK(fixture.Encode() == bytes);
    }

    TEST_CASE("World dependency sibling rejects rehashed malformed tables and omitted hard targets",
              "[unit][world_streaming][world_artifact]") {
        const Fixture fixture;
        const auto original = fixture.Encode();
        for (const auto offset : {12U, 176U, 244U, 399U}) {
            auto bytes = original;
            bytes[offset] = std::byte{1};
            Seal(bytes);
            CHECK(DecodeWorldDependencyArtifact(bytes, fixture.index, Limits()).HasError());
        }
        for (const auto [offset, value] : std::array<std::pair<std::size_t, std::uint64_t>, 5>{
                 {{228, 0xffffffffU}, {224, 4}, {88, 0xffffffffU}, {552, 99}, {616, 99}}}) {
            auto bytes = original;
            Write(bytes, offset, value, 4);
            Seal(bytes);
            CHECK(DecodeWorldDependencyArtifact(bytes, fixture.index, Limits()).HasError());
        }
        auto missing = original;
        missing.resize(missing.size() - 16);
        Write(missing, 100, 1, 4);
        Write(missing, 136, missing.size(), 8);
        Write(missing, 708, 0, 4);
        Seal(missing);
        CHECK(DecodeWorldDependencyArtifact(missing, fixture.index, Limits()).HasError());
        CHECK(DecodeWorldDependencyArtifact(std::span{original}.first(191), fixture.index, Limits()).HasError());
    }

    TEST_CASE("World dependency sibling rejects stale index bytes and unsupported independent versions",
              "[unit][world_streaming][world_artifact]") {
        const Fixture fixture;
        const auto original = fixture.Encode();
        const auto changedIndexBytes = fixture.world.Encode("Changed world generation");
        auto changedIndex = DecodeWorldIndexArtifact(changedIndexBytes.bytes, ArtifactCodecTestSupport::Limits());
        REQUIRE(changedIndex.HasValue());
        CHECK(DecodeWorldDependencyArtifact(original, changedIndex.Value(), Limits()).HasError());
        for (const auto offset : {8U, 10U, 32U, 34U}) {
            auto bytes = original;
            Write(bytes, offset, 2, 2);
            Seal(bytes);
            CHECK(DecodeWorldDependencyArtifact(bytes, fixture.index, Limits()).HasError());
        }
    }

    TEST_CASE("World dependency sibling enforces exact envelope allocation and derivation boundaries",
              "[unit][world_streaming][world_artifact]") {
        const Fixture fixture;
        const auto bytes = fixture.Encode();
        auto limits = Limits();
        limits.maximumArtifactBytes = bytes.size();
        limits.maximumHardCellEdges = 2;
        CHECK(
            EncodeWorldDependencyArtifact(fixture.index, fixture.assignments, fixture.placements, fixture.dependencies, limits).HasValue());
        CHECK(DecodeWorldDependencyArtifact(bytes, fixture.index, limits).HasValue());
        --limits.maximumArtifactBytes;
        CHECK(DecodeWorldDependencyArtifact(bytes, fixture.index, limits).HasError());
        limits = Limits();
        limits.maximumHardCellEdges = 1;
        CHECK(
            EncodeWorldDependencyArtifact(fixture.index, fixture.assignments, fixture.placements, fixture.dependencies, limits).HasError());
        limits = Limits();
        limits.maximumDecodedAllocationBytes = 1;
        CHECK(DecodeWorldDependencyArtifact(bytes, fixture.index, limits).HasError());
        limits = Limits();
        limits.maximumDerivationSteps = 1;
        CHECK(DecodeWorldDependencyArtifact(bytes, fixture.index, limits).HasError());
    }

    TEST_CASE("World dependency sibling cancellation exposes no partial output", "[unit][world_streaming][world_artifact]") {
        const Fixture fixture;
        const auto bytes = fixture.Encode();
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        CHECK(EncodeWorldDependencyArtifact(fixture.index, fixture.assignments, fixture.placements, fixture.dependencies, Limits(),
                                            cancellation.Token())
                  .HasError());
        CHECK(DecodeWorldDependencyArtifact(bytes, fixture.index, Limits(), cancellation.Token()).HasError());
    }

    TEST_CASE("World dependency sibling rejects a stale sibling after actual cell content replacement",
              "[unit][world_streaming][world_artifact]") {
        const Fixture fixture;
        const auto sibling = fixture.Encode();
        auto cells = ArtifactCodecTestSupport::Fixture::Artifacts();
        const std::array core{std::byte{'9'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'},
                              std::byte{'6'}, std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
        const std::array terrain{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
        const std::array payloads{StreamingCellPayloadBytes{StreamingCellProvider::CoreEcs, StreamingCellPayloadRequirement::Required, 1,
                                                            core},
                                  StreamingCellPayloadBytes{StreamingCellProvider::Terrain, StreamingCellPayloadRequirement::Optional, 1,
                                                            terrain}};
        auto replaced = EncodeStreamingCellArtifact(CandidateTestSupport::Cell(-1), payloads, {2, 195, 12});
        REQUIRE(replaced.HasValue());
        cells[0] = std::move(replaced).Value();
        auto manifest = ArtifactCodecTestSupport::Fixture::Manifest(cells);
        auto encoded = EncodeWorldIndexArtifact(manifest, "World", {}, {}, ArtifactCodecTestSupport::Limits());
        REQUIRE(encoded.HasValue());
        auto changed = DecodeWorldIndexArtifact(encoded.Value().bytes, ArtifactCodecTestSupport::Limits());
        REQUIRE(changed.HasValue());
        CHECK(changed.Value().artifactHash != fixture.index.artifactHash);
        CHECK(DecodeWorldDependencyArtifact(sibling, changed.Value(), Limits()).HasError());
    }

    TEST_CASE("World dependency split placement retains every covered negative and cross cell", "[unit][world_streaming][world_artifact]") {
        const Fixture fixture;
        const std::array
            directives{WorldSpanningObjectDirective{Endpoint(1).address, Endpoint(1).revision, WorldSpanningObjectPolicy::SplitPerCell, {}},
                       WorldSpanningObjectDirective{Endpoint(3).address, Endpoint(3).revision, WorldSpanningObjectPolicy::NonSpatial, {}}};
        auto placements = WorldSpanningObjectPlan::Create(fixture.assignments, directives, {8, 1, 8});
        REQUIRE(placements.HasValue());
        auto encoded =
            EncodeWorldDependencyArtifact(fixture.index, fixture.assignments, placements.Value(), fixture.dependencies, Limits());
        REQUIRE(encoded.HasValue());
        auto decoded = DecodeWorldDependencyArtifact(encoded.Value(), fixture.index, Limits());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().objects.front().placement == WorldSpanningObjectPlacement::SplitPerCell);
        CHECK(decoded.Value().objects.front().cells == decoded.Value().objects.front().coverage);
        CHECK(decoded.Value().objects.front().cells == std::vector{CandidateTestSupport::Cell(-1), CandidateTestSupport::Cell()});
        CHECK(decoded.Value().cells.front().hardTargets == std::vector{CandidateTestSupport::Cell()});
    }
}  // namespace Horo::WorldStreaming
