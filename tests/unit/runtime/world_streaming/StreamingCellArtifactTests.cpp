#include "Horo/WorldStreaming/StreamingCellCandidate.h"
#include "StreamingCellCandidateTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string_view>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::RequireError;

        void Write(std::vector<std::byte> &bytes, const std::size_t offset, std::uint64_t value, const std::size_t width) {
            for (std::size_t index = 0; index < width; ++index, value >>= 8U)
                bytes[offset + index] = static_cast<std::byte>(value & 255U);
        }

        Sha256Digest Seal(std::vector<std::byte> &bytes) {
            std::ranges::fill(std::span{bytes}.subspan(64, 32), std::byte{0});
            const auto hash = ComputeSha256(bytes);
            for (std::size_t index = 0; index < hash.bytes.size(); ++index)
                bytes[64 + index] = static_cast<std::byte>(hash.bytes[index]);
            return hash;
        }

        std::vector<std::byte> Artifact() {
            // Independent golden fields: two uncompressed rows; known IEEE CRCs for text blocks.
            std::vector<std::byte> bytes(195);
            constexpr std::string_view magic = "HOROCELL";
            for (std::size_t index = 0; index < magic.size(); ++index)
                bytes[index] = static_cast<std::byte>(magic[index]);
            Write(bytes, 8, 1, 2);
            Write(bytes, 30, 2, 2);
            Write(bytes, 36, 0xbdb0c0e4, 4);
            Write(bytes, 40, 12, 8);
            Write(bytes, 48, 99, 8);
            Write(bytes, 56, 96, 4);
            Write(bytes, 60, 2, 4);
            Write(bytes, 96, 1, 2);
            Write(bytes, 98, 1, 2);
            Write(bytes, 100, 1, 4);
            Write(bytes, 104, 176, 8);
            Write(bytes, 112, 9, 8);
            Write(bytes, 120, 9, 8);
            Write(bytes, 128, 0xcbf43926, 4);
            Write(bytes, 136, 2, 2);
            Write(bytes, 138, 2, 2);
            Write(bytes, 140, 1, 4);
            Write(bytes, 144, 192, 8);
            Write(bytes, 152, 3, 8);
            Write(bytes, 160, 3, 8);
            Write(bytes, 168, 0x352441c2, 4);
            constexpr std::string_view first = "123456789";
            for (std::size_t index = 0; index < first.size(); ++index)
                bytes[176 + index] = static_cast<std::byte>(first[index]);
            bytes[192] = std::byte{'a'};
            bytes[193] = std::byte{'b'};
            bytes[194] = std::byte{'c'};
            Seal(bytes);
            return bytes;
        }

        CookedWorldIndexManifest ManifestFor(const std::span<const std::byte> bytes) {
            const auto grid = WorldCellQuantizationPolicy::Create({}, 100, {-1, 1, -1, 1, -1, 1}, 1).Value();
            const std::array layers{
                WorldLayerDescriptor{TestSupport::Layer(), "base", WorldLayerOwnership::WorldStreaming, WorldLayerFlags::None, 1.0F}};
            const std::array cells{WorldPartitionCellDescriptor{CandidateTestSupport::Cell(), {TestSupport::Asset(4)}}};
            auto descriptor = WorldPartitionDescriptor::Create({}, TestSupport::World(),
                                                               {Math::WorldCoordinate64::FromMillimeters(0, 0, 0),
                                                                Math::WorldCoordinate64::FromMillimeters(99, 99, 99)},
                                                               grid, layers, cells, {1, 1, 4})
                                  .Value();
            Sha256Digest hash;
            for (std::size_t index = 0; index < hash.bytes.size(); ++index)
                hash.bytes[index] = std::to_integer<std::uint8_t>(bytes[64 + index]);
            const std::array metadata{CookedWorldCellManifestCandidate{CandidateTestSupport::Cell(), 12, 99, 0xbdb0c0e4, hash, {}}};
            return std::move(CookedWorldIndexManifest::Create(std::move(descriptor), metadata, {1, 1, 1, 99, 12})).Value();
        }

        void RequireRejected(std::vector<std::byte> bytes, const ErrorCodeDescriptor &error) {
            Seal(bytes);
            const auto manifest = ManifestFor(bytes);
            RequireError(ParseStreamingCellArtifact(manifest, CandidateTestSupport::Context(), bytes), error);
            REQUIRE(manifest.Cells().size() == 1);
            REQUIRE(manifest.Cells()[0].compressedSize == 99);
        }
    }  // namespace

    TEST_CASE("Canonical cell bytes produce an owned generation-fenced candidate", "[unit][world_streaming][cell_artifact]") {
        auto bytes = Artifact();
        const auto manifest = ManifestFor(bytes);
        auto context = CandidateTestSupport::Context();
        context.maximumPayloads = 2;
        context.maximumCompressedBytes = 99;
        context.maximumUncompressedBytes = 12;
        auto result = ParseStreamingCellArtifact(manifest, context, bytes);
        REQUIRE(result.HasValue());
        auto candidate = std::move(result).Value();
        bytes.clear();
        REQUIRE(candidate.Payloads().size() == 2);
        REQUIRE(candidate.Payloads()[0].offset == 176);
        REQUIRE(candidate.Payloads()[1].payloadCrc32 == 0x352441c2);
        REQUIRE(candidate.Operation() == context.operation);
        REQUIRE(candidate.ChunkAsset() == TestSupport::Asset(4));
    }

    TEST_CASE("Cell parsing rejects truncation corrupted bytes and stale manifest hashes", "[unit][world_streaming][cell_artifact]") {
        const auto bytes = Artifact();
        const auto manifest = ManifestFor(bytes);
        for (std::size_t length = 0; length < 96; ++length)
            RequireError(ParseStreamingCellArtifact(manifest, CandidateTestSupport::Context(), std::span{bytes}.first(length)),
                         WorldStreamingErrors::CellCandidateInvalid);
        auto corrupt = bytes;
        corrupt.back() ^= std::byte{1};
        RequireError(ParseStreamingCellArtifact(manifest, CandidateTestSupport::Context(), corrupt),
                     WorldStreamingErrors::CellCandidateInvalid);
        Seal(corrupt);
        RequireError(ParseStreamingCellArtifact(manifest, CandidateTestSupport::Context(), corrupt),
                     WorldStreamingErrors::CellCandidateStale);
    }

    TEST_CASE("Cell parser bounds byte counts and hostile TOC counts before allocation", "[unit][world_streaming][cell_artifact]") {
        const auto bytes = Artifact();
        const auto manifest = ManifestFor(bytes);
        for (unsigned limit = 0; limit < 3; ++limit) {
            auto context = CandidateTestSupport::Context();
            if (limit == 0)
                context.maximumPayloads = 1;
            if (limit == 1)
                context.maximumCompressedBytes = 98;
            if (limit == 2)
                context.maximumUncompressedBytes = 11;
            RequireError(ParseStreamingCellArtifact(manifest, context, bytes), WorldStreamingErrors::CellCandidateCapacityExceeded);
        }
        auto hostile = bytes;
        Write(hostile, 60, std::numeric_limits<std::uint32_t>::max(), 4);
        RequireRejected(hostile, WorldStreamingErrors::CellCandidateCapacityExceeded);
        Write(hostile, 60, 3, 4);
        RequireRejected(hostile, WorldStreamingErrors::CellCandidateInvalid);
    }

    TEST_CASE("Cell parsing validates authenticated canonical controls ranges and CRC", "[unit][world_streaming][cell_artifact]") {
        for (const auto offset : {0U, 56U, 98U, 100U, 128U, 132U, 172U, 185U}) {
            auto malformed = Artifact();
            malformed[offset] ^= std::byte{1};
            RequireRejected(malformed, WorldStreamingErrors::CellCandidateInvalid);
        }
        auto duplicate = Artifact();
        Write(duplicate, 136, 1, 2);
        RequireRejected(duplicate, WorldStreamingErrors::CellCandidateInvalid);
        auto overflow = Artifact();
        Write(overflow, 104, std::numeric_limits<std::uint64_t>::max(), 8);
        RequireRejected(overflow, WorldStreamingErrors::CellCandidateInvalid);
        auto trailing = Artifact();
        trailing.push_back(std::byte{0});
        RequireRejected(trailing, WorldStreamingErrors::CellCandidateInvalid);
        auto mismatch = Artifact();
        Write(mismatch, 12, 2, 4);
        RequireRejected(mismatch, WorldStreamingErrors::CellCandidateInvalid);
        auto decodedOverflow = Artifact();
        Write(decodedOverflow, 120, std::numeric_limits<std::uint64_t>::max(), 8);
        RequireRejected(decodedOverflow, WorldStreamingErrors::CellCandidateInvalid);
        auto staleCell = Artifact();
        Write(staleCell, 16, 1, 4);
        RequireRejected(staleCell, WorldStreamingErrors::CellCandidateStale);
    }

    TEST_CASE("Cell parsing preserves major rejection and optional minor compatibility", "[unit][world_streaming][cell_artifact]") {
        auto bytes = Artifact();
        Write(bytes, 8, 2, 2);
        RequireRejected(bytes, WorldStreamingErrors::CellCandidateUnsupported);
        bytes = Artifact();
        Write(bytes, 10, 1, 2);
        Write(bytes, 136, 0x8000, 2);
        Seal(bytes);
        auto manifest = ManifestFor(bytes);
        REQUIRE(ParseStreamingCellArtifact(manifest, CandidateTestSupport::Context(), bytes).HasValue());
        Write(bytes, 136, 8, 2);
        Seal(bytes);
        const auto unknownManifest = ManifestFor(bytes);
        REQUIRE(ParseStreamingCellArtifact(unknownManifest, CandidateTestSupport::Context(), bytes).HasValue());
        Write(bytes, 138, 1, 2);
        RequireRejected(bytes, WorldStreamingErrors::CellCandidateUnsupported);
        bytes = Artifact();
        Write(bytes, 12, 1, 4);
        RequireRejected(bytes, WorldStreamingErrors::CellCandidateUnsupported);
        bytes = Artifact();
        Write(bytes, 32, 0x100, 4);
        RequireRejected(bytes, WorldStreamingErrors::CellCandidateUnsupported);
    }

    TEST_CASE("Cell parsing closes cancelled shutdown and stale owner admission without mutation",
              "[unit][world_streaming][cell_artifact][lifecycle]") {
        const auto bytes = Artifact();
        const auto manifest = ManifestFor(bytes);
        CancellationSource source;
        source.RequestCancellation();
        RequireError(ParseStreamingCellArtifact(manifest, CandidateTestSupport::Context(), bytes, source.Token()),
                     WorldStreamingErrors::CellCandidateLifecycleUnavailable);
        for (const auto lifecycle : {StreamingCellCandidateLifecycle::Cancelling, StreamingCellCandidateLifecycle::Closed}) {
            auto context = CandidateTestSupport::Context();
            context.lifecycle = lifecycle;
            RequireError(ParseStreamingCellArtifact(manifest, context, bytes), WorldStreamingErrors::CellCandidateLifecycleUnavailable);
        }
        auto context = CandidateTestSupport::Context();
        context.operation.fence.partition = TestSupport::World(2);
        RequireError(ParseStreamingCellArtifact(manifest, context, bytes), WorldStreamingErrors::CellCandidateStale);
        context = CandidateTestSupport::Context();
        const auto old = ParseStreamingCellArtifact(manifest, context, bytes).Value();
        context.operation.fence.generation = TestSupport::IdentityFrom<StreamingGeneration>(2);
        const auto replacement = ParseStreamingCellArtifact(manifest, context, bytes).Value();
        REQUIRE(old.Operation() != replacement.Operation());
        REQUIRE(old.Operation().fence.generation == TestSupport::IdentityFrom<StreamingGeneration>(1));
    }
}  // namespace Horo::WorldStreaming
