#include "Horo/Destruction/ChunkMeshCook.h"

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
    namespace {
        using Point = std::array<double, 3>;

        [[nodiscard]] Point Sub(const Point &a, const Point &b) {
            return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        }

        [[nodiscard]] double Dot(const Point &a, const Point &b) {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        [[nodiscard]] Point Cross(const Point &a, const Point &b) {
            return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        }

        [[nodiscard]] Point Position(const std::array<float, 3> &p) {
            return {p[0], p[1], p[2]};
        }

        [[nodiscard]] bool Finite(const Point &p) {
            return std::ranges::all_of(p, [](double v) {
                return std::isfinite(v);
            });
        }

        [[nodiscard]] bool Nonzero(const Sha256Digest &digest) {
            return std::ranges::any_of(digest.bytes, [](std::uint8_t byte) {
                return byte != 0;
            });
        }

        void HashByte(Sha256Builder &hash, std::uint8_t value) {
            const auto byte = static_cast<std::byte>(value);
            (void)hash.Update(std::span{&byte, 1});
        }

        void HashU64(Sha256Builder &hash, std::uint64_t value) {
            for (int shift = 56; shift >= 0; shift -= 8)
                HashByte(hash, static_cast<std::uint8_t>(value >> shift));
        }

        void HashDigest(Sha256Builder &hash, const Sha256Digest &digest) {
            for (const auto byte : digest.bytes)
                HashByte(hash, byte);
        }

        void HashName(Sha256Builder &hash, std::string_view name) {
            HashU64(hash, name.size());
            for (const char value : name)
                HashByte(hash, static_cast<std::uint8_t>(value));
        }

        [[nodiscard]] Sha256Digest MaterialDigest(std::span<const ChunkMaterialBinding> materials) {
            Sha256Builder hash;
            HashU64(hash, materials.size());
            for (const auto &material : materials) {
                HashU64(hash, material.slot);
                for (const auto byte : material.asset.Bytes())
                    HashByte(hash, byte);
                HashDigest(hash, material.revisionDigest);
            }
            return hash.Finalize();
        }

        void HashFloat(Sha256Builder &hash, float value) {
            HashU64(hash, std::bit_cast<std::uint32_t>(value));
        }

        void HashDouble(Sha256Builder &hash, double value) {
            HashU64(hash, std::bit_cast<std::uint64_t>(value));
        }

        [[nodiscard]] Sha256Digest ArtifactDigest(const ChunkMeshArtifact &artifact) {
            Sha256Builder hash;
            HashU64(hash, artifact.schemaVersion);
            HashU64(hash, static_cast<std::uint8_t>(artifact.tier));
            HashU64(hash, artifact.producedFeatures.bits);
            HashDigest(hash, artifact.content.SemanticDigest());
            for (const auto byte : artifact.sourceAsset.Bytes())
                HashByte(hash, byte);
            HashU64(hash, artifact.sourceRevision);
            HashDigest(hash, artifact.sourceDigest);
            HashU64(hash, artifact.recipeId);
            HashU64(hash, artifact.recipeRevision);
            HashDigest(hash, artifact.inputFingerprint);
            HashDigest(hash, artifact.materialFingerprint);
            HashU64(hash, artifact.materials.size());
            for (const auto &material : artifact.materials) {
                HashU64(hash, material.slot);
                for (const auto byte : material.asset.Bytes())
                    HashByte(hash, byte);
                HashDigest(hash, material.revisionDigest);
            }
            HashU64(hash, artifact.chunks.size());
            for (const auto &chunk : artifact.chunks) {
                HashU64(hash, chunk.id.Value());
                HashU64(hash, chunk.vertices.size());
                for (const auto &vertex : chunk.vertices) {
                    for (const float value : vertex.position)
                        HashFloat(hash, value);
                    for (const float value : vertex.normal)
                        HashFloat(hash, value);
                    for (const float value : vertex.tangent)
                        HashFloat(hash, value);
                    for (const float value : vertex.uv)
                        HashFloat(hash, value);
                }
                HashU64(hash, chunk.faces.size());
                for (const auto &face : chunk.faces) {
                    for (const auto index : face.indices)
                        HashU64(hash, index);
                    HashU64(hash, face.materialSlot);
                    HashByte(hash, face.interior ? 1 : 0);
                }
                for (float value : chunk.mass.minimum)
                    HashFloat(hash, value);
                for (float value : chunk.mass.maximum)
                    HashFloat(hash, value);
                HashDouble(hash, chunk.mass.volume);
                for (double value : chunk.mass.firstMoment)
                    HashDouble(hash, value);
            }
            HashU64(hash, artifact.estimatedBytes);
            HashU64(hash, artifact.workItems);
            return hash.Finalize();
        }

        [[nodiscard]] bool ValidLimits(const DestructionLimits &limits, DestructionFeatureTier tier) {
            const auto profile = GetDestructionTierProfile(tier);
            if (profile.HasError() || !profile.Value().supportedFeatures.Contains(DestructionFeature::PreCookedFracture))
                return false;
            return limits.maximumChunksPerDestructible != 0 &&
                   limits.maximumChunksPerDestructible <= DestructionHardLimits::ChunksPerDestructible &&
                   limits.maximumChunksPerDestructible <= profile.Value().limits.maximumChunksPerDestructible &&
                   limits.maximumArtifactBytes != 0 && limits.maximumArtifactBytes <= DestructionHardLimits::ArtifactBytes &&
                   limits.maximumArtifactBytes <= profile.Value().limits.maximumArtifactBytes &&
                   limits.maximumTransitionBytes >= limits.maximumArtifactBytes &&
                   limits.maximumTransitionBytes <= DestructionHardLimits::TransitionBytes &&
                   limits.maximumTransitionBytes <= profile.Value().limits.maximumTransitionBytes &&
                   limits.maximumResidentBytes >= limits.maximumTransitionBytes &&
                   limits.maximumResidentBytes <= DestructionHardLimits::ResidentBytes &&
                   limits.maximumResidentBytes <= profile.Value().limits.maximumResidentBytes &&
                   limits.maximumWorkItemsPerTransition != 0 &&
                   limits.maximumWorkItemsPerTransition <= DestructionHardLimits::WorkItemsPerTransition &&
                   limits.maximumWorkItemsPerTransition <= profile.Value().limits.maximumWorkItemsPerTransition;
        }

        struct Budget final {
            std::uint64_t bytes{};
            std::uint64_t work{};
            const DestructionLimits &limits;

            [[nodiscard]] bool Charge(std::uint64_t addedBytes, std::uint64_t addedWork) {
                if (bytes > limits.maximumArtifactBytes || addedBytes > limits.maximumArtifactBytes - bytes ||
                    work > limits.maximumWorkItemsPerTransition || addedWork > limits.maximumWorkItemsPerTransition - work)
                    return false;
                bytes += addedBytes;
                work += addedWork;
                return true;
            }
        };

        using InteriorPair = std::pair<DestructionChunkId, DestructionChunkId>;

        struct InteriorArea final {
            double low{};
            double high{};
        };

        [[nodiscard]] Result<void> CheckInterior(const OfflineVoronoiChunk &source, const std::map<DestructionChunkId, Point> &sites,
                                                 const Point &a, const Point &b, const Point &c, const Point &normal, double area,
                                                 std::map<InteriorPair, InteriorArea> &areas) {
            DestructionChunkId match{};
            for (const auto neighbor : source.neighbors) {
                const auto it = sites.find(neighbor);
                if (it == sites.end() || neighbor == source.id)
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
                const Point middle{(source.site[0] + it->second[0]) * 0.5, (source.site[1] + it->second[1]) * 0.5,
                                   (source.site[2] + it->second[2]) * 0.5};
                const Point direction = Sub(it->second, source.site);
                const double distance = std::sqrt(Dot(direction, direction));
                if (!(distance > 1.0e-9))
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
                const double tolerance = 2.0e-4 * std::max(1.0, distance);
                if (std::abs(Dot(Sub(a, middle), direction) / distance) < tolerance &&
                    std::abs(Dot(Sub(b, middle), direction) / distance) < tolerance &&
                    std::abs(Dot(Sub(c, middle), direction) / distance) < tolerance) {
                    if (Dot(normal, direction) <= 0)
                        return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
                    if (match.IsValid())
                        return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
                    match = neighbor;
                }
            }
            if (!match.IsValid())
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
            auto &pair = areas[std::minmax(source.id, match)];
            if (source.id < match)
                pair.low += area;
            else
                pair.high += area;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendFace(const OfflineVoronoiChunk &source, const OfflineVoronoiTriangle &face, const ChunkUvPolicy uv,
                                              const std::map<DestructionChunkId, Point> &sites, std::map<InteriorPair, InteriorArea> &areas,
                                              ChunkMesh &target) {
            if (std::ranges::any_of(face.indices, [&](std::uint32_t index) {
                return index >= source.positions.size();
            }))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            const Point a = Position(source.positions[face.indices[0]]);
            const Point b = Position(source.positions[face.indices[1]]);
            const Point c = Position(source.positions[face.indices[2]]);
            if (!Finite(a) || !Finite(b) || !Finite(c))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            const Point cross = Cross(Sub(b, a), Sub(c, a));
            const double length = std::sqrt(Dot(cross, cross));
            if (!(length > 1.0e-12) || !std::isfinite(length))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            if (face.interior && !sites.empty()) {
                auto checked = CheckInterior(source, sites, a, b, c, cross, length * 0.5, areas);
                if (checked.HasError())
                    return checked;
            }
            const Point normal{cross[0] / length, cross[1] / length, cross[2] / length};
            const Point reference = std::abs(normal[0]) < 0.8 ? Point{1, 0, 0} : Point{0, 0, 1};
            const Point projected = Sub(reference, Point{normal[0] * Dot(reference, normal), normal[1] * Dot(reference, normal),
                                                         normal[2] * Dot(reference, normal)});
            const double tangentLength = std::sqrt(Dot(projected, projected));
            const Point tangent{projected[0] / tangentLength, projected[1] / tangentLength, projected[2] / tangentLength};
            const Point bitangent = Cross(normal, tangent);
            const double scale = face.interior ? uv.interiorScale : uv.exteriorScale;
            ChunkMeshFace outputFace{{}, face.materialSlot, face.interior};
            for (const Point &point : {a, b, c}) {
                const double u = Dot(point, tangent) * scale;
                const double v = Dot(point, bitangent) * scale;
                if (!std::isfinite(u) || !std::isfinite(v) || std::abs(u) > std::numeric_limits<float>::max() ||
                    std::abs(v) > std::numeric_limits<float>::max())
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
                const auto index = static_cast<std::uint32_t>(target.vertices.size());
                outputFace.indices[index % 3U] = index;
                target.vertices.push_back(
                    {{static_cast<float>(point[0]), static_cast<float>(point[1]), static_cast<float>(point[2])},
                     {static_cast<float>(normal[0]), static_cast<float>(normal[1]), static_cast<float>(normal[2])},
                     {static_cast<float>(tangent[0]), static_cast<float>(tangent[1]), static_cast<float>(tangent[2]), 1.0F},
                     {static_cast<float>(u), static_cast<float>(v)}});
            }
            target.faces.push_back(outputFace);
            return Result<void>::Success();
        }
    }  // namespace

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

    /** @copydoc CookChunkMeshes */
    Result<std::shared_ptr<const ChunkMeshArtifact>> CookChunkMeshes(const OfflineVoronoiCandidate &source,
                                                                     FractureArtifactContentIdentity content,
                                                                     std::span<const ChunkMaterialBinding> materials, ChunkUvPolicy uv,
                                                                     const DestructionLimits &limits,
                                                                     const CancellationToken &cancellation) {
        using Output = Result<std::shared_ptr<const ChunkMeshArtifact>>;
        if (cancellation.IsCancellationRequested())
            return Output::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
        if (!content.IsValid() || !source.IsIntact() || !source.sourceAsset.IsValid() || source.sourceRevision == 0 ||
            source.recipeId == 0 || source.recipeRevision == 0 || !Nonzero(source.sourceDigest) || !Nonzero(source.semanticFingerprint) ||
            source.schemaVersion != OfflineVoronoiSchemaVersion || source.chunks.empty() || !std::isfinite(uv.exteriorScale) ||
            !std::isfinite(uv.interiorScale) || uv.exteriorScale <= 0.0 || uv.interiorScale <= 0.0)
            return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
        if (!ValidLimits(limits, source.tier) || source.chunks.size() > limits.maximumChunksPerDestructible)
            return Output::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
        if (content.SemanticDigest() != ComputeChunkMeshSemanticDigest(source, materials, uv))
            return Output::Failure(MakeError(ChunkMeshCookErrors::Stale));
        for (std::size_t i = 0; i < materials.size(); ++i) {
            if (!materials[i].asset.IsValid() || !Nonzero(materials[i].revisionDigest) ||
                (i != 0 && materials[i - 1].slot >= materials[i].slot))
                return Output::Failure(MakeError(ChunkMeshCookErrors::MissingMaterial));
        }
        std::map<DestructionChunkId, Point> sites;
        for (const auto &chunk : source.chunks) {
            if (!chunk.id.IsValid() || !Finite(chunk.site) || !sites.emplace(chunk.id, chunk.site).second)
                return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
        }
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
        Budget budget{sizeof(ChunkMeshArtifact), 0, limits};
        if (!budget.Charge(materials.size() * sizeof(ChunkMaterialBinding), materials.size()))
            return Output::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
        std::map<InteriorPair, InteriorArea> areas;
        for (const auto &chunk : source.chunks) {
            if (cancellation.IsCancellationRequested())
                return Output::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
            if (chunk.triangles.empty() || chunk.positions.empty() || !std::isfinite(chunk.volume) || chunk.volume <= 0.0 ||
                !Finite(chunk.centerOfMass))
                return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            if (!std::ranges::is_sorted(chunk.neighbors) || std::ranges::adjacent_find(chunk.neighbors) != chunk.neighbors.end())
                return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
            for (const auto neighbor : chunk.neighbors) {
                if (sites.find(neighbor) == sites.end() || neighbor == chunk.id)
                    return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
                areas.try_emplace(std::minmax(chunk.id, neighbor));
            }
            if (!budget.Charge(sizeof(ChunkMesh) + chunk.triangles.size() * (sizeof(ChunkMeshFace) + 3 * sizeof(ChunkMeshVertex)),
                               chunk.positions.size() + chunk.triangles.size() * (3 + chunk.neighbors.size())))
                return Output::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            ChunkMesh output;
            output.id = chunk.id;
            output.vertices.reserve(chunk.triangles.size() * 3);
            output.faces.reserve(chunk.triangles.size());
            output.mass.volume = chunk.volume;
            for (std::size_t axis = 0; axis < 3; ++axis)
                output.mass.firstMoment[axis] = chunk.volume * chunk.centerOfMass[axis];
            for (const auto &face : chunk.triangles) {
                if (cancellation.IsCancellationRequested())
                    return Output::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
                const auto material = std::ranges::lower_bound(materials, face.materialSlot, {}, &ChunkMaterialBinding::slot);
                if (material == materials.end() || material->slot != face.materialSlot)
                    return Output::Failure(MakeError(ChunkMeshCookErrors::MissingMaterial));
                auto appended = AppendFace(chunk, face, uv, sites, areas, output);
                if (appended.HasError())
                    return Output::Failure(appended.ErrorValue());
            }
            if (output.vertices.empty())
                return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            output.mass.minimum = output.vertices.front().position;
            output.mass.maximum = output.mass.minimum;
            for (const auto &vertex : output.vertices) {
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    output.mass.minimum[axis] = std::min(output.mass.minimum[axis], vertex.position[axis]);
                    output.mass.maximum[axis] = std::max(output.mass.maximum[axis], vertex.position[axis]);
                }
            }
            artifact->chunks.push_back(std::move(output));
        }
        for (const auto &[key, area] : areas) {
            (void)key;
            if (!(area.low > 0.0) || !(area.high > 0.0) || std::abs(area.low - area.high) > 1.0e-3 * std::max(area.low, area.high))
                return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
        }
        artifact->estimatedBytes = budget.bytes;
        artifact->workItems = budget.work;
        artifact->integrityDigest = ArtifactDigest(*artifact);
        return Output::Success(std::move(artifact));
    }

    /** @copydoc CookPreFracturedChunkMeshes */
    Result<std::shared_ptr<const ChunkMeshArtifact>> CookPreFracturedChunkMeshes(
        const PreFracturedCandidate &source, Assets::AssetId sourceAsset, std::uint64_t sourceRevision, Sha256Digest sourceDigest,
        FractureArtifactContentIdentity content, DestructionFeatureTier tier, std::span<const ImportedChunkMaterialBinding> materials,
        ChunkUvPolicy uv, const DestructionLimits &limits, const CancellationToken &cancellation) {
        using Output = Result<std::shared_ptr<const ChunkMeshArtifact>>;
        if (cancellation.IsCancellationRequested())
            return Output::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
        if (!sourceAsset.IsValid() || sourceRevision == 0 || !content.IsValid() ||
            source.SchemaVersion() != CurrentPreFracturedImportSchemaVersion || source.Chunks().empty() ||
            !std::isfinite(uv.exteriorScale) || !std::isfinite(uv.interiorScale) || uv.exteriorScale <= 0 || uv.interiorScale <= 0)
            return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
        if (!ValidLimits(limits, tier) || source.Chunks().size() > limits.maximumChunksPerDestructible)
            return Output::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
        if (sourceDigest != ComputePreFracturedMeshSourceDigest(source) ||
            content.SemanticDigest() != ComputePreFracturedMeshSemanticDigest(source, sourceAsset, sourceRevision, tier, materials, uv))
            return Output::Failure(MakeError(ChunkMeshCookErrors::Stale));
        std::map<std::string, ImportedChunkMaterialBinding> mappings;
        std::set<std::uint32_t> slots;
        for (const auto &binding : materials) {
            if (binding.sourceName.empty() || !binding.material.asset.IsValid() || !Nonzero(binding.material.revisionDigest) ||
                !mappings.emplace(binding.sourceName, binding).second || !slots.insert(binding.material.slot).second)
                return Output::Failure(MakeError(ChunkMeshCookErrors::MissingMaterial));
        }
        auto artifact = std::shared_ptr<ChunkMeshArtifact>(new ChunkMeshArtifact);
        artifact->content = content;
        artifact->sourceAsset = sourceAsset;
        artifact->sourceRevision = sourceRevision;
        artifact->sourceDigest = sourceDigest;
        artifact->inputFingerprint = sourceDigest;
        artifact->tier = tier;
        artifact->producedFeatures.bits = DestructionFeatureBit<DestructionFeature::PreCookedFracture>;
        artifact->materials.reserve(materials.size());
        for (const auto &binding : materials)
            artifact->materials.push_back(binding.material);
        std::ranges::sort(artifact->materials, {}, &ChunkMaterialBinding::slot);
        artifact->materialFingerprint = MaterialDigest(artifact->materials);
        Budget budget{sizeof(ChunkMeshArtifact), 0, limits};
        std::uint64_t materialBytes = 0;
        for (const auto &binding : materials)
            materialBytes += sizeof(binding) + binding.sourceName.size();
        if (!budget.Charge(materialBytes, materials.size()))
            return Output::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
        using TriangleKey = std::array<std::array<float, 3>, 3>;

        struct CutFace final {
            DestructionChunkId chunk;
            Point normal;
            bool interior{};
            std::uint32_t count{};
        };

        std::map<TriangleKey, CutFace> cuts;
        const std::map<DestructionChunkId, Point> noSites;
        std::map<InteriorPair, InteriorArea> noAreas;
        DestructionChunkId previous{};
        for (const auto &chunk : source.Chunks()) {
            if (cancellation.IsCancellationRequested())
                return Output::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
            if (!chunk.id.IsValid() || (previous.IsValid() && chunk.id <= previous) || chunk.positions.empty() ||
                chunk.triangleIndices.empty() || chunk.triangleIndices.size() % 3 != 0 ||
                chunk.triangleMaterials.size() != chunk.triangleIndices.size() / 3)
                return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            previous = chunk.id;
            const auto triangleCount = chunk.triangleIndices.size() / 3;
            if (!budget.Charge(sizeof(ChunkMesh) + triangleCount * (sizeof(ChunkMeshFace) + 3 * sizeof(ChunkMeshVertex)),
                               chunk.positions.size() + triangleCount * 6))
                return Output::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            ChunkMesh output;
            output.id = chunk.id;
            output.vertices.reserve(triangleCount * 3);
            output.faces.reserve(triangleCount);
            double sixVolume{};
            Point weightedCenter{};
            for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
                if (cancellation.IsCancellationRequested())
                    return Output::Failure(MakeError(ChunkMeshCookErrors::Cancelled));
                const auto found = mappings.find(chunk.triangleMaterials[triangle]);
                if (found == mappings.end())
                    return Output::Failure(MakeError(ChunkMeshCookErrors::MissingMaterial));
                const auto offset = triangle * 3;
                OfflineVoronoiTriangle face{{chunk.triangleIndices[offset], chunk.triangleIndices[offset + 1],
                                             chunk.triangleIndices[offset + 2]},
                                            found->second.material.slot,
                                            found->second.interior};
                if (std::ranges::any_of(face.indices, [&](std::uint32_t index) {
                    return index >= chunk.positions.size();
                }))
                    return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
                const Point a = Position(chunk.positions[face.indices[0]]);
                const Point b = Position(chunk.positions[face.indices[1]]);
                const Point c = Position(chunk.positions[face.indices[2]]);
                if (!Finite(a) || !Finite(b) || !Finite(c))
                    return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
                const Point normal = Cross(Sub(b, a), Sub(c, a));
                const double v6 = Dot(a, normal);
                if (!std::isfinite(v6))
                    return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
                sixVolume += v6;
                for (std::size_t axis = 0; axis < 3; ++axis)
                    weightedCenter[axis] += (a[axis] + b[axis] + c[axis]) * v6;
                TriangleKey key{chunk.positions[face.indices[0]], chunk.positions[face.indices[1]], chunk.positions[face.indices[2]]};
                std::ranges::sort(key);
                auto [it, inserted] = cuts.try_emplace(key, CutFace{chunk.id, normal, face.interior, 1});
                if (!inserted) {
                    if (it->second.count != 1 || it->second.chunk == chunk.id || !it->second.interior || !face.interior ||
                        Dot(it->second.normal, normal) >= 0)
                        return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
                    it->second.count = 2;
                }
                OfflineVoronoiChunk faceSource;
                faceSource.positions = {chunk.positions[face.indices[0]], chunk.positions[face.indices[1]],
                                        chunk.positions[face.indices[2]]};
                face.indices = {0, 1, 2};
                auto appended = AppendFace(faceSource, face, uv, noSites, noAreas, output);
                if (appended.HasError())
                    return Output::Failure(appended.ErrorValue());
            }
            output.mass.volume = sixVolume / 6.0;
            if (!(output.mass.volume > 0.0) || !std::isfinite(output.mass.volume))
                return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            for (std::size_t axis = 0; axis < 3; ++axis)
                output.mass.firstMoment[axis] = weightedCenter[axis] / 24.0;
            output.mass.minimum = output.vertices.front().position;
            output.mass.maximum = output.mass.minimum;
            for (const auto &vertex : output.vertices) {
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    output.mass.minimum[axis] = std::min(output.mass.minimum[axis], vertex.position[axis]);
                    output.mass.maximum[axis] = std::max(output.mass.maximum[axis], vertex.position[axis]);
                }
            }
            artifact->chunks.push_back(std::move(output));
        }
        for (const auto &[key, cut] : cuts) {
            (void)key;
            if (cut.interior && cut.count != 2)
                return Output::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
        }
        artifact->estimatedBytes = budget.bytes;
        artifact->workItems = budget.work;
        artifact->integrityDigest = ArtifactDigest(*artifact);
        return Output::Success(std::move(artifact));
    }

    /** @copydoc ChunkMeshCookOwner::Accept */
    Result<void> ChunkMeshCookOwner::Accept(std::shared_ptr<const ChunkMeshArtifact> candidate, std::uint64_t expectedRevision,
                                            FractureArtifactContentIdentity currentContent) {
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
