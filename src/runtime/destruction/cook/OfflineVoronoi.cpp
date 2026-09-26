#include "Horo/Destruction/OfflineVoronoi.h"

#include "OfflineVoronoiInternal.h"
#include "OfflineVoronoiProvenance.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <numbers>
#include <utility>

namespace Horo::Destruction::OfflineVoronoiErrors {
    namespace {
        const ErrorDomainId kDomain{"horo.destruction"};
        constexpr auto kSeverity = ErrorSeverity::Error;
    }  // namespace

    const ErrorCodeDescriptor InvalidInput{kDomain, ErrorCode{"destruction.voronoi.invalid_input"}, kSeverity,
                                           "Offline Voronoi input or provenance is invalid.",
                                           "Capture a valid immutable source and recipe."};
    const ErrorCodeDescriptor InvalidMesh{kDomain, ErrorCode{"destruction.voronoi.invalid_mesh"}, kSeverity,
                                          "Offline Voronoi source must be a closed, consistently wound mesh.",
                                          "Repair the source topology before generation."};
    const ErrorCodeDescriptor InvalidSites{kDomain, ErrorCode{"destruction.voronoi.invalid_sites"}, kSeverity,
                                           "Voronoi sites are duplicate, outside the source, or cannot be placed.",
                                           "Choose finite distinct sites inside the source or change the seed."};
    const ErrorCodeDescriptor LimitExceeded{kDomain, ErrorCode{"destruction.voronoi.limit_exceeded"}, kSeverity,
                                            "Offline Voronoi work or output exceeds explicit limits.",
                                            "Reduce source complexity or site count, or select a supported larger profile."};
    const ErrorCodeDescriptor Cancelled{kDomain, ErrorCode{"destruction.voronoi.cancelled"}, kSeverity,
                                        "Offline Voronoi generation was cancelled.", "Retry from the current source and recipe."};
    const ErrorCodeDescriptor Stale{kDomain, ErrorCode{"destruction.voronoi.stale"}, kSeverity,
                                    "Voronoi candidate refers to replaced source or recipe content.",
                                    "Regenerate from the current revision."};
    const ErrorCodeDescriptor Shutdown{kDomain, ErrorCode{"destruction.voronoi.shutdown"}, kSeverity,
                                       "Voronoi owner has closed candidate acceptance.", "Do not publish after shutdown."};
}  // namespace Horo::Destruction::OfflineVoronoiErrors

namespace Horo::Destruction {
    namespace {
        using namespace VoronoiDetail;

