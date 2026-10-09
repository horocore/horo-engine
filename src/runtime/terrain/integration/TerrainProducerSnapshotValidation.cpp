#include "TerrainProducerSnapshotInternal.h"

#include <tuple>

namespace Horo::Terrain::ProducerDetail {
    namespace {
        /** @brief Validates the independent typed correlation dimensions, not their current authority state. */
        bool ValidCorrelation(const TerrainProducerSnapshotHeader &header) noexcept {
            return header.schema == 1 && header.consumer < TerrainProducerConsumer::Count && header.terrain.IsValid() &&
                   header.revision.IsValid() && header.request.IsValid() && header.world.IsValid() && header.origin.IsValid() &&
                   header.capabilities.IsValid();
        }

        /** @brief Standalone absence is explicit; a cell fence must belong to the supplied world. */
        bool ValidScope(const TerrainProducerSnapshotHeader &header) noexcept {
            return !header.cell || (header.cell->IsValid() && header.cell->partition == header.world);
        }

        /** @brief Rejects malformed or mirrored authored placement; canonical producer geometry is not transformed here. */
        bool ValidPlacement(const Math::Transform &transform) {
            return transform.TryToMatrix().HasValue() && transform.scale.x > 0 && transform.scale.y > 0 && transform.scale.z > 0;
        }

        /** @brief A finite reservation cannot silently mean unlimited. */
        bool Within(const std::uint64_t value, const std::uint64_t ceiling) noexcept {
            return value > 0 && value <= ceiling;
        }

        /** @brief Validates all independent project-lowered ceilings before allocation or hashing. */
        bool ValidLimits(const TerrainProducerSnapshotLimits &limits) noexcept {
            return Within(limits.maximumMeshes, 4'096) && Within(limits.maximumClusters, 4'096) &&
                   Within(limits.maximumInstances, 2'097'152) && Within(limits.maximumVertices, 1'048'576) &&
                   Within(limits.maximumTriangles, 2'097'152) && Within(limits.maximumOwnedBytes, 128ULL * 1024 * 1024) &&
                   Within(limits.maximumWorkItems, 8'388'608);
        }

        /** @brief Preserves closed-before-cancelled lifecycle precedence and rejects unknown lifecycle values. */
        Result<void> ValidateLifecycle(const TerrainProducerSnapshotRequest &request, const CancellationToken &cancellation) {
            if (request.lifecycle == TerrainRuntimeLifecycle::Closing || request.lifecycle == TerrainRuntimeLifecycle::Closed)
                return Result<void>::Failure(Failure(request, TerrainProducerErrors::Closed));
            if (request.lifecycle == TerrainRuntimeLifecycle::Cancelled || cancellation.IsCancellationRequested())
                return Result<void>::Failure(Failure(request, TerrainProducerErrors::Cancelled));
            if (request.lifecycle != TerrainRuntimeLifecycle::Active)
                return Result<void>::Failure(Failure(request, TerrainProducerErrors::Invalid));
            return Result<void>::Success();
        }

        /** @brief Establishes safe complete correlation and bounded envelope before reading the manifest. */
        Result<void> ValidateEnvelope(const TerrainProducerSnapshotRequest &request) {
            if (!ValidCorrelation(request.header) || !ValidScope(request.header) || !ValidPlacement(request.header.datasetToWorld) ||
                !request.manifest || request.header.targetDigest == Sha256Digest{} || request.header.manifestDigest == Sha256Digest{} ||
                !ValidLimits(request.limits))
                return Result<void>::Failure(Failure(request, TerrainProducerErrors::Invalid));
            return Result<void>::Success();
        }

