#include "ChunkMeshCookInternal.h"

namespace Horo::Destruction {
    using namespace ChunkMeshDetail;

    namespace {
        struct VoronoiContext final {
            std::map<DestructionChunkId, Point> sites;
            std::map<InteriorPair, InteriorArea> areas;
            Budget budget;

            VoronoiContext(const DestructionLimits &limits) : budget{sizeof(ChunkMeshArtifact), 0, limits} {}
        };

        [[nodiscard]] Result<void> ValidateSource(const OfflineVoronoiCandidate &source, FractureArtifactContentIdentity content,
                                                  std::span<const ChunkMaterialBinding> materials, ChunkUvPolicy uv,
                                                  const DestructionLimits &limits, VoronoiContext &context) {
            if (!content.IsValid() || !source.IsIntact() || !source.sourceAsset.IsValid() || source.sourceRevision == 0 ||
                source.recipeId == 0 || source.recipeRevision == 0 || !Nonzero(source.sourceDigest) ||
                !Nonzero(source.semanticFingerprint) || source.schemaVersion != OfflineVoronoiSchemaVersion || source.chunks.empty() ||
                !std::isfinite(uv.exteriorScale) || !std::isfinite(uv.interiorScale) || uv.exteriorScale <= 0.0 || uv.interiorScale <= 0.0)
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            if (!ValidLimits(limits, source.tier) || source.chunks.size() > limits.maximumChunksPerDestructible)
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            if (content.SemanticDigest() != ComputeChunkMeshSemanticDigest(source, materials, uv))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Stale));
            for (std::size_t i = 0; i < materials.size(); ++i) {
                if (!materials[i].asset.IsValid() || !Nonzero(materials[i].revisionDigest) ||
                    (i != 0 && materials[i - 1].slot >= materials[i].slot))
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::MissingMaterial));
            }
            for (const auto &chunk : source.chunks) {
                if (!chunk.id.IsValid() || !Finite(chunk.site) || !context.sites.emplace(chunk.id, chunk.site).second)
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            }
            if (!context.budget.Charge(materials.size() * sizeof(ChunkMaterialBinding), materials.size()))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendChunk(const OfflineVoronoiChunk &chunk, std::span<const ChunkMaterialBinding> materials,
                                               ChunkUvPolicy uv, VoronoiContext &context, const CancellationToken &cancellation,
                                               ChunkMeshArtifact &artifact) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
            if (chunk.triangles.empty() || chunk.positions.empty() || !std::isfinite(chunk.volume) || chunk.volume <= 0.0 ||
                !Finite(chunk.centerOfMass))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            if (!std::ranges::is_sorted(chunk.neighbors) || std::ranges::adjacent_find(chunk.neighbors) != chunk.neighbors.end())
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
            for (const auto neighbor : chunk.neighbors) {
                if (context.sites.find(neighbor) == context.sites.end() || neighbor == chunk.id)
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
                context.areas.try_emplace(std::minmax(chunk.id, neighbor));
            }
            if (!context.budget.Charge(sizeof(ChunkMesh) + chunk.triangles.size() * (sizeof(ChunkMeshFace) + 3 * sizeof(ChunkMeshVertex)),
                                       chunk.positions.size() + chunk.triangles.size() * (3 + chunk.neighbors.size())))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            ChunkMesh output;
            output.id = chunk.id;
            output.vertices.reserve(chunk.triangles.size() * 3);
            output.faces.reserve(chunk.triangles.size());
            output.mass.volume = chunk.volume;
            for (std::size_t axis = 0; axis < 3; ++axis)
                output.mass.firstMoment[axis] = chunk.volume * chunk.centerOfMass[axis];
            for (const auto &face : chunk.triangles) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
                const auto material = std::ranges::lower_bound(materials, face.materialSlot, {}, &ChunkMaterialBinding::slot);
                if (material == materials.end() || material->slot != face.materialSlot)
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::MissingMaterial));
                auto appended = AppendFace(chunk, face, uv, context.sites, context.areas, output);
                if (appended.HasError())
                    return appended;
            }
            if (output.vertices.empty())
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            SetMeshBounds(output);
            artifact.chunks.push_back(std::move(output));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CheckAreas(const VoronoiContext &context) {
            for (const auto &[key, area] : context.areas) {
                (void)key;
                if (!(area.low > 0.0) || !(area.high > 0.0) || std::abs(area.low - area.high) > 1.0e-3 * std::max(area.low, area.high))
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CookChunkMeshes */
    Result<std::shared_ptr<const ChunkMeshArtifact>> CookChunkMeshes(const OfflineVoronoiCandidate &source,
                                                                     FractureArtifactContentIdentity content,
                                                                     std::span<const ChunkMaterialBinding> materials, ChunkUvPolicy uv,
                                                                     const DestructionLimits &limits,
                                                                     const CancellationToken &cancellation) {
        using Output = Result<std::shared_ptr<const ChunkMeshArtifact>>;
        if (cancellation.IsCancellationRequested())
            return Output::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
        VoronoiContext context{limits};
        auto valid = ValidateSource(source, content, materials, uv, limits, context);
        if (valid.HasError())
            return Output::Failure(valid.ErrorValue());
        auto artifact = std::shared_ptr<ChunkMeshArtifact>(new ChunkMeshArtifact);
        artifact->content = content;
        artifact->sourceAsset = source.sourceAsset;
        artifact->sourceRevision = source.sourceRevision;
        artifact->sourceDigest = source.sourceDigest;
        artifact->recipeId = source.recipeId;
        artifact->recipeRevision = source.recipeRevision;
        artifact->inputFingerprint = source.semanticFingerprint;
        artifact->materialFingerprint = MaterialDigest(materials);
        artifact->tier = source.tier;
        artifact->producedFeatures.bits = DestructionFeatureBit<DestructionFeature::PreCookedFracture>;
        artifact->materials.assign(materials.begin(), materials.end());
        for (const auto &chunk : source.chunks) {
            auto appended = AppendChunk(chunk, materials, uv, context, cancellation, *artifact);
            if (appended.HasError())
                return Output::Failure(appended.ErrorValue());
        }
        auto checked = CheckAreas(context);
        if (checked.HasError())
            return Output::Failure(checked.ErrorValue());
        artifact->estimatedBytes = context.budget.bytes;
        artifact->workItems = context.budget.work;
        artifact->integrityDigest = ArtifactDigest(*artifact);
        return Output::Success(std::move(artifact));
    }
}  // namespace Horo::Destruction