        [[nodiscard]] Result<void> ValidateGenerationInput(const OfflineVoronoiSource &source, const OfflineVoronoiRecipe &recipe,
                                                           const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Cancelled));
            if (!source.asset.IsValid() || source.revision == 0 || recipe.id == 0 || recipe.revision == 0 || recipe.toolchainVersion == 0 ||
                !NonzeroDigest(recipe.toolchainDigest) || recipe.siteIds.size() != recipe.siteCount ||
                (!recipe.sites.empty() && recipe.sites.size() != recipe.siteCount))
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidInput));
            if (!ValidLimits(recipe))
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            std::vector<DestructionChunkId> sortedIds = recipe.siteIds;
            std::sort(sortedIds.begin(), sortedIds.end());
            if (std::any_of(sortedIds.begin(), sortedIds.end(),
                            [](const DestructionChunkId id) {
                return !id.IsValid();
            }) ||
                std::adjacent_find(sortedIds.begin(), sortedIds.end()) != sortedIds.end())
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidInput));
            if (source.positions.size() > recipe.maximumVertices || source.indices.size() / 3 > recipe.maximumTriangles ||
                source.positions.size() > recipe.limits.maximumArtifactBytes / sizeof(std::array<float, 3>) ||
                source.indices.size() > recipe.limits.maximumArtifactBytes / sizeof(std::uint32_t) ||
                source.materialSlots.size() > recipe.limits.maximumArtifactBytes / sizeof(std::uint32_t))
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            if (source.positions.size() > recipe.maximumWorkItems ||
                source.indices.size() > recipe.maximumWorkItems - source.positions.size() ||
                source.materialSlots.size() > recipe.maximumWorkItems - source.positions.size() - source.indices.size())
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
            if (source.digest != ComputeOfflineVoronoiSourceDigest(source))
                return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidInput));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<OfflineVoronoiChunk> GenerateSiteChunk(const std::uint32_t index, const std::vector<Point> &sites,
                                                                    const std::vector<std::vector<Face>> &regions,
                                                                    const std::vector<Face> &sourceFaces,
                                                                    const OfflineVoronoiRecipe &recipe, Budget &budget,
                                                                    const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<OfflineVoronoiChunk>::Failure(MakeError(OfflineVoronoiErrors::Cancelled));
            OfflineVoronoiChunk aggregate;
            aggregate.id = recipe.siteIds[index];
            aggregate.site = sites[index];
            for (const auto &region : regions) {
                auto faces = region;
                for (std::uint32_t other = 0; other < sites.size(); ++other) {
                    if (other == index)
                        continue;
                    auto clipped = Clip(faces, sites[index], sites[other], other + 1U, recipe.interiorMaterialSlot, budget, cancellation);
                    if (clipped.HasError())
                        return Result<OfflineVoronoiChunk>::Failure(clipped.ErrorValue());
                    if (faces.empty())
                        break;
                }
                if (faces.empty())
                    continue;
                auto chunk = MakeChunk(faces, sites[index], recipe.siteIds, index, budget);
                if (chunk.HasError())
                    return chunk;
                MergeChunk(aggregate, std::move(chunk.Value()));
            }
            if (!(aggregate.volume > 0.0))
                return Result<OfflineVoronoiChunk>::Failure(MakeError(OfflineVoronoiErrors::InvalidSites));
            if (auto exterior = AppendExterior(aggregate, sourceFaces, sites, index, budget, cancellation); exterior.HasError())
                return Result<OfflineVoronoiChunk>::Failure(exterior.ErrorValue());
            std::sort(aggregate.neighbors.begin(), aggregate.neighbors.end());
            aggregate.neighbors.erase(std::unique(aggregate.neighbors.begin(), aggregate.neighbors.end()), aggregate.neighbors.end());
            return Result<OfflineVoronoiChunk>::Success(std::move(aggregate));
        }

        [[nodiscard]] Result<void> ValidateNeighbors(const std::vector<OfflineVoronoiChunk> &chunks) {
            for (const auto &chunk : chunks) {
                for (const auto neighbor : chunk.neighbors) {
                    const auto other = std::find_if(chunks.begin(), chunks.end(), [&](const auto &candidateChunk) {
                        return candidateChunk.id == neighbor;
                    });
                    if (other == chunks.end() || !std::binary_search(other->neighbors.begin(), other->neighbors.end(), chunk.id))
                        return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidMesh));
                }
            }
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc GenerateOfflineVoronoi */
    Result<OfflineVoronoiCandidate> GenerateOfflineVoronoi(const OfflineVoronoiSource &source, const OfflineVoronoiRecipe &recipe,
                                                           const CancellationToken &cancellation) {
        if (auto validated = ValidateGenerationInput(source, recipe, cancellation); validated.HasError())
            return Result<OfflineVoronoiCandidate>::Failure(validated.ErrorValue());
        Budget budget{.workLimit = recipe.maximumWorkItems,
                      .byteLimit = recipe.limits.maximumArtifactBytes,
                      .vertexLimit = recipe.maximumVertices,
                      .triangleLimit = recipe.maximumTriangles};
        if (!budget.ChargeBytes(sizeof(OfflineVoronoiCandidate) +
                                static_cast<std::uint64_t>(recipe.siteCount) * sizeof(OfflineVoronoiChunk)))
            return Result<OfflineVoronoiCandidate>::Failure(MakeError(OfflineVoronoiErrors::LimitExceeded));
        auto sourceFaces = ValidateSource(source, budget, cancellation);
        if (sourceFaces.HasError())
            return Result<OfflineVoronoiCandidate>::Failure(sourceFaces.ErrorValue());
        auto sites = Sites(source, recipe, sourceFaces.Value(), budget, cancellation);
        if (sites.HasError())
            return Result<OfflineVoronoiCandidate>::Failure(sites.ErrorValue());
        std::vector<std::vector<Face>> regions;
        if (ConvexSource(source, sourceFaces.Value(), 1.0e-8, budget)) {
            regions.push_back(sourceFaces.Value());
            for (auto &face : regions.front())
                face.visible = false;
        } else {
            auto decomposed = ConvexRegions(source, sourceFaces.Value(), recipe, budget, cancellation);
            if (decomposed.HasError())
                return Result<OfflineVoronoiCandidate>::Failure(decomposed.ErrorValue());
            regions = std::move(decomposed.Value());
        }
        OfflineVoronoiCandidate candidate;
        candidate.sourceAsset = source.asset;
        candidate.sourceRevision = source.revision;
        candidate.sourceDigest = source.digest;
        candidate.recipeId = recipe.id;
        candidate.recipeRevision = recipe.revision;
        candidate.semanticFingerprint = Detail::VoronoiFingerprint(source, recipe, sites.Value());
        candidate.toolchainDigest = recipe.toolchainDigest;
        candidate.chunks.reserve(sites.Value().size());
        for (std::uint32_t index = 0; index < sites.Value().size(); ++index) {
            auto chunk = GenerateSiteChunk(index, sites.Value(), regions, sourceFaces.Value(), recipe, budget, cancellation);
            if (chunk.HasError())
                return Result<OfflineVoronoiCandidate>::Failure(chunk.ErrorValue());
            candidate.chunks.push_back(std::move(chunk.Value()));
        }
        if (auto neighbors = ValidateNeighbors(candidate.chunks); neighbors.HasError())
            return Result<OfflineVoronoiCandidate>::Failure(neighbors.ErrorValue());
        candidate.estimatedBytes = budget.bytes;
        candidate.workItems = budget.work;
        candidate.outputChecksum_ = Detail::VoronoiOutputChecksum(candidate);
        return Result<OfflineVoronoiCandidate>::Success(std::move(candidate));
    }

    /** @copydoc OfflineVoronoiOwner::Accept */
    Result<void> OfflineVoronoiOwner::Accept(OfflineVoronoiCandidate candidate, const std::uint64_t expectedOwnerRevision,
                                             const OfflineVoronoiSource &currentSource, const OfflineVoronoiRecipe &currentRecipe) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Shutdown));
        if (candidate.outputChecksum_ != Detail::VoronoiOutputChecksum(candidate))
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::InvalidInput));
        if (!ValidLimits(currentRecipe) || currentRecipe.siteIds.size() != currentRecipe.siteCount ||
            (!currentRecipe.sites.empty() && currentRecipe.sites.size() != currentRecipe.siteCount))
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Stale));
        if (expectedOwnerRevision != revision_ || candidate.sourceAsset != currentSource.asset ||
            candidate.sourceRevision != currentSource.revision || candidate.sourceDigest != currentSource.digest ||
            candidate.recipeId != currentRecipe.id || candidate.recipeRevision != currentRecipe.revision ||
            candidate.toolchainDigest != currentRecipe.toolchainDigest || candidate.schemaVersion != OfflineVoronoiSchemaVersion ||
            candidate.sourceDigest != ComputeOfflineVoronoiSourceDigest(currentSource) ||
            candidate.chunks.size() != currentRecipe.siteCount)
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Stale));
        std::vector<Point> sites;
        sites.reserve(candidate.chunks.size());
        for (const auto &chunk : candidate.chunks)
            sites.push_back(chunk.site);
        if (candidate.semanticFingerprint != Detail::VoronoiFingerprint(currentSource, currentRecipe, sites) ||
            (!currentRecipe.sites.empty() && currentRecipe.sites != sites))
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Stale));
        if (revision_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(DestructionErrors::RevisionExhausted));
        CancellationSource nextCancellation;
        auto next = std::make_shared<const OfflineVoronoiCandidate>(std::move(candidate));
        cancellation_.RequestCancellation();
        current_ = std::move(next);
        cancellation_ = std::move(nextCancellation);
        ++revision_;
        return Result<void>::Success();
    }

    /** @copydoc OfflineVoronoiOwner::Invalidate */
    Result<void> OfflineVoronoiOwner::Invalidate() {
        if (shutdown_)
            return Result<void>::Failure(MakeError(OfflineVoronoiErrors::Shutdown));
        if (revision_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(DestructionErrors::RevisionExhausted));
        CancellationSource nextCancellation;
        cancellation_.RequestCancellation();
        cancellation_ = std::move(nextCancellation);
        ++revision_;
        return Result<void>::Success();
    }

    /** @copydoc OfflineVoronoiOwner::Shutdown */
    void OfflineVoronoiOwner::Shutdown() noexcept {
        shutdown_ = true;
        cancellation_.RequestCancellation();
    }
}  // namespace Horo::Destruction
