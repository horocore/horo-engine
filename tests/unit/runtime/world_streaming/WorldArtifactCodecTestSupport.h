#pragma once

#include "Horo/WorldStreaming/StreamingCellEncoding.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"
#include "Horo/WorldStreaming/WorldIndexArtifact.h"
#include "StreamingCellCandidateTestSupport.h"

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>

namespace Horo::WorldStreaming::ArtifactCodecTestSupport {
    using TestSupport::RequireError;

    /** @brief Independent fixed-width golden-field access for assertions and malformed fixtures. */
    inline std::uint64_t Read(const std::span<const std::byte> bytes, const std::size_t offset, const std::size_t width) {
        std::uint64_t value{};
        for (std::size_t i = 0; i < width; ++i)
            value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
        return value;
    }

    /** @brief Alters one externally authored wire field without consulting the codec implementation. */
    inline void Write(std::vector<std::byte> &bytes, const std::size_t offset, std::uint64_t value, const std::size_t width) {
        for (std::size_t i = 0; i < width; ++i, value >>= 8)
            bytes[offset + i] = static_cast<std::byte>(value & 255);
    }

    /** @brief Recomputes the documented zero-field digest so structural rejection is exercised beyond corruption checks. */
    inline void Seal(std::vector<std::byte> &bytes) {
        std::ranges::fill(std::span{bytes}.subspan(152, 32), std::byte{0});
        const auto hash = ComputeSha256(bytes);
        for (std::size_t i = 0; i < hash.bytes.size(); ++i)
            bytes[152 + i] = static_cast<std::byte>(hash.bytes[i]);
    }

    /** @brief Mandatory small limits contain every allocation in the independent wire fixtures. */
    inline WorldIndexArtifactLimits Limits() {
        return {4096, 64, 4, {2, 8, 32}, {8, 8, 32, 4096, 4096}};
    }

    /** @brief Actual cooked cells and complete authoritative topology, including negative coordinates. */
    struct Fixture final {
        std::array<EncodedStreamingCell, 2> artifacts{Artifacts()};
        CookedWorldIndexManifest manifest{Manifest(artifacts)};

        static std::array<EncodedStreamingCell, 2> Artifacts() {
            const std::array first{std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'},
                                   std::byte{'6'}, std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
            const std::array second{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
            const std::array payloads{StreamingCellPayloadBytes{StreamingCellProvider::Terrain, StreamingCellPayloadRequirement::Optional,
                                                                1, second},
                                      StreamingCellPayloadBytes{StreamingCellProvider::CoreEcs, StreamingCellPayloadRequirement::Required,
                                                                1, first}};
            auto left = EncodeStreamingCellArtifact(CandidateTestSupport::Cell(-1), payloads, {2, 195, 12});
            auto right = EncodeStreamingCellArtifact(CandidateTestSupport::Cell(), payloads, {2, 195, 12});
            REQUIRE(left.HasValue());
            REQUIRE(right.HasValue());
            return {std::move(left).Value(), std::move(right).Value()};
        }

        static CookedWorldIndexManifest Manifest(const std::array<EncodedStreamingCell, 2> &cells) {
            auto grid = WorldCellQuantizationPolicy::Create({}, 1000, {-1, 0, 0, 0, 0, 0}, 1);
            REQUIRE(grid.HasValue());
            const std::array layers{
                WorldLayerDescriptor{TestSupport::Layer(), "base", WorldLayerOwnership::WorldStreaming, WorldLayerFlags::Persistent, 1.0F}};
            const std::array topology{WorldPartitionCellDescriptor{CandidateTestSupport::Cell(-1), {TestSupport::Asset(4)}},
                                      WorldPartitionCellDescriptor{CandidateTestSupport::Cell(), {TestSupport::Asset(5)}}};
            auto descriptor = WorldPartitionDescriptor::Create({}, TestSupport::World(),
                                                               {Math::WorldCoordinate64::FromMillimeters(-1000, 0, 0),
                                                                Math::WorldCoordinate64::FromMillimeters(999, 999, 999)},
                                                               grid.Value(), layers, topology, {1, 2, 4});
            REQUIRE(descriptor.HasValue());
            const auto &a = cells[0].integrity;
            const auto &b = cells[1].integrity;
            const std::array hard{topology[1].id};
            const std::array metadata{CookedWorldCellManifestCandidate{a.cell, a.uncompressedSize, a.compressedSize, a.payloadCrc32,
                                                                       a.artifactHash, hard},
                                      CookedWorldCellManifestCandidate{b.cell,
                                                                       b.uncompressedSize,
                                                                       b.compressedSize,
                                                                       b.payloadCrc32,
                                                                       b.artifactHash,
                                                                       {}}};
            auto manifest = CookedWorldIndexManifest::Create(std::move(descriptor).Value(), metadata, {2, 1, 1, 198, 24});
            REQUIRE(manifest.HasValue());
            return std::move(manifest).Value();
        }

        EncodedWorldIndex Encode(std::string_view name = "World", std::span<const WorldIndexVolume> volumes = {}) const {
            auto encoded = EncodeWorldIndexArtifact(manifest, name, WorldIndexCoordinateSystem::RightHandedYUp, volumes, Limits());
            if (encoded.HasError())
                INFO(encoded.ErrorValue().message);
            REQUIRE(encoded.HasValue());
            return std::move(encoded).Value();
        }
    };
}  // namespace Horo::WorldStreaming::ArtifactCodecTestSupport
