#include "Horo/Navigation/NavigationRuntimeTileRebuild.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Navigation {
    namespace {
        using Tile = std::shared_ptr<const NavigationCookedTile>;
        using TileClosure = std::vector<Tile>;
        using Candidate = Result<NavigationRuntimeTileRebuildCandidate>;

        /** @brief Fences ordered, fixed-width identities of the exact prepared tile closure. */
        [[nodiscard]] Sha256Digest ClosureIdentity(const std::span<const Tile> tiles) noexcept {
            Sha256Builder hash;
            for (const auto &tile : tiles)
                static_cast<void>(hash.Update(std::as_bytes(std::span{tile->ContentIdentity().bytes})));
            return hash.Finalize();
        }

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &reason) {
            return Result<T>::Failure(MakeError(reason));
        }

        /** @brief Checks that input and successor belong to the captured predecessor authority. */
        [[nodiscard]] bool MatchesAuthority(const NavigationRuntimeTileRebuildRequest &request) {
            const auto &fence = request.expected;
            return fence.activation.IsValid() && fence.sources == request.input.Revisions() && fence.overlay != 0 && fence.origin != 0 &&
                   fence.worldChanges != 0 && request.nextTopology.IsValid() && request.nextTopology > fence.activation.topology;
        }

        /** @brief Rejects budgets beyond the engine's existing storage and work ceilings. */
        [[nodiscard]] bool ValidLimits(const NavigationRuntimeTileRebuildRequest &request) {
            return request.maximumRebuiltTiles > 0 && request.maximumRebuiltTiles <= NavMeshArtifactLimits::MaximumTiles &&
                   request.maximumOwnedBytes > 0 && request.maximumOwnedBytes <= NavMeshArtifactLimits::MaximumOwnedBytes &&
                   request.tileLimits.IsValid();
        }

        /** @brief Requires a complete sorted layout so a rebuild cannot accidentally remove active coverage. */
        [[nodiscard]] bool MatchesActiveLayout(const NavigationRuntimeTileRebuildRequest &request) {
            if (request.layout.empty() || request.layout.size() != request.activeTiles.size() ||
                request.layout.size() > NavMeshArtifactLimits::MaximumTiles)
                return false;
            for (std::size_t index = 0; index < request.layout.size(); ++index) {
                const auto &key = request.layout[index].key;
                const auto &tile = request.activeTiles[index];
                if (!tile || tile->Key() != key || (index != 0 && request.layout[index - 1].key >= key))
                    return false;
            }
            return true;
        }

        /** @brief Retains permitted builder output only if it fits caller-selected topology bounds. */
        [[nodiscard]] bool FitsTopologyLimits(const NavigationTileBuildResult &topology, const NavigationTileBuildLimits &limits) {
            return topology.vertices.size() <= limits.maximumVertices && topology.polygons.size() <= limits.maximumPolygons &&
                   topology.offMeshLinks.size() <= limits.maximumOffMeshLinks &&
                   std::ranges::all_of(topology.polygons, [&limits](const auto &polygon) {
                return polygon.vertexIndices.count <= limits.maximumVerticesPerPolygon;
            });
        }

        /** @brief Reprojection can update placement and narrow clearance while preserving authored traversal semantics. */
        [[nodiscard]] bool MatchesLinkAuthority(const NavMeshOffMeshLink &previous, const NavMeshOffMeshLink &candidate) noexcept {
            return candidate.authoredLink == previous.authoredLink && candidate.area == previous.area &&
                   candidate.bidirectional == previous.bidirectional && candidate.radiusMeters > 0 &&
                   candidate.radiusMeters <= previous.radiusMeters && candidate.startPolygon != candidate.endPolygon;
        }

        /** @brief Admits persistent authored links under an explicit worker projection authority. */
        [[nodiscard]] bool CanReproject(const INavigationRuntimeLinkProjector *projector, std::span<const NavMeshOffMeshLink> links) {
            return projector && std::ranges::all_of(links, [](const auto &link) {
                return link.authoredLink.IsValid();
            });
        }

        /** @brief Reprojects the complete old authored link set under the detached build's remaining scratch ceiling. */
        [[nodiscard]] Result<void> ReprojectLinks(const NavigationRuntimeTileRebuildRequest &request, const NavigationCookedTile &previous,
                                                  NavigationTileBuildResult &built, const std::size_t scratch,
                                                  const CancellationToken &cancel) {
            const auto &links = previous.Topology().offMeshLinks;
            if (links.empty())
                return Result<void>::Success();
            if (links.size() > request.tileLimits.maximumOffMeshLinks || links.size() >= scratch / sizeof(NavMeshOffMeshLink))
                return Failure<void>(NavigationErrors::CapacityExceeded);
            std::vector<NavMeshOffMeshLink> projected;
            projected.reserve(links.size());
            if (projected.capacity() >= scratch / sizeof(NavMeshOffMeshLink))
                return Failure<void>(NavigationErrors::CapacityExceeded);
            const auto remaining = scratch - projected.capacity() * sizeof(NavMeshOffMeshLink);
            for (const auto &link : links) {
                if (cancel.IsCancellationRequested())
                    return Failure<void>(NavigationErrors::BakeInputCancelled);
                const auto result = request.linkProjector->Project(request.input, previous.Key(), built, link, remaining, cancel);
                if (result.HasError())
                    return Result<void>::Failure(result.ErrorValue());
                if (!MatchesLinkAuthority(link, result.Value()))
                    return Failure<void>(NavigationErrors::BakeInputInvalid);
                projected.push_back(result.Value());
            }
            built.offMeshLinks = std::move(projected);
            return Result<void>::Success();
        }

        /** @brief Builds one detached tile, keeping input scratch and encoding within the operation's budget. */
        [[nodiscard]] Result<Tile> Build(const NavigationRuntimeTileRebuildRequest &request, const INavigationMeshBuilder &builder,
                                         const NavigationPreparedTile &prepared, const NavigationCookedTile &previous,
                                         const std::size_t available, const CancellationToken &cancel) {
            auto limits = request.tileLimits;
            const auto preparedBytes = prepared.triangles.capacity() * sizeof(NavigationTileBuildTriangle) +
                                       prepared.modifiers.capacity() * sizeof(NavigationTileBuildModifier);
            if (preparedBytes >= available)
                return Failure<Tile>(NavigationErrors::CapacityExceeded);
            const auto outputBytes = available - preparedBytes;
            limits.maximumOwnedBytes = std::min<std::uint64_t>(limits.maximumOwnedBytes, outputBytes / 2);
            if (limits.maximumOwnedBytes == 0)
                return Failure<Tile>(NavigationErrors::CapacityExceeded);
            auto built = builder.BuildTile({.key = prepared.tile.key.tile,
                                            .bounds = prepared.tile.bounds,
                                            .tileSizeMeters = prepared.tile.tileSizeMeters,
                                            .buildGeometry = prepared.geometry,
                                            .triangles = prepared.triangles,
                                            .modifiers = prepared.modifiers,
                                            .limits = limits,
                                            .borderSizeCells = prepared.borderSizeCells},
                                           cancel);
            if (built.HasError())
                return Result<Tile>::Failure(std::move(built).ErrorValue());
            if (cancel.IsCancellationRequested())
                return Failure<Tile>(NavigationErrors::BakeInputCancelled);
            if (!built.Value().offMeshLinks.empty())
                return Failure<Tile>(NavigationErrors::OperationUnsupported);
            if (!FitsTopologyLimits(built.Value(), limits))
                return Failure<Tile>(NavigationErrors::CapacityExceeded);
            auto topology = std::move(built).Value();
            if (auto projected = ReprojectLinks(request, previous, topology, outputBytes / 2, cancel); projected.HasError())
                return Result<Tile>::Failure(std::move(projected).ErrorValue());
            return NavigationCookedTile::Create(prepared, std::move(topology),
                                                std::min<std::size_t>(outputBytes, NavigationTileBuildLimits::MaximumOwnedBytes));
        }

        /** @brief Charges retained topology and counts detached builds independently from reuse. */
        struct RebuildClosure final {
            TileClosure tiles;
            std::size_t remaining{};
            std::size_t rebuilt{};

            [[nodiscard]] Result<void> Add(Tile tile) {
                const auto bytes = tile->StorageBytes();
                if (bytes > remaining)
                    return Failure<void>(NavigationErrors::CapacityExceeded);
                remaining -= bytes;
                tiles.push_back(std::move(tile));
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> Resolve(const NavigationRuntimeTileRebuildRequest &request, const INavigationMeshBuilder &builder,
                                               const std::size_t index, const CancellationToken &cancel) {
                if (remaining == 0)
                    return Failure<void>(NavigationErrors::CapacityExceeded);
                const auto &old = request.activeTiles[index];
                const auto &links = old->Topology().offMeshLinks;
                if (!links.empty() && !CanReproject(request.linkProjector, links))
                    return Failure<void>(NavigationErrors::OperationUnsupported);
                auto prepared = PrepareNavigationBakeTile(request.input, request.layout[index], request.compatibility, cancel,
                                                          std::min<std::size_t>(remaining, NavigationBakeInputLimits::MaximumOwnedBytes));
                if (prepared.HasError())
                    return Result<void>::Failure(std::move(prepared).ErrorValue());
                if (prepared.Value().dependencyKey == old->DependencyKey() && links.empty()) {
                    if (!FitsTopologyLimits(old->Topology(), request.tileLimits))
                        return Failure<void>(NavigationErrors::CapacityExceeded);
                    return Add(old);
                }
                if (rebuilt >= request.maximumRebuiltTiles)
                    return Failure<void>(NavigationErrors::CapacityExceeded);
                auto tile = Build(request, builder, prepared.Value(), *old, remaining, cancel);
                if (tile.HasError())
                    return Result<void>::Failure(std::move(tile).ErrorValue());
                ++rebuilt;
                return Add(std::move(tile).Value());
            }
        };

        /** @brief Holds the bounded result and pins all unchanged tiles without touching the live world. */
        [[nodiscard]] Result<RebuildClosure> PrepareClosure(const NavigationRuntimeTileRebuildRequest &request,
                                                            const INavigationMeshBuilder &builder, const CancellationToken &cancel) {
            if (request.layout.size() > request.maximumOwnedBytes / sizeof(Tile))
                return Failure<RebuildClosure>(NavigationErrors::CapacityExceeded);
            RebuildClosure closure;
            closure.tiles.reserve(request.layout.size());
            const auto storage = closure.tiles.capacity() * sizeof(Tile);
            if (storage > request.maximumOwnedBytes)
                return Failure<RebuildClosure>(NavigationErrors::CapacityExceeded);
            closure.remaining = request.maximumOwnedBytes - storage;
            for (std::size_t index = 0; index < request.layout.size(); ++index) {
                auto resolved = closure.Resolve(request, builder, index, cancel);
                if (resolved.HasError())
                    return Result<RebuildClosure>::Failure(std::move(resolved).ErrorValue());
            }
            return Result<RebuildClosure>::Success(std::move(closure));
        }

        /** @brief Selects no backend for explicit loss of walkable coverage. */
        [[nodiscard]] Result<NavigationPreparedRuntimeRebuildBackend> PrepareBackend(const NavigationRuntimeTileRebuildRequest &request,
                                                                                     const INavigationRuntimeRebuildBackendFactory &factory,
                                                                                     const RebuildClosure &closure,
                                                                                     const CancellationToken &cancel) {
            using Backend = Result<NavigationPreparedRuntimeRebuildBackend>;
            if (cancel.IsCancellationRequested())
                return Failure<NavigationPreparedRuntimeRebuildBackend>(NavigationErrors::BakeInputCancelled);
            if (std::ranges::all_of(closure.tiles, [](const Tile &tile) {
                return tile->Topology().IsEmpty();
            }))
                return Backend::Success({});
            if (closure.remaining == 0)
                return Failure<NavigationPreparedRuntimeRebuildBackend>(NavigationErrors::CapacityExceeded);
            auto activation = request.expected.activation;
            activation.topology = request.nextTopology;
            auto backend = factory.Prepare(activation, closure.tiles, closure.remaining, cancel);
            if (backend.HasError())
                return backend;
            const auto &prepared = backend.Value();
            if (!prepared.backend || prepared.reservedProviderBytes == 0)
                return Failure<NavigationPreparedRuntimeRebuildBackend>(NavigationErrors::OutcomeDescriptorInvalid);
            if (prepared.reservedProviderBytes > closure.remaining)
                return Failure<NavigationPreparedRuntimeRebuildBackend>(NavigationErrors::CapacityExceeded);
            return backend;
        }
    }  // namespace

    /** @copydoc NavigationRuntimeTileRebuildCandidate::NavigationRuntimeTileRebuildCandidate */
    NavigationRuntimeTileRebuildCandidate::NavigationRuntimeTileRebuildCandidate(const NavigationRuntimeRebuildFence &expected,
                                                                                 NavigationGeneration next, TileClosure tiles,
                                                                                 std::unique_ptr<INavigationQueryBackend> backend,
                                                                                 const std::size_t rebuiltTiles,
                                                                                 const std::size_t reservedBytes) noexcept
        : expected_(expected), next_(next), tiles_(std::move(tiles)), backend_(std::move(backend)), rebuiltTiles_(rebuiltTiles),
          reservedBytes_(reservedBytes) {}

    /** @copydoc PrepareRuntimeNavigationTileRebuild */
    Candidate PrepareRuntimeNavigationTileRebuild(const NavigationRuntimeTileRebuildRequest &request, const INavigationMeshBuilder &builder,
                                                  const INavigationRuntimeRebuildBackendFactory &factory, const CancellationToken &cancel) {
        if (cancel.IsCancellationRequested())
            return Failure<NavigationRuntimeTileRebuildCandidate>(NavigationErrors::BakeInputCancelled);
        if (!MatchesAuthority(request))
            return Failure<NavigationRuntimeTileRebuildCandidate>(NavigationErrors::StaleSnapshot);
        if (!ValidLimits(request) || !MatchesActiveLayout(request))
            return Failure<NavigationRuntimeTileRebuildCandidate>(NavigationErrors::BakeInputInvalid);
        try {
            auto prepared = PrepareClosure(request, builder, cancel);
            if (prepared.HasError())
                return Candidate::Failure(std::move(prepared).ErrorValue());
            auto closure = std::move(prepared).Value();
            auto backend = PrepareBackend(request, factory, closure, cancel);
            if (backend.HasError())
                return Candidate::Failure(std::move(backend).ErrorValue());
            if (cancel.IsCancellationRequested())
                return Failure<NavigationRuntimeTileRebuildCandidate>(NavigationErrors::BakeInputCancelled);
            auto provider = std::move(backend).Value();
            const auto reserved = request.maximumOwnedBytes - closure.remaining + provider.reservedProviderBytes;
            NavigationRuntimeTileRebuildCandidate candidate{request.expected,         request.nextTopology,
                                                            std::move(closure.tiles), std::move(provider.backend),
                                                            closure.rebuilt,          reserved};
            candidate.predecessorTiles_ = ClosureIdentity(request.activeTiles);
            return Candidate::Success(std::move(candidate));
        } catch (const std::bad_alloc &) {
            return Failure<NavigationRuntimeTileRebuildCandidate>(NavigationErrors::CapacityExceeded);
        }
    }

    /** @copydoc NavigationRuntimeTileRebuildCandidate::MatchesPredecessor */
    bool NavigationRuntimeTileRebuildCandidate::MatchesPredecessor(const std::span<const Tile> tiles) const noexcept {
        return tiles.size() == tiles_.size() && ClosureIdentity(tiles) == predecessorTiles_;
    }

    /** @copydoc NavigationRuntimeTileRebuildCandidate::PublishAtSafePoint */
    Result<TileClosure> NavigationRuntimeTileRebuildCandidate::PublishAtSafePoint(NavigationWorldLifecycle &world,
                                                                                  const NavigationRuntimeRebuildFence &current,
                                                                                  const CancellationToken &cancel) && {
        if (reservation_.state_)
            return Failure<TileClosure>(NavigationErrors::OperationUnsupported);
        auto tiles = std::exchange(tiles_, {});
        auto backend = std::move(backend_);
        if (cancel.IsCancellationRequested())
            return Failure<TileClosure>(NavigationErrors::BakeInputCancelled);
        if (tiles.empty() || current != expected_)
            return Failure<TileClosure>(NavigationErrors::StaleSnapshot);
        if (auto staged = backend ? world.StageTopology(expected_.activation, next_, std::move(backend))
                                  : world.StageEmptyTopology(expected_.activation, next_);
            staged.HasError())
            return Result<TileClosure>::Failure(std::move(staged).ErrorValue());
        auto successor = expected_.activation;
        successor.topology = next_;
        if (cancel.IsCancellationRequested()) {
            (void)world.DiscardStagedTopology(successor);
            return Failure<TileClosure>(NavigationErrors::BakeInputCancelled);
        }
        if (auto committed = world.CommitStagedTopology(successor); committed.HasError()) {
            (void)world.DiscardStagedTopology(successor);
            return Result<TileClosure>::Failure(std::move(committed).ErrorValue());
        }
        return Result<TileClosure>::Success(std::move(tiles));
    }
}  // namespace Horo::Navigation
