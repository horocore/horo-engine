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

            template <std::size_t N> void Id(const std::array<std::uint8_t, N> &identity) {
                value.insert(value.end(), identity.begin(), identity.end());
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

        /** @brief Serialize producer-issued fixed-point values; no native handles or inferred mesh bounds are stored. */
        void Instance(Bytes &bytes, const Terrain::CookedFoliageInstance &instance) {
            bytes.Id(instance.id.Bytes());
            bytes.Id(instance.cluster.Bytes());
            bytes.Id(instance.type.Bytes());
            bytes.U(static_cast<std::uint64_t>(instance.xMillimeters));
            bytes.U(static_cast<std::uint64_t>(instance.altitudeMillimeters));
            bytes.U(static_cast<std::uint64_t>(instance.zMillimeters));
            bytes.U(instance.scalePermille, 4);
            bytes.U(instance.yawMilliDegrees, 4);
            bytes.U(instance.slopeMilliDegrees, 4);
            bytes.U(instance.candidateOrdinal, 4);
            bytes.U(static_cast<std::uint16_t>(instance.normalXPermille), 2);
            bytes.U(static_cast<std::uint16_t>(instance.normalYPermille), 2);
            bytes.U(static_cast<std::uint16_t>(instance.normalZPermille), 2);
        }

        Result<void> Proof(Bytes &bytes, const Terrain::CookedFoliagePlacement &proof, const CancellationToken &cancellation) {
            constexpr std::size_t HeaderBytes = 145;
            constexpr std::size_t InstanceBytes = 94;
            if (bytes.value.size() > bytes.maximum || bytes.maximum - bytes.value.size() < HeaderBytes)
                return Result<void>::Failure(MakeError(Terrain::TerrainSourceErrors::LimitExceeded));
            if (proof.Instances().size() > (bytes.maximum - bytes.value.size() - HeaderBytes) / InstanceBytes)
                return Result<void>::Failure(MakeError(Terrain::TerrainSourceErrors::LimitExceeded));
            auto tile = Terrain::SerializeTerrainTileId(proof.Tile());
            bytes.Id(tile);
            bytes.Id(proof.Type().Bytes());
            bytes.U(proof.DefinitionRevision().Value());
            bytes.U(proof.ContentRevision().Value());
            bytes.U(proof.SourceRevision().Value());
            bytes.U(proof.CapabilityRevision().Value());
            bytes.Digest(proof.ResultDigest());
            bytes.Digest(proof.Fingerprint());
            bytes.U(proof.Instances().size());
            std::size_t index{};
            for (const auto &instance : proof.Instances()) {
                if ((index++ % 64) == 0 && cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Cancelled));
                Instance(bytes, instance);
            }
            return Result<void>::Success();
        }

        Result<void> AppendFoliage(Bytes &bytes, std::span<const Terrain::CookedFoliagePlacement> proofs,
                                   const CancellationToken &cancellation) {
            bytes.U(proofs.size());
            for (const auto &proof : proofs) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Cancelled));
                if (auto appended = Proof(bytes, proof, cancellation); appended.HasError())
                    return appended;
            }
            return Result<void>::Success();
        }

        /** @brief Bound and cooperatively hash the complete domain bundle before generic Assets envelope/publication. */
        Result<std::vector<std::uint8_t>> Finish(Bytes bytes, const CancellationToken &cancellation) {
            using Return = Result<std::vector<std::uint8_t>>;
            if (bytes.value.size() > bytes.maximum || bytes.maximum - bytes.value.size() < 32)
                return Return::Failure(MakeError(Terrain::TerrainSourceErrors::LimitExceeded));
            Sha256Builder hash;
            for (std::size_t at = 0; at < bytes.value.size();) {
                if (cancellation.IsCancellationRequested())
                    return Return::Failure(MakeError(TerrainAuthoringCookErrors::Cancelled));
                const auto count = std::min<std::size_t>(4096, bytes.value.size() - at);
                if (!hash.Update(std::as_bytes(std::span(bytes.value).subspan(at, count))))
                    return Return::Failure(MakeError(TerrainAuthoringCookErrors::Invalid));
                at += count;
            }
            bytes.Digest(hash.Finalize());
            return Return::Success(std::move(bytes.value));
        }
    }  // namespace

    /** @copydoc ProfileDigest */
    Sha256Digest ProfileDigest(const TerrainAuthoringCookProfile &profile) {
        Bytes bytes{{}, 4096};
        bytes.value.reserve(512);
        constexpr std::string_view domain = "horo.terrain.authored.one-verified-tile-cluster.v1";
        bytes.U(domain.size());
        for (const auto byte : domain)
            bytes.U(static_cast<std::uint8_t>(byte), 1);
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
        bytes.U(profile.foliage.maximumCells);
        bytes.U(profile.foliage.maximumCandidates);
        bytes.U(profile.foliage.maximumInstances);
        bytes.U(profile.foliage.maximumClusters);
        bytes.U(profile.foliage.maximumExclusions);
        bytes.U(profile.foliage.maximumPredicateChecks);
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
        auto foliage = CookAuthoredFoliage(capture, cooked.Value().Tiles(), profile, cancellation);
        if (foliage.HasError())
            return Return::Failure(std::move(foliage).ErrorValue());
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
        if (auto proofs = AppendFoliage(bytes, foliage.Value(), cancellation); proofs.HasError())
            return Return::Failure(std::move(proofs).ErrorValue());
        return Finish(std::move(bytes), cancellation);
    }
}  // namespace Horo::Application::TerrainCookDetail