        /** @brief Every nonempty selection requires its exact root; count admission precedes root lookup. */
        Result<void> ValidateSelections(const TerrainProducerSnapshotRequest &request) {
            if (request.tiles.size() > request.limits.maximumMeshes || request.clusters.size() > request.limits.maximumClusters ||
                request.definitions.size() > 4'096)
                return Result<void>::Failure(Failure(request, TerrainProducerErrors::Limit));
            if ((!request.tiles.empty() && !request.terrain) || (!request.clusters.empty() && !request.foliage))
                return Result<void>::Failure(Failure(request, TerrainProducerErrors::Invalid));
            return Result<void>::Success();
        }

        /** @brief Requires exact consumer capability; visual or another consumer role never substitutes. */
        Result<void> ValidateCapabilities(const TerrainProducerSnapshotRequest &request) {
            using enum TerrainFoliageCapability;
            if (const auto capability =
                    request.header.consumer == TerrainProducerConsumer::Collision ? PhysicsCollision : NavigationBlocking;
                !request.header.capabilities.Contains(TerrainRuntime) || !request.header.capabilities.Contains(capability) ||
                (!request.clusters.empty() && !request.header.capabilities.Contains(FoliageRuntime)))
                return Result<void>::Failure(Failure(request, TerrainProducerErrors::Unavailable));
            return Result<void>::Success();
        }

        /** @brief Compares complete terrain root provenance as one exact binding, without constructing a competing manifest model. */
        bool MatchesTerrainRoot(const CookedTerrainSourceArtifacts &root, const TerrainPayloadProvenance &provenance) {
            const auto &tiles = root.Tiles();
            return std::tuple{tiles.dataset,      root.Capability(),         root.Fingerprint(), root.ManifestDigest(),
                              tiles.fingerprint,  tiles.manifestDigest,      tiles.sourceAsset,  tiles.sourceRevision,
                              tiles.sourceDigest, tiles.profile.targetDigest} == std::tuple{provenance.dataset,
                                                                                            provenance.capability,
                                                                                            provenance.geometryFingerprint,
                                                                                            provenance.geometryManifestDigest,
                                                                                            provenance.tileFingerprint,
                                                                                            provenance.tileManifestDigest,
                                                                                            provenance.sourceAsset,
                                                                                            provenance.source,
                                                                                            provenance.sourceDigest,
                                                                                            provenance.targetDigest};
        }

        /** @brief Compares the exact foliage generation and cook roots, never just a matching tile address. */
        bool MatchesFoliageRoot(const CookedFoliageClusterSet &root, const TerrainPayloadProvenance &provenance) {
            return std::tuple{root.Dataset(),
                              root.ContentRevision(),
                              root.Fingerprint(),
                              root.ManifestDigest(),
                              root.Profile().configuration.Data().capability,
                              root.Profile().targetDigest} == std::tuple{provenance.dataset,
                                                                         provenance.content,
                                                                         provenance.foliageFingerprint,
                                                                         provenance.foliageManifestDigest,
                                                                         provenance.capability,
                                                                         provenance.targetDigest};
        }

        /** @brief Checks exact roots without scanning unselected payloads or treating a cache hit as current authority. */
        Result<void> ValidateRoots(const TerrainProducerSnapshotRequest &request) {
            const auto &header = request.header;
            const auto &provenance = request.manifest->Provenance();
            if (request.manifest->Digest() != header.manifestDigest || provenance.dataset != header.terrain.dataset ||
                provenance.content != header.revision.content || provenance.capability != header.revision.capability ||
                provenance.targetDigest != header.targetDigest)
                return Result<void>::Failure(Failure(request, TerrainProducerErrors::Stale));
            if (request.terrain && !MatchesTerrainRoot(*request.terrain, provenance))
                return Result<void>::Failure(Failure(request, TerrainProducerErrors::Stale));
            if (request.foliage && !MatchesFoliageRoot(*request.foliage, provenance))
                return Result<void>::Failure(Failure(request, TerrainProducerErrors::Stale));
            return Result<void>::Success();
        }

        /** @brief Maps only the already validated consumer to its canonical neutral surface role. */
        TerrainSourceArtifactRole Role(const TerrainProducerSnapshotRequest &request) noexcept {
            return request.header.consumer == TerrainProducerConsumer::Collision ? TerrainSourceArtifactRole::Collision
                                                                                 : TerrainSourceArtifactRole::Navigation;
        }

        /** @brief Resolves exact consumer membership in the authoritative manifest before any cook-root selection. */
        Result<const TerrainTilePayloadEntry *> TileMembership(const TerrainProducerSnapshotRequest &request,
                                                               const TerrainProducerTileSelection &selection, Budget &budget,
                                                               const CancellationToken &cancellation) {
            using Membership = Result<const TerrainTilePayloadEntry *>;
            if (const auto requirement = request.header.consumer == TerrainProducerConsumer::Collision
                                             ? request.manifest->TerrainRequirements().collision
                                             : request.manifest->TerrainRequirements().navigation;
                requirement == TerrainPayloadRequirement::NotRequested)
                return Membership::Failure(Failure(request, TerrainProducerErrors::Unavailable));
            auto entry = FindMember(request.manifest->Tiles(), request, budget, cancellation, [&selection](const auto &value) {
                return value.tile == selection.tile;
            });
            if (!entry.HasValue())
                return Membership::Failure(entry.ErrorValue());
            if (const auto &artifact = entry.Value()->consumers[static_cast<std::size_t>(Role(request))];
                !artifact || artifact->digest != selection.digest)
                return Membership::Failure(Failure(request, TerrainProducerErrors::Invalid));
            return entry;
        }

        /** @brief Compares complete cluster provenance and payload descriptor, not arbitrary same-tile roots. */
        bool MatchesCluster(const FoliageClusterPayloadEntry &entry, const CookedFoliageCluster &cluster) {
            return std::tuple{entry.tile,
                              entry.type,
                              entry.source,
                              entry.definition,
                              entry.placement,
                              entry.placementFingerprint,
                              entry.placementDigest,
                              entry.geometryDigest,
                              entry.profileFingerprint,
                              entry.bounds,
                              entry.geometryRadiusMillimeters,
                              entry.instanceCount,
                              entry.instances.digest,
                              entry.instances.byteSize} == std::tuple{cluster.tile,
                                                                      cluster.type,
                                                                      cluster.sourceRevision,
                                                                      cluster.definitionRevision,
                                                                      cluster.placementRevision,
                                                                      cluster.placementFingerprint,
                                                                      cluster.placementDigest,
                                                                      cluster.geometryDigest,
                                                                      cluster.profileFingerprint,
                                                                      cluster.bounds,
                                                                      cluster.geometryRadiusMillimeters,
                                                                      std::uint64_t{cluster.instances.size()},
                                                                      cluster.digest,
                                                                      std::uint64_t{cluster.payload.size()}};
        }

        /** @brief Requires full neutral decode counts and seams as well as the authoritative role digest. */
        bool MatchesMesh(const TerrainTilePayloadEntry &entry, const TerrainSourceArtifact &artifact) {
            const auto &descriptor = entry.consumers[static_cast<std::size_t>(artifact.role)];
            if (!descriptor || descriptor->schema != CurrentTerrainSourceArtifactSchema || descriptor->digest != artifact.digest ||
                descriptor->byteSize != artifact.payload.size())
                return false;
            const auto decodedBytes =
                artifact.vertices.size() * sizeof(TerrainSourceVertex) + artifact.triangles.size() * sizeof(TerrainSourceTriangle);
            return descriptor->decodedBytes == decodedBytes &&
                   descriptor->workItems == artifact.vertices.size() + artifact.triangles.size() && entry.seams == artifact.seams &&
                   entry.requiresSameLodNeighbors == artifact.requiresSameLodNeighbors;
        }

        /** @brief Resolves one terrain surface through manifest membership and the exact cook-issued consumer role. */
        Result<const TerrainSourceArtifact *> ResolveMesh(const TerrainProducerSnapshotRequest &request,
                                                          const TerrainProducerTileSelection &selection, Budget &budget,
                                                          const CancellationToken &cancellation) {
            auto entry = TileMembership(request, selection, budget, cancellation);
            if (!entry.HasValue())
                return Result<const TerrainSourceArtifact *>::Failure(entry.ErrorValue());
            auto mesh =
                FindMember(request.terrain->Artifacts(), request, budget, cancellation, [&selection, &request](const auto &artifact) {
                return artifact.tile == selection.tile && artifact.role == Role(request);
            });
            if (!mesh.HasValue())
                return mesh;
            if (!MatchesMesh(*entry.Value(), *mesh.Value()))
                return Result<const TerrainSourceArtifact *>::Failure(Failure(request, TerrainProducerErrors::Invalid));
            return mesh;
        }

        /** @brief Resolves one exact cluster digest and every placement/type binding through the manifest. */
        Result<const CookedFoliageCluster *> ResolveCluster(const TerrainProducerSnapshotRequest &request,
                                                            const TerrainProducerClusterSelection &selection, Budget &budget,
                                                            const CancellationToken &cancellation) {
            auto cluster = FindMember(request.foliage->Clusters(), request, budget, cancellation, [&selection](const auto &value) {
                return value.id == selection.cluster;
            });
            if (!cluster.HasValue())
                return cluster;
            if (cluster.Value()->digest != selection.digest)
                return Result<const CookedFoliageCluster *>::Failure(Failure(request, TerrainProducerErrors::Invalid));
            auto entry = FindMember(request.manifest->Clusters(), request, budget, cancellation, [&selection](const auto &value) {
                return value.cluster == selection.cluster;
            });
            if (!entry.HasValue())
                return Result<const CookedFoliageCluster *>::Failure(entry.ErrorValue());
            if (!MatchesCluster(*entry.Value(), *cluster.Value()))
                return Result<const CookedFoliageCluster *>::Failure(Failure(request, TerrainProducerErrors::Invalid));
            return cluster;
        }
    }  // namespace

