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

        struct MassAccumulator final {
            double sixVolume{};
            Point weightedCenter{};
        };

        struct ImportContext final {
            std::map<std::string, ImportedChunkMaterialBinding, std::less<>> mappings;
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
                    !context.mappings.try_emplace(binding.sourceName, binding).second ||
                    !context.slots.insert(binding.material.slot).second)
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
            auto [it, inserted] = context.cuts.try_emplace(key, chunk.id, normal, face.interior, 1U);
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
                                                  MassAccumulator &mass) {
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
            const std::array<Point, 3> points{Position(chunk.positions[face.indices[0]]), Position(chunk.positions[face.indices[1]]),
                                              Position(chunk.positions[face.indices[2]])};
            if (!Finite(points[0]) || !Finite(points[1]) || !Finite(points[2]))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            const Point normal = Cross(Sub(points[1], points[0]), Sub(points[2], points[0]));
            const double v6 = Dot(points[0], normal);
            if (!std::isfinite(v6))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            mass.sixVolume += v6;
            for (std::size_t axis = 0; axis < 3; ++axis)
                mass.weightedCenter[axis] += (points[0][axis] + points[1][axis] + points[2][axis]) * v6;
            if (auto recorded = RecordCut(chunk, face, normal, context); recorded.HasError())
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
            MassAccumulator mass{};
            for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
                if (auto appended = AppendTriangle(chunk, triangle, uv, cancellation, context, output, mass); appended.HasError())
                    return appended;
            }
            output.mass.volume = mass.sixVolume / 6.0;
            if (!(output.mass.volume > 0.0) || !std::isfinite(output.mass.volume))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            for (std::size_t axis = 0; axis < 3; ++axis)
                output.mass.firstMoment[axis] = mass.weightedCenter[axis] / 24.0;
            if (!context.budget.Charge(sizeof(ChunkCollisionPiece) + chunk.positions.size() * sizeof(chunk.positions.front()) +
                                           triangleCount * sizeof(std::array<std::uint32_t, 3>),
                                       chunk.positions.size() + triangleCount))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            ChunkCollisionPiece piece;
            piece.positions = chunk.positions;
            piece.volume = output.mass.volume;
            piece.triangles.reserve(triangleCount);
            for (std::size_t offset = 0; offset < chunk.triangleIndices.size(); offset += 3)
                piece.triangles.push_back(
                    {chunk.triangleIndices[offset], chunk.triangleIndices[offset + 1], chunk.triangleIndices[offset + 2]});
            piece.id = PieceIdentity(piece);
            output.collisionPieces.push_back(std::move(piece));
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
        if (auto valid = ValidateImport(source, request, materials, context); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        auto artifact = std::make_shared<ChunkMeshArtifact>(ChunkMeshArtifact::ConstructionKey());
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
            if (auto appended = AppendChunk(chunk, request.uv, cancellation, context, *artifact); appended.HasError())
                return Output::Failure(appended.ErrorValue());
        }
        if (auto checked = CheckCuts(context); checked.HasError())
            return Output::Failure(checked.ErrorValue());
        artifact->estimatedBytes = context.budget.bytes;
        artifact->workItems = context.budget.work;
        artifact->integrityDigest = ArtifactDigest(*artifact);
        return Output::Success(std::move(artifact));
    }
}  // namespace Horo::Destruction
