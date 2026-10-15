#include "Horo/WorldStreaming/StreamingCellEncoding.h"
#include "Horo/WorldStreaming/StreamingCellInspection.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"
#include "Horo/WorldStreaming/WorldIndexArtifact.h"
#include "StreamingCellCandidateTestSupport.h"
#include "WorldArtifactCodecTestSupport.h"

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>

namespace Horo::WorldStreaming {
    using ArtifactCodecTestSupport::Fixture;
    using ArtifactCodecTestSupport::Limits;
    using ArtifactCodecTestSupport::Read;
    using ArtifactCodecTestSupport::Seal;
    using ArtifactCodecTestSupport::Write;
    using TestSupport::RequireError;

    TEST_CASE("World index canonical bytes preserve ADR023 offsets negative cells and exact integrity",
              "[unit][world_streaming][world_artifact]") {
        const Fixture fixture;
        const auto encoded = fixture.Encode();
        REQUIRE(encoded.bytes.size() == 425);
        CHECK(Read(encoded.bytes, 0, 4) == 0x58444e49);
        CHECK(Read(encoded.bytes, 12, 4) == 192);
        CHECK(Read(encoded.bytes, 108, 4) == 192);
        CHECK(Read(encoded.bytes, 116, 4) == 224);
        CHECK(Read(encoded.bytes, 124, 4) == 224);
        CHECK(Read(encoded.bytes, 132, 4) == 416);
        CHECK(Read(encoded.bytes, 224, 4) == 0xffffffffU);
        CHECK(Read(encoded.bytes, 320, 4) == 0);
        CHECK(Read(encoded.bytes, 224 + 32, 8) == 12);
        CHECK(Read(encoded.bytes, 224 + 40, 8) == 99);
        CHECK(Read(encoded.bytes, 224 + 48, 4) == 0xbdb0c0e4U);
        CHECK(encoded.artifactHash == ComputeSha256(encoded.bytes));
        auto decoded = DecodeWorldIndexArtifact(encoded.bytes, Limits());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().name == "World");
        CHECK(decoded.Value().descriptor.Cells().size() == 2);
        CHECK(decoded.Value().descriptor.Cells()[0].id.x == -1);
        CHECK(decoded.Value().descriptor.Bounds() == fixture.manifest.Descriptor().Bounds());
        CHECK(decoded.Value().cells[0].artifactHash == fixture.artifacts[0].integrity.artifactHash);
        CHECK(decoded.Value().artifactHash == encoded.artifactHash);
        CHECK(decoded.Value().cells[0].dependencyCount == 0);  // No fabricated hard edges: the matching sibling is required.
        CHECK(fixture.manifest.HardDependencies(0).size() == 1);
    }

    TEST_CASE("World index names volumes and clean repeated output remain canonical", "[unit][world_streaming][world_artifact]") {
        const Fixture fixture;
        const std::array volumes{WorldIndexVolume{.id = 2,
                                                  .kind = WorldIndexVolumeKind::Preload,
                                                  .shape = WorldIndexVolumeShape::Box,
                                                  .origin = {-1.0F, 2.0F, 3.0F},
                                                  .extents = {2.0F, 3.0F, 4.0F},
                                                  .layerMask = 1},
                                 WorldIndexVolume{.id = 1,
                                                  .kind = WorldIndexVolumeKind::Camera,
                                                  .shape = WorldIndexVolumeShape::Sphere,
                                                  .unloadOutside = true,
                                                  .extents = {1.0F, 0.0F, 0.0F},
                                                  .layerMask = 1}};
        const auto first = fixture.Encode("Cafe\xcc\x81", volumes);
        const std::array reversed{volumes[1], volumes[0]};
        const auto second = fixture.Encode("Caf\xc3\xa9", reversed);
        CHECK(first.bytes == second.bytes);
        auto decoded = DecodeWorldIndexArtifact(first.bytes, Limits());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().name == "Caf\xc3\xa9");
        REQUIRE(decoded.Value().volumes.size() == 2);
        CHECK(decoded.Value().volumes[0] == volumes[1]);
        CHECK(decoded.Value().volumes[1] == volumes[0]);
    }

    TEST_CASE("World index rejects corrupt malformed versioned and oversized input before publication",
              "[unit][world_streaming][world_artifact]") {
        const Fixture fixture;
        const auto good = fixture.Encode();
        auto bytes = good.bytes;
        SECTION("Truncated fixed header") {
            for (std::size_t length = 0; length < 192; ++length)
                RequireError(DecodeWorldIndexArtifact(std::span{bytes}.first(length), Limits()), WorldArtifactErrors::Invalid);
            return;
        }
        SECTION("Corruption") {
            bytes.back() ^= std::byte{1};
        }
        SECTION("Reserved fields") {
            bytes[184] = std::byte{1};
            Seal(bytes);
        }
        SECTION("Packed offsets") {
            Write(bytes, 124, 232, 4);
            Seal(bytes);
        }
        SECTION("Count overflow") {
            Write(bytes, 128, 0xffffffffU, 4);
            Seal(bytes);
        }
        SECTION("Duplicate cells") {
            Write(bytes, 320, 0xffffffffU, 4);
            Seal(bytes);
        }
        SECTION("Negative zero") {
            Write(bytes, 68, 0x80000000U, 4);
            Seal(bytes);
        }
        SECTION("NaN") {
            Write(bytes, 64, 0x7fc00000U, 4);
            Seal(bytes);
        }
        SECTION("Trailing bytes") {
            bytes.push_back(std::byte{0});
        }
        CHECK(DecodeWorldIndexArtifact(bytes, Limits()).HasError());
        REQUIRE(fixture.manifest.Cells().size() == 2);
        CHECK(fixture.manifest.HardDependencies(0).size() == 1);
    }

    TEST_CASE("World index mandatory boundaries unsupported versions and cancellation stay typed",
              "[unit][world_streaming][world_artifact]") {
        const Fixture fixture;
        auto encoded = fixture.Encode();
        auto limits = Limits();
        limits.maximumArtifactBytes = encoded.bytes.size();
        CHECK(DecodeWorldIndexArtifact(encoded.bytes, limits).HasValue());
        --limits.maximumArtifactBytes;
        RequireError(DecodeWorldIndexArtifact(encoded.bytes, limits), WorldArtifactErrors::CapacityExceeded);
        Write(encoded.bytes, 4, 2, 2);
        RequireError(DecodeWorldIndexArtifact(encoded.bytes, Limits()), WorldArtifactErrors::Unsupported);
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        RequireError(EncodeWorldIndexArtifact(fixture.manifest, "World", WorldIndexCoordinateSystem::RightHandedYUp, {}, Limits(),
                                              cancelled.Token()),
                     WorldArtifactErrors::Cancelled);
        RequireError(DecodeWorldIndexArtifact(encoded.bytes, Limits(), cancelled.Token()), WorldArtifactErrors::Cancelled);
    }

    TEST_CASE("Cell production matches independent CRC and fixed golden rows before existing parser admission",
              "[unit][world_streaming][world_artifact]") {
        const Fixture fixture;
        const auto &encoded = fixture.artifacts[1];
        REQUIRE(encoded.bytes.size() == 195);
        CHECK(Read(encoded.bytes, 36, 4) == 0xbdb0c0e4U);
        CHECK(Read(encoded.bytes, 96, 2) == 1);
        CHECK(Read(encoded.bytes, 104, 8) == 176);
        CHECK(Read(encoded.bytes, 128, 4) == 0xcbf43926U);
        CHECK(Read(encoded.bytes, 136, 2) == 2);
        CHECK(Read(encoded.bytes, 144, 8) == 192);
        CHECK(Read(encoded.bytes, 168, 4) == 0x352441c2U);
        auto context = CandidateTestSupport::Context();
        context.maximumPayloads = 2;
        context.maximumCompressedBytes = 99;
        context.maximumUncompressedBytes = 12;
        CHECK(ParseStreamingCellArtifact(fixture.manifest, context, encoded.bytes).HasValue());
    }

    TEST_CASE("Inert cell inspection shares runtime validation and owns its moved header table",
              "[unit][world_streaming][world_artifact]") {
        const Fixture fixture;
        auto bytes = fixture.artifacts[0].bytes;
        auto inspected = InspectStreamingCellArtifact(bytes, fixture.artifacts[0].integrity.artifactHash, {2, 99, 12});
        REQUIRE(inspected.HasValue());
        auto owned = std::move(inspected).Value();
        auto copied = owned;
        auto moved = std::move(owned);
        CHECK(copied.Header().cell == CandidateTestSupport::Cell(-1));
        CHECK(moved.Header().payloads.size() == 2);
        CHECK(copied.Header().payloads.data() != moved.Header().payloads.data());
        bytes.back() ^= std::byte{1};
        CHECK(InspectStreamingCellArtifact(bytes, fixture.artifacts[0].integrity.artifactHash, {2, 99, 12}).HasError());
        CHECK(
            InspectStreamingCellArtifact(fixture.artifacts[0].bytes, fixture.artifacts[0].integrity.artifactHash, {1, 99, 12}).HasError());
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        CHECK(InspectStreamingCellArtifact(fixture.artifacts[0].bytes, fixture.artifacts[0].integrity.artifactHash, {2, 99, 12},
                                           cancellation.Token())
                  .HasError());
    }
}  // namespace Horo::WorldStreaming