    /** @copydoc ValidateCaptureRequest */
    Result<void> ValidateCaptureRequest(const TerrainProducerSnapshotRequest &request, const CancellationToken &cancellation) {
        auto validation = ValidateLifecycle(request, cancellation);
        if (!validation.HasValue())
            return validation;
        validation = ValidateEnvelope(request);
        if (!validation.HasValue())
            return validation;
        validation = ValidateSelections(request);
        if (!validation.HasValue())
            return validation;
        validation = ValidateCapabilities(request);
        if (!validation.HasValue())
            return validation;
        return ValidateRoots(request);
    }

    /** @copydoc VerifyManifest */
    Result<void> VerifyManifest(const TerrainProducerSnapshotRequest &request, Budget &budget, const CancellationToken &cancellation) {
        if (!budget.Work(request.manifest->Bytes().size() / 4096 + 1))
            return Result<void>::Failure(Failure(request, TerrainProducerErrors::Limit));
        if (const auto verified = VerifyTerrainPayloadManifest(*request.manifest, request.manifest->Bytes(), cancellation);
            !verified.HasValue())
            return Result<void>::Failure(
                WrapError(cancellation.IsCancellationRequested() ? TerrainProducerErrors::Cancelled : TerrainProducerErrors::Invalid,
                          verified.ErrorValue()));
        return Result<void>::Success();
    }

