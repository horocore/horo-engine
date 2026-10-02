#pragma once

/** @file ChunkCollisionArtifact.h
 * @brief Source-free packaged DFR collision dependency closure and exact runtime loading.
 */

#include "Horo/Destruction/DestructibleDescriptor.h"
#include "Horo/Physics/PhysicsCompoundCook.h"

#include <memory>

namespace Horo::Destruction {
    struct ChunkMeshArtifact;
    struct ChunkCollisionCookRequest;

    /** @brief Stable chunk and target-specific Physics product; Assets owns physical storage/publication. */
    struct ChunkCollisionArtifact final {
        DestructionChunkId chunk;
        Physics::PhysicsCompoundCookResult shape;
    };

    /** @brief Immutable detached dependency closure; it contains cooked bytes and no source geometry. */
    class ChunkCollisionArtifactSet final {
        struct CookKey final {
            CookKey(const CookKey &) = default;

        private:
            CookKey() = default;
            friend class ChunkCollisionArtifactSet;
        };

        static CookKey ConstructionKey() {
            return {};
        }

    public:
        /** @brief Constructs an immutable result only for authorized cooks/loaders. @param key Private construction key. */
        explicit ChunkCollisionArtifactSet([[maybe_unused]] const CookKey &key) {}

        /** @brief Exact DFR content consumed by Physics. @return Borrowed immutable identity. */
        [[nodiscard]] const FractureArtifactContentIdentity &Content() const noexcept {
            return content_;
        }

        /** @brief Exact source mesh revision digest. @return Borrowed immutable digest. */
        [[nodiscard]] const Sha256Digest &MeshDigest() const noexcept {
            return meshDigest_;
        }

        /** @brief Exact captured Physics profile. @return Borrowed immutable target. */
        [[nodiscard]] const Physics::PhysicsShapeCookTargetDigest &Target() const noexcept {
            return target_;
        }

        /** @brief Exact admitted DFR tier. @return Product tier, with no silent fallback. */
        [[nodiscard]] DestructionFeatureTier Tier() const noexcept {
            return tier_;
        }

        /** @brief Complete stable-ID ordered packaged shape products. @return Borrowed immutable artifacts. */
        [[nodiscard]] std::span<const ChunkCollisionArtifact> Shapes() const noexcept {
            return shapes_;
        }

        /** @brief Exact captured settings/material fingerprint. @return Borrowed immutable dependency digest. */
        [[nodiscard]] const Sha256Digest &ConfigurationDigest() const noexcept {
            return requestDigest_;
        }

        /** @brief Aggregate output charge. @return Cooked bytes bounded by the request. */
        [[nodiscard]] std::uint64_t Bytes() const noexcept {
            return bytes_;
        }

    private:
        friend class ChunkCollisionCookOwner;
        friend Result<std::shared_ptr<const ChunkCollisionArtifactSet>> LoadChunkCollisionArtifacts(
            const struct ChunkCollisionBundleReference &, std::span<const std::uint8_t>, const DestructionLimits &);
        friend Result<std::shared_ptr<const ChunkCollisionArtifactSet>> CookChunkCollision(const ChunkMeshArtifact &,
                                                                                           const ChunkCollisionCookRequest &,
                                                                                           const CancellationToken &);
        FractureArtifactContentIdentity content_;
        Sha256Digest meshDigest_;
        Physics::PhysicsShapeCookTargetDigest target_;
        DestructionFeatureTier tier_{};
        std::vector<ChunkCollisionArtifact> shapes_;
        std::uint64_t bytes_{};
        Sha256Digest requestDigest_;
    };

    /** @brief Exact catalog reference for one complete collision bundle, never a latest-version lookup. */
    struct ChunkCollisionBundleReference final {
        FractureArtifactContentIdentity content;
        Sha256Digest meshDigest;
        Physics::PhysicsShapeCookTargetDigest target;
        Sha256Digest payloadDigest;
    };

    /**
     * @brief Encodes all chunk shapes as one deterministic asset payload for Assets archive publication.
     * @param artifacts Complete immutable offline or loaded artifact set.
     * @param maximumBytes Finite encoded byte ceiling, at most the qualified DFR maximum.
     * @return Complete bundle bytes or typed capacity failure; no source geometry is encoded.
     */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodeChunkCollisionArtifacts(
        const ChunkCollisionArtifactSet &artifacts, std::uint64_t maximumBytes = DestructionHardLimits::ArtifactBytes);

    /**
     * @brief Loads and verifies every packaged chunk/Physics artifact without source data or cooking.
     * @param reference Exact owner-captured content, mesh, Physics target and bundle digest.
     * @param payload Immutable bytes supplied by the runtime Assets provider.
     * @param limits Runtime finite count, output and verification-work admission.
     * @return Complete immutable set ready for Physics cache acquisition or typed zero-publication failure.
     */
    [[nodiscard]] Result<std::shared_ptr<const ChunkCollisionArtifactSet>> LoadChunkCollisionArtifacts(
        const ChunkCollisionBundleReference &reference, std::span<const std::uint8_t> payload, const DestructionLimits &limits);
}  // namespace Horo::Destruction
