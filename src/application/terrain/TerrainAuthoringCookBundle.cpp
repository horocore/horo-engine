#include "TerrainAuthoringCookInternal.h"

#include <algorithm>

namespace Horo::Application::TerrainCookDetail {
    namespace {
        /** @brief Bounded canonical framing; no native pointers, padding or host-endian integers enter durable bytes. */
        struct Bytes final {
            std::vector<std::uint8_t> value;
            std::size_t maximum;

            void U(std::uint64_t number, unsigned count = 8) {
                for (unsigned i = 0; i < count; ++i)
                    value.push_back(static_cast<std::uint8_t>((number >> (8U * i)) & 255U));
            }

            void Digest(const Sha256Digest &digest) {
                value.insert(value.end(), digest.bytes.begin(), digest.bytes.end());
            }

            Result<void> Payload(std::span<const std::uint8_t> bytes) {
                if (value.size() > maximum || maximum - value.size() < 8 || bytes.size() > maximum - value.size() - 8)
                    return Result<void>::Failure(MakeError(Terrain::TerrainSourceErrors::LimitExceeded));
                U(bytes.size());
                value.insert(value.end(), bytes.begin(), bytes.end());
                return Result<void>::Success();
            }
        };

        void TilePolicy(Bytes &bytes, const Terrain::TerrainTileCookProfile &profile) {
            bytes.U(profile.interiorQuads);
            bytes.U(profile.lodLevels);
            bytes.U(static_cast<std::uint8_t>(profile.tier));
            bytes.Digest(profile.targetDigest);
            bytes.Digest(profile.toolchainDigest);
            bytes.U(profile.maximumTiles);
            bytes.U(profile.maximumPayloadBytes);
            bytes.U(profile.maximumWorkItems);
        }

        Result<void> AppendTiles(Bytes &bytes, const Terrain::CookedTerrainTileSet &tiles, const CancellationToken &cancellation) {
            bytes.U(tiles.tiles.size());
            for (const auto &tile : tiles.tiles) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Cancelled));
                auto appended = bytes.Payload(tile.payload);
                if (appended.HasError())
                    return appended;
            }
            return Result<void>::Success();
        }

        Result<void> AppendRoles(Bytes &bytes, std::span<const Terrain::TerrainSourceArtifact> artifacts,
                                 const CancellationToken &cancellation) {
            bytes.U(artifacts.size());
            for (const auto &artifact : artifacts) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Cancelled));
                auto appended = bytes.Payload(artifact.payload);
                if (appended.HasError())
                    return appended;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ProfileDigest */
    Sha256Digest ProfileDigest(const TerrainAuthoringCookProfile &profile) {
        Bytes bytes{{}, 4096};
        bytes.value.reserve(512);
        TilePolicy(bytes, profile.artifacts.tiles);
        bytes.U(profile.artifacts.collisionLod);
        bytes.U(profile.artifacts.navigationLod);
        bytes.U(profile.artifacts.maximumVertices);
        bytes.U(profile.artifacts.maximumTriangles);
        bytes.U(profile.artifacts.maximumOwnedBytes);
        bytes.U(profile.artifacts.maximumWorkItems);
        bytes.U(profile.configuration.Data().configuration.Value());
        bytes.U(profile.configuration.Data().capability.Value());
        bytes.U(static_cast<std::uint8_t>(profile.configuration.Data().tier));
        return ComputeSha256(std::as_bytes(std::span(bytes.value)));
    }

    /** @copydoc MakeBundle */
    Result<std::vector<std::uint8_t>> MakeBundle(const Capture &capture, const TerrainAuthoringCookProfile &profile,
                                                 const CancellationToken &cancellation) {
        using Return = Result<std::vector<std::uint8_t>>;
        // No compiled material is consumed by this geometry producer. Material/type/mesh SOURCE commitments live
        // in the generic complete input capture; none is misrepresented as a TerrainTileCookDependency artifact.
        auto cooked = Terrain::CookTerrainSourceArtifacts(capture.source.source, profile.artifacts, {}, cancellation);
        if (cooked.HasError())
            return Return::Failure(std::move(cooked).ErrorValue());
        if (auto current = cooked.Value().ValidateCurrent(capture.source.source); current.HasError())
            return Return::Failure(std::move(current).ErrorValue());
        Bytes bytes{{'H', 'T', 'A', 'B', 1, 0, 0, 0}, profile.assets.maximumArtifactBytes};
        bytes.value.reserve(std::min<std::size_t>(profile.assets.maximumArtifactBytes, 1024 * 1024));
        bytes.Digest(capture.inputs->ClosureDigest());
        bytes.Digest(ProfileDigest(profile));
        if (auto manifest = bytes.Payload(cooked.Value().Manifest()); manifest.HasError())
            return Return::Failure(std::move(manifest).ErrorValue());
        if (auto tiles = AppendTiles(bytes, cooked.Value().Tiles(), cancellation); tiles.HasError())
            return Return::Failure(std::move(tiles).ErrorValue());
        if (auto roles = AppendRoles(bytes, cooked.Value().Artifacts(), cancellation); roles.HasError())
            return Return::Failure(std::move(roles).ErrorValue());
        if (bytes.value.size() > bytes.maximum || bytes.maximum - bytes.value.size() < 32)
            return Return::Failure(MakeError(Terrain::TerrainSourceErrors::LimitExceeded));
        bytes.Digest(ComputeSha256(std::as_bytes(std::span(bytes.value))));
        return Return::Success(std::move(bytes.value));
    }
}  // namespace Horo::Application::TerrainCookDetail