    /** @copydoc SelectMeshes */
    Result<std::vector<const TerrainSourceArtifact *>> SelectMeshes(const TerrainProducerSnapshotRequest &request, Budget &budget,
                                                                    const CancellationToken &cancellation) {
        return SelectMembers<TerrainSourceArtifact>(request, request.tiles, budget, cancellation,
                                                    [](const auto &invocation, const auto &selection, Budget &admission,
                                                       const CancellationToken &observer) {
            return ResolveMesh(invocation, selection, admission, observer);
        },
                                                    [](const auto *artifact) {
            return std::tuple{artifact->tile.tile.lod, artifact->tile.tile.z, artifact->tile.tile.x};
        }, &TerrainSourceArtifact::tile);
    }

    /** @copydoc SelectClusters */
    Result<std::vector<const CookedFoliageCluster *>> SelectClusters(const TerrainProducerSnapshotRequest &request, Budget &budget,
                                                                     const CancellationToken &cancellation) {
        return SelectMembers<CookedFoliageCluster>(request, request.clusters, budget, cancellation,
                                                   [](const auto &invocation, const auto &selection, Budget &admission,
                                                      const CancellationToken &observer) {
            return ResolveCluster(invocation, selection, admission, observer);
        },
                                                   [](const auto *cluster) {
            return std::tuple{cluster->tile, cluster->type, cluster->id};
        }, &CookedFoliageCluster::id);
    }
}  // namespace Horo::Terrain::ProducerDetail
