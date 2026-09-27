#include "ChunkMeshCookInternal.h"

#include <set>

namespace Horo::Destruction {
    using namespace ChunkMeshDetail;

    namespace {
        using TriangleKey = std::array<std::array<float, 3>, 3>;

        struct CutFace final {
            DestructionChunkId chunk;
            Point normal;
            bool interior{};
            std::uint32_t count{};
        };

        struct ImportContext final {
            std::map<std::string, ImportedChunkMaterialBinding> mappings;
            std::set<std::uint32_t> slots;
            std::map<TriangleKey, CutFace> cuts;
            const std::map<DestructionChunkId, Point> noSites;
            std::map<InteriorPair, InteriorArea> noAreas;
            Budget budget;
            DestructionChunkId previous{};

            explicit ImportContext(const DestructionLimits &limits) : budget{sizeof(ChunkMeshArtifact), 0, limits} {}
        };

        [[nodiscard]] Result<void> ValidateImport(const PreFracturedCandidate &source, const ImportedChunkMeshCookRequest &request,
                                                  std::span<const ImportedChunkMaterialBinding> materials, ImportContext &context) {
            if (!request.sourceAsset.IsValid() || request.sourceRevision == 0 || !request.content.IsValid() ||
                source.SchemaVersion() != CurrentPreFracturedImportSchemaVersion || source.Chunks().empty() ||
                !std::isfinite(request.uv.exteriorScale) || !std::isfinite(request.uv.interiorScale) || request.uv.exteriorScale <= 0 ||
                request.uv.interiorScale <= 0)
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            if (!ValidLimits(request.limits, request.tier) || source.Chunks().size() > request.limits.maximumChunksPerDestructible)
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            if (request.sourceDigest != ComputePreFracturedMeshSourceDigest(source) ||
                request.content.SemanticDigest() != ComputePreFracturedMeshSemanticDigest(source, request.sourceAsset,
                                                                                          request.sourceRevision, request.tier, materials,
                                                                                          request.uv))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Stale));
            std::uint64_t materialBytes = 0;
            for (const auto &binding : materials) {
                if (binding.sourceName.empty() || !binding.material.asset.IsValid() || !Nonzero(binding.material.revisionDigest) ||
                    !context.mappings.emplace(binding.sourceName, binding).second || !context.slots.insert(binding.material.slot).second)
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::MissingMaterial));
                materialBytes += sizeof(binding) + binding.sourceName.size();
            }
            if (!context.budget.Charge(materialBytes, materials.size()))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> RecordCut(const PreFracturedChunk &chunk, const OfflineVoronoiTriangle &face, const Point &normal,
                                             ImportContext &context) {
            TriangleKey key{chunk.positions[face.indices[0]], chunk.positions[face.indices[1]], chunk.positions[face.indices[2]]};
            std::ranges::sort(key);
            auto [it, inserted] = context.cuts.try_emplace(key, CutFace{chunk.id, normal, face.interior, 1});
            if (!inserted) {
                if (it->second.count != 1 || it->second.chunk == chunk.id || !it->second.interior || !face.interior ||
                    Dot(it->second.normal, normal) >= 0)
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
                it->second.count = 2;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendTriangle(const PreFracturedChunk &chunk, std::size_t triangle, ChunkUvPolicy uv,
                                                  const CancellationToken &cancellation, ImportContext &context, ChunkMesh &output,
                                                  double &sixVolume, Point &weightedCenter) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
            const auto found = context.mappings.find(chunk.triangleMaterials[triangle]);
            if (found == context.mappings.end())
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::MissingMaterial));
            const auto offset = triangle * 3;
            OfflineVoronoiTriangle face{{chunk.triangleIndices[offset], chunk.triangleIndices[offset + 1],
                                         chunk.triangleIndices[offset + 2]},
                                        found->second.material.slot,
                                        found->second.interior};
            if (std::ranges::any_of(face.indices, [&](std::uint32_t index) {
                return index >= chunk.positions.size();
            }))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            const Point a = Position(chunk.positions[face.indices[0]]);
            const Point b = Position(chunk.positions[face.indices[1]]);
            const Point c = Position(chunk.positions[face.indices[2]]);
            if (!Finite(a) || !Finite(b) || !Finite(c))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            const Point normal = Cross(Sub(b, a), Sub(c, a));
            const double v6 = Dot(a, normal);
            if (!std::isfinite(v6))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            sixVolume += v6;
            for (std::size_t axis = 0; axis < 3; ++axis)
                weightedCenter[axis] += (a[axis] + b[axis] + c[axis]) * v6;
            auto recorded = RecordCut(chunk, face, normal, context);
            if (recorded.HasError())
                return recorded;
            OfflineVoronoiChunk faceSource;
            faceSource.positions = {chunk.positions[face.indices[0]], chunk.positions[face.indices[1]], chunk.positions[face.indices[2]]};
            face.indices = {0, 1, 2};
            return AppendFace(faceSource, face, uv, context.noSites, context.noAreas, output);
        }

        [[nodiscard]] Result<void> AppendChunk(const PreFracturedChunk &chunk, ChunkUvPolicy uv, const CancellationToken &cancellation,
                                               ImportContext &context, ChunkMeshArtifact &artifact) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
            if (!chunk.id.IsValid() || (context.previous.IsValid() && chunk.id <= context.previous) || chunk.positions.empty() ||
                chunk.triangleIndices.empty() || chunk.triangleIndices.size() % 3 != 0 ||
                chunk.triangleMaterials.size() != chunk.triangleIndices.size() / 3)
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            context.previous = chunk.id;
            const auto triangleCount = chunk.triangleIndices.size() / 3;
            if (!context.budget.Charge(sizeof(ChunkMesh) + triangleCount * (sizeof(ChunkMeshFace) + 3 * sizeof(ChunkMeshVertex)),
                                       chunk.positions.size() + triangleCount * 6))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            ChunkMesh output;
            output.id = chunk.id;
            output.vertices.reserve(triangleCount * 3);
            output.faces.reserve(triangleCount);
            double sixVolume{};
            Point weightedCenter{};
            for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
                auto appended = AppendTriangle(chunk, triangle, uv, cancellation, context, output, sixVolume, weightedCenter);
                if (appended.HasError())
                    return appended;
            }
            output.mass.volume = sixVolume / 6.0;
            if (!(output.mass.volume > 0.0) || !std::isfinite(output.mass.volume))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            for (std::size_t axis = 0; axis < 3; ++axis)
                output.mass.firstMoment[axis] = weightedCenter[axis] / 24.0;
            SetMeshBounds(output);
            artifact.chunks.push_back(std::move(output));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CheckCuts(const ImportContext &context) {
            for (const auto &[key, cut] : context.cuts) {
                (void)key;
                if (cut.interior && cut.count != 2)
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CookPreFracturedChunkMeshes */
    Result<std::shared_ptr<const ChunkMeshArtifact>> CookPreFracturedChunkMeshes(const PreFracturedCandidate &source,
                                                                                 const ImportedChunkMeshCookRequest &request,
                                                                                 std::span<const ImportedChunkMaterialBinding> materials,
                                                                                 const CancellationToken &cancellation) {
        using Output = Result<std::shared_ptr<const ChunkMeshArtifact>>;
        if (cancellation.IsCancellationRequested())
            return Output::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
        ImportContext context{request.limits};
        auto valid = ValidateImport(source, request, materials, context);
        if (valid.HasError())
            return Output::Failure(valid.ErrorValue());
        auto artifact = std::shared_ptr<ChunkMeshArtifact>(new ChunkMeshArtifact);
        artifact->content = request.content;
        artifact->sourceAsset = request.sourceAsset;
        artifact->sourceRevision = request.sourceRevision;
        artifact->sourceDigest = request.sourceDigest;
        artifact->inputFingerprint = request.sourceDigest;
        artifact->tier = request.tier;
        artifact->producedFeatures.bits = DestructionFeatureBit<DestructionFeature::PreCookedFracture>;
        artifact->materials.reserve(materials.size());
        for (const auto &binding : materials)
            artifact->materials.push_back(binding.material);
        std::ranges::sort(artifact->materials, {}, &ChunkMaterialBinding::slot);
        artifact->materialFingerprint = MaterialDigest(artifact->materials);
        for (const auto &chunk : source.Chunks()) {
            auto appended = AppendChunk(chunk, request.uv, cancellation, context, *artifact);
            if (appended.HasError())
                return Output::Failure(appended.ErrorValue());
        }
        auto checked = CheckCuts(context);
        if (checked.HasError())
            return Output::Failure(checked.ErrorValue());
        artifact->estimatedBytes = context.budget.bytes;
        artifact->workItems = context.budget.work;
        artifact->integrityDigest = ArtifactDigest(*artifact);
        return Output::Success(std::move(artifact));
    }
}  // namespace Horo::Destruction
