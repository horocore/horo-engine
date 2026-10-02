#include "Horo/Destruction/ChunkMeshCook.h"

#include "ChunkMeshCookInternal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <set>

namespace Horo::Destruction::ChunkMeshCookErrors {
    namespace {
        const ErrorDomainId kDomain{"horo.destruction"};
        constexpr auto kSeverity = ErrorSeverity::Error;
    }  // namespace

    const ErrorCodeDescriptor InvalidInput{kDomain, ErrorCode{"destruction.mesh.invalid_input"}, kSeverity,
                                           "Chunk mesh source or provenance is invalid.", "Regenerate the fracture candidate."};
    const ErrorCodeDescriptor InvalidInterior{kDomain, ErrorCode{"destruction.mesh.invalid_interior"}, kSeverity,
                                              "Interior face geometry or pairing is invalid.", "Repair the cut geometry before cooking."};
    const ErrorCodeDescriptor MissingMaterial{kDomain, ErrorCode{"destruction.mesh.missing_material"}, kSeverity,
                                              "A chunk face has no exact material dependency.", "Bind every exterior and interior slot."};
    const ErrorCodeDescriptor LimitExceeded{kDomain, ErrorCode{"destruction.mesh.limit_exceeded"}, kSeverity,
                                            "Chunk mesh exceeds its work or decoded byte limit.",
                                            "Reduce geometry or raise a supported limit."};
    const ErrorCodeDescriptor Cancelled{kDomain, ErrorCode{"destruction.mesh.cancelled"}, kSeverity, "Chunk mesh cook was cancelled.",
                                        "Retry from the current content generation."};
    const ErrorCodeDescriptor Stale{kDomain, ErrorCode{"destruction.mesh.stale"}, kSeverity,
                                    "Chunk mesh candidate is from a stale generation.", "Cook again from current content."};
    const ErrorCodeDescriptor Shutdown{kDomain, ErrorCode{"destruction.mesh.shutdown"}, kSeverity, "Chunk mesh owner has closed admission.",
                                       "Do not publish after shutdown."};
}  // namespace Horo::Destruction::ChunkMeshCookErrors

namespace Horo::Destruction {
    using namespace ChunkMeshDetail;

    /** @copydoc ValidateChunkMeshArtifact */
    Result<void> ValidateChunkMeshArtifact(const ChunkMeshArtifact &artifact) {
        if (artifact.schemaVersion != ChunkMeshCookSchemaVersion || !artifact.content.IsValid() || artifact.chunks.empty() ||
            artifact.integrityDigest != ArtifactDigest(artifact))
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
        return Result<void>::Success();
    }

    /** @copydoc ComputeChunkMeshSemanticDigest */
    Sha256Digest ComputeChunkMeshSemanticDigest(const OfflineVoronoiCandidate &source, std::span<const ChunkMaterialBinding> materials,
                                                ChunkUvPolicy uv) {
        Sha256Builder hash;
        HashU64(hash, ChunkMeshCookSchemaVersion);
        HashDigest(hash, source.semanticFingerprint);
        HashU64(hash, static_cast<std::uint8_t>(source.tier));
        HashDigest(hash, MaterialDigest(materials));
        HashDouble(hash, uv.exteriorScale);
        HashDouble(hash, uv.interiorScale);
        return hash.Finalize();
    }

    /** @copydoc ComputePreFracturedMeshSourceDigest */
    Sha256Digest ComputePreFracturedMeshSourceDigest(const PreFracturedCandidate &source) {
        Sha256Builder hash;
        HashU64(hash, source.SchemaVersion());
        HashU64(hash, source.Chunks().size());
        for (const auto &chunk : source.Chunks()) {
            HashU64(hash, chunk.id.Value());
            HashU64(hash, chunk.parent.Value());
            for (const double value : chunk.geometryToWorld)
                HashDouble(hash, value);
            HashU64(hash, chunk.positions.size());
            for (const auto &position : chunk.positions)
                for (const float value : position)
                    HashFloat(hash, value);
            HashU64(hash, chunk.triangleIndices.size());
            for (const auto index : chunk.triangleIndices)
                HashU64(hash, index);
            HashU64(hash, chunk.triangleMaterials.size());
            for (const auto &name : chunk.triangleMaterials)
                HashName(hash, name);
        }
        return hash.Finalize();
    }

    /** @copydoc ComputePreFracturedMeshSemanticDigest */
    Sha256Digest ComputePreFracturedMeshSemanticDigest(const PreFracturedCandidate &source, Assets::AssetId sourceAsset,
                                                       std::uint64_t sourceRevision, DestructionFeatureTier tier,
                                                       std::span<const ImportedChunkMaterialBinding> materials, ChunkUvPolicy uv) {
        Sha256Builder hash;
        HashU64(hash, ChunkMeshCookSchemaVersion);
        for (const auto byte : sourceAsset.Bytes())
            HashByte(hash, byte);
        HashU64(hash, sourceRevision);
        HashU64(hash, static_cast<std::uint8_t>(tier));
        HashDigest(hash, ComputePreFracturedMeshSourceDigest(source));
        HashU64(hash, materials.size());
        for (const auto &binding : materials) {
            HashName(hash, binding.sourceName);
            HashU64(hash, binding.material.slot);
            for (const auto byte : binding.material.asset.Bytes())
                HashByte(hash, byte);
            HashDigest(hash, binding.material.revisionDigest);
            HashByte(hash, binding.interior ? 1 : 0);
        }
        HashDouble(hash, uv.exteriorScale);
        HashDouble(hash, uv.interiorScale);
        return hash.Finalize();
    }

    /** @copydoc ChunkMeshCookOwner::Revision */
    std::uint64_t ChunkMeshCookOwner::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc ChunkMeshCookOwner::Token */
    CancellationToken ChunkMeshCookOwner::Token() const noexcept {
        return cancellation_.Token();
    }

    /** @copydoc ChunkMeshCookOwner::Accept */
    Result<void> ChunkMeshCookOwner::Accept(std::shared_ptr<const ChunkMeshArtifact> candidate, std::uint64_t expectedRevision,
                                            const FractureArtifactContentIdentity &currentContent) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Shutdown));
        if (revision_ != expectedRevision || cancellation_.Token().IsCancellationRequested() || !candidate || !currentContent.IsValid() ||
            candidate->content != currentContent)
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Stale));
        if (candidate->integrityDigest != ArtifactDigest(*candidate))
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
        current_ = std::move(candidate);
        return Result<void>::Success();
    }

    /** @copydoc ChunkMeshCookOwner::Invalidate */
    Result<void> ChunkMeshCookOwner::Invalidate() {
        if (shutdown_)
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Shutdown));
        if (revision_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
        cancellation_.RequestCancellation();
        cancellation_ = CancellationSource{};
        ++revision_;
        return Result<void>::Success();
    }

    /** @copydoc ChunkMeshCookOwner::Shutdown */
    void ChunkMeshCookOwner::Shutdown() noexcept {
        shutdown_ = true;
        cancellation_.RequestCancellation();
    }
}  // namespace Horo::Destruction
