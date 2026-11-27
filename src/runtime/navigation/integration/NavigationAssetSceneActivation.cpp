#include "Horo/Navigation/NavigationAssetSceneActivation.h"

#include <algorithm>
#include <atomic>
#include <new>

namespace Horo::Navigation {
    namespace Detail {
        /** @brief Atomic counters protect only final worker-thread destruction; host mutation stays owner-thread. */
        struct NavigationAssetWorldAccounting final {
            std::atomic<std::size_t> worlds{};
            std::atomic<std::size_t> providerBytes{};
            std::atomic<bool> rebuildReserved{}; /**< Set only by owner admission, cleared after terminal storage release. */
        };

        /** @brief Owns one tile's storage charge across every closure and worker pin that shares the allocation. */
        struct NavigationAssetRebuildTile final {
            NavigationAssetRebuildTile(std::shared_ptr<const NavigationCookedTile> tile,
                                       std::shared_ptr<NavigationAssetWorldAccounting> counters, std::size_t bytes)
                : contents(std::move(tile)), accounting(std::move(counters)), reservation(bytes) {
                accounting->providerBytes.fetch_add(reservation);
            }

            NavigationAssetRebuildTile(const NavigationAssetRebuildTile &) = delete;
            NavigationAssetRebuildTile &operator=(const NavigationAssetRebuildTile &) = delete;

            ~NavigationAssetRebuildTile() {
                contents.reset();
                accounting->providerBytes.fetch_sub(reservation);
            }

            std::shared_ptr<const NavigationCookedTile> contents;
            std::shared_ptr<NavigationAssetWorldAccounting> accounting;
            std::size_t reservation;
        };

        /** @brief Charges each closure's index independently of the tile allocations it shares. */
        struct NavigationAssetRebuildTiles final {
            using Tiles = std::vector<std::shared_ptr<const NavigationCookedTile>>;

            NavigationAssetRebuildTiles(Tiles contents, std::shared_ptr<NavigationAssetWorldAccounting> counters, std::size_t bytes,
                                        const std::optional<NavigationProjectProfile> &profile)
                : tiles(std::move(contents)), policy(profile), accounting(std::move(counters)), reservation(bytes) {
                accounting->providerBytes.fetch_add(reservation);
            }

            NavigationAssetRebuildTiles(const NavigationAssetRebuildTiles &) = delete;
            NavigationAssetRebuildTiles &operator=(const NavigationAssetRebuildTiles &) = delete;

            ~NavigationAssetRebuildTiles() {
                Tiles{}.swap(tiles);
                accounting->providerBytes.fetch_sub(reservation);
            }

            Tiles tiles;
            std::optional<NavigationProjectProfile> policy;
            std::shared_ptr<NavigationAssetWorldAccounting> accounting;
            std::size_t reservation;
        };

        /** @brief Host state shared with aggregate candidates, never borrowed by worker backend destruction. */
        struct NavigationAssetSceneState final {
            Assets::AssetPayloadCache *cache{};
            AssetCookTargetId target;
            NavigationAssetBackendFactory factory;
            NavigationAssetSceneLimits limits;
            std::optional<std::vector<NavMeshAssetContentExpectation>> content;
            NavigationAssetSceneSelection selection;
            std::shared_ptr<NavigationAssetWorldAccounting> accounting{std::make_shared<NavigationAssetWorldAccounting>()};
            std::shared_ptr<NavigationWorldLifecycle> active;
            std::shared_ptr<const std::vector<NavigationAssetProvenance>> provenance;
            std::shared_ptr<const NavigationAssetRebuildTiles> rebuildTiles;
            bool closed{};
        };

        /** @brief Rechecks the host's fresh obstacle, source and command observation on the owner lane. */
        struct RuntimeRebuildAdoption final {
            NavigationRuntimeRebuildFence expected;
            NavigationRuntimeRebuildAuthority observe;
            CancellationToken cancellation;
            std::shared_ptr<const NavigationAssetRebuildTiles> tiles;

            [[nodiscard]] Result<void> CheckAuthority() const {
                if (!observe)
                    return Result<void>::Failure(MakeError(NavigationErrors::CapabilityUnavailable));
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
                const auto current = observe();
                if (current.HasError())
                    return Result<void>::Failure(current.ErrorValue());
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
                return current.Value() == expected ? Result<void>::Success()
                                                   : Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
            }
        };
    }  // namespace Detail

    /** @brief A detached job pins only its charged bytes and slot, never the Scene or its callbacks. */
    struct NavigationRuntimeRebuildReservation::State final {
        State(std::shared_ptr<Detail::NavigationAssetWorldAccounting> counters, std::size_t maximumOwnedBytes)
            : accounting(std::move(counters)), bytes(maximumOwnedBytes) {
            accounting->worlds.fetch_add(1);
            accounting->providerBytes.fetch_add(bytes);
            accounting->rebuildReserved.store(true);
        }

        State(const State &) = delete;
        State &operator=(const State &) = delete;

        ~State() {
            accounting->providerBytes.fetch_sub(bytes);
            accounting->worlds.fetch_sub(1);
            accounting->rebuildReserved.store(false);
        }

        /** @brief Accepts only a reservation charged to the supplied Scene accounting owner. */
        [[nodiscard]] static bool HeldBy(const std::shared_ptr<const State> &reservation,
                                         const std::shared_ptr<Detail::NavigationAssetWorldAccounting> &owner) noexcept {
            return reservation && reservation->accounting == owner;
        }

        std::shared_ptr<Detail::NavigationAssetWorldAccounting> accounting;
        std::size_t bytes;
    };

    /** @brief Rolls back detached navigation work unless the containing structural transaction commits it. */
    class NavigationStreamedTopologyPublication final : public Runtime::SceneStructuralCandidate {
    public:
        NavigationStreamedTopologyPublication(std::shared_ptr<Detail::NavigationAssetSceneState> state,
                                              std::shared_ptr<NavigationWorldLifecycle> world,
                                              const NavigationWorldActivationDescriptor &candidate, NavigationWorldReadLease predecessor,
                                              NavigationTopologyChange change, std::optional<Detail::RuntimeRebuildAdoption> rebuild = {})
            : state_(std::move(state)), world_(std::move(world)), candidate_(candidate), predecessor_(std::move(predecessor)),
              change_(change), rebuild_(std::move(rebuild)) {}

        NavigationStreamedTopologyPublication(const NavigationStreamedTopologyPublication &) = delete;
        NavigationStreamedTopologyPublication &operator=(const NavigationStreamedTopologyPublication &) = delete;

        ~NavigationStreamedTopologyPublication() override {
            if (ownsStage_)
                static_cast<void>(world_->TryDiscardStagedTopology(candidate_));
        }

        /** @brief Records ownership only after this candidate successfully stages its provider. */
        void OwnStagedTopology() noexcept {
            ownsStage_ = true;
        }

        Result<void> ValidatePublication() const override {
            if (state_->closed || state_->active != world_)
                return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
            if (rebuild_.has_value()) {
                if (auto checked = rebuild_->CheckAuthority(); checked.HasError())
                    return checked;
            }
            return world_->ValidateStagedTopology(candidate_);
        }

        void Publish() noexcept override {
            world_->PublishValidatedTopology(candidate_, change_);
            if (rebuild_.has_value())
                state_->rebuildTiles.swap(rebuild_->tiles);
            ownsStage_ = false;
        }

        Result<void> AfterPublication() override {
            NavigationWorldLifecycle::NotifyRetired(predecessor_);
            predecessor_ = {};
            rebuild_.reset();
            static_cast<void>(world_->CollectRetired());
            return Result<void>::Success();
        }

    private:
        std::shared_ptr<Detail::NavigationAssetSceneState> state_;
        std::shared_ptr<NavigationWorldLifecycle> world_;
        NavigationWorldActivationDescriptor candidate_;
        NavigationWorldReadLease predecessor_; /**< Pins retirement capacity through the no-fail publication phase. */
        NavigationTopologyChange change_;
        bool ownsStage_{};
        std::optional<Detail::RuntimeRebuildAdoption> rebuild_;
    };

    namespace {
        /** @brief Backend lifetime, not cache residency or Scene borrows, owns immutable tile and reservation pins. */
        class PinnedAssetBackend final : public INavigationQueryBackend {
        public:
            PinnedAssetBackend(NavigationPreparedAssetBackend prepared, std::vector<Assets::AssetPayloadLease> bytes,
                               std::shared_ptr<Detail::NavigationAssetWorldAccounting> accounting,
                               std::shared_ptr<const std::vector<NavigationAssetProvenance>> provenance,
                               std::shared_ptr<const Detail::NavigationAssetRebuildTiles> rebuildTiles = {})
                : backend_(std::move(prepared.backend)), bytes_(std::move(bytes)), accounting_(std::move(accounting)),
                  reservation_(prepared.reservedProviderBytes), provenance_(std::move(provenance)), rebuildTiles_(std::move(rebuildTiles)) {
                accounting_->worlds.fetch_add(1);
                accounting_->providerBytes.fetch_add(reservation_);
            }

            ~PinnedAssetBackend() override {
                // Destruction refunds only after native storage has actually been released.
                backend_.reset();
                bytes_.clear();
                accounting_->providerBytes.fetch_sub(reservation_);
                accounting_->worlds.fetch_sub(1);
            }

            [[nodiscard]] NavigationProviderCapabilities Capabilities() const noexcept override {
                return backend_->Capabilities();
            }

            [[nodiscard]] Result<NavigationPath> FindPath(const NavigationPathRequest &request,
                                                          const CancellationToken &token) const override {
                return backend_->FindPath(request, token);
            }

            [[nodiscard]] Result<NavigationProjectionResult> ProjectPoint(const NavigationPointProjectionRequest &request,
                                                                          const CancellationToken &token) const override {
                return backend_->ProjectPoint(request, token);
            }

            [[nodiscard]] Result<NavigationSamplePositionResult> SamplePosition(const NavigationSamplePositionRequest &request,
                                                                                const CancellationToken &token) const override {
                return backend_->SamplePosition(request, token);
            }

            [[nodiscard]] Result<NavigationRaycastResult> Raycast(const NavigationRaycastRequest &request,
                                                                  const CancellationToken &token) const override {
                return backend_->Raycast(request, token);
            }

            [[nodiscard]] Result<NavigationPolygonQueryResult> QueryPolygons(const NavigationPolygonQueryRequest &request,
                                                                             const CancellationToken &token) const override {
                return backend_->QueryPolygons(request, token);
            }

        private:
            std::unique_ptr<INavigationQueryBackend> backend_;
            std::vector<Assets::AssetPayloadLease> bytes_;
            std::shared_ptr<Detail::NavigationAssetWorldAccounting> accounting_;
            std::size_t reservation_{};
            std::shared_ptr<const std::vector<NavigationAssetProvenance>> provenance_;
            std::shared_ptr<const Detail::NavigationAssetRebuildTiles> rebuildTiles_;
        };

        /** @brief Publication swaps a fully finalized detached owner, while shutdown targets only this candidate. */
        class AssetWorldCandidate final : public Runtime::SceneActivationCandidate {
        public:
            AssetWorldCandidate(std::shared_ptr<Detail::NavigationAssetSceneState> state, const Runtime::RuntimeSceneView &scene,
                                std::shared_ptr<NavigationWorldLifecycle> world,
                                std::shared_ptr<const std::vector<NavigationAssetProvenance>> provenance,
                                std::shared_ptr<const Detail::NavigationAssetRebuildTiles> rebuildTiles)
                : state_(std::move(state)), scene_(scene), world_(std::move(world)), provenance_(std::move(provenance)),
                  rebuildTiles_(std::move(rebuildTiles)) {}

            [[nodiscard]] Result<void> ValidatePublication() const override {
                if (state_->closed || !scene_.IsCurrent()) {
                    return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
                }
                return Result<void>::Success();
            }

            /** @brief The prepared navigation root owns runtime navigation state, not a persistent-world dataset identity. */
            Runtime::SceneCanonicalDatasetProjection CanonicalDatasetProjection() const noexcept override {
                return Runtime::SceneCanonicalDatasetProjection::Absent;
            }

            void Publish() noexcept override {
                state_->active = world_;
                state_->provenance = provenance_;
                state_->rebuildTiles = std::move(rebuildTiles_);
            }

            void Shutdown() noexcept override {
                if (world_)
                    world_->BeginShutdown();
                if (state_->active == world_) {
                    state_->active.reset();
                    state_->provenance.reset();
                    state_->rebuildTiles.reset();
                }
                world_.reset();
            }

        private:
            std::shared_ptr<Detail::NavigationAssetSceneState> state_;
            Runtime::RuntimeSceneView scene_;
            std::shared_ptr<NavigationWorldLifecycle> world_;
            std::shared_ptr<const std::vector<NavigationAssetProvenance>> provenance_;
            std::shared_ptr<const Detail::NavigationAssetRebuildTiles> rebuildTiles_;
        };

        /** @brief Pending RuntimeScene incarnations are never reused and fence all provider/Scene identities. */
        [[nodiscard]] Result<NavigationWorldActivationDescriptor> Descriptor(const Runtime::RuntimeSceneView &scene) {
            const auto value = scene.RuntimeId().value;
            const auto world = NavigationWorldId::Create(value);
            const auto sceneId = NavigationSceneRuntimeId::Create(value);
            const auto generation = NavigationSceneGeneration::Create(value);
            const auto topology = NavigationGeneration::Create(value);
            if (!scene.IsCurrent() || world.HasError() || sceneId.HasError() || generation.HasError() || topology.HasError()) {
                return Result<NavigationWorldActivationDescriptor>::Failure(MakeError(NavigationErrors::StaleSnapshot));
            }
            return Result<NavigationWorldActivationDescriptor>::Success(
                {sceneId.Value(), generation.Value(), world.Value(), topology.Value()});
        }

        /** @brief Resolve one canonical asset once per Scene preparation, even when multiple surfaces share it. */
        [[nodiscard]] Result<std::size_t> ResolveAsset(const Assets::AssetId id, const Runtime::RuntimeSceneView &scene,
                                                       Detail::NavigationAssetSceneState &state, std::vector<LoadedNavMeshAsset> &assets) {
            if (const auto found = std::ranges::find(assets, id, &LoadedNavMeshAsset::id); found != assets.end())
                return Result<std::size_t>::Success(static_cast<std::size_t>(found - assets.begin()));
            const auto resolved = scene.FindAsset(id);
            if (!resolved || !resolved->type)
                return Result<std::size_t>::Failure(MakeError(NavigationErrors::NoNavigationData));
            const NavMeshAssetContentExpectation *expectation = nullptr;
            if (state.content) {
                const auto found = std::ranges::find(*state.content, id, &NavMeshAssetContentExpectation::id);
                if (found == state.content->end())
                    return Result<std::size_t>::Failure(MakeError(NavigationErrors::NoNavigationData));
                expectation = std::to_address(found);
            }
            const Assets::AssetDependency metadata{id, *resolved->type};
            auto loaded = LoadNavMeshAsset(metadata, scene.AssetRegistryRevision(), resolved->bytes, state.target, *state.cache,
                                           {.limits = state.limits.assets,
                                            .expectation = expectation,
                                            .scope = state.selection.scope,
                                            .retainTileBytes = !state.selection.streamCoverage});
            if (loaded.HasError())
                return Result<std::size_t>::Failure(loaded.ErrorValue());
            if (const auto &required = state.selection.requiredProfile; required) {
                const auto &provenance = loaded.Value().contentProvenance;
                if (!provenance || !provenance->projectProfile || !provenance->projectProfile->MatchesAuthority(*required))
                    return Result<std::size_t>::Failure(MakeError(NavigationErrors::ProjectProfileStale));
            }
            assets.push_back(std::move(loaded).Value());
            return Result<std::size_t>::Success(assets.size() - 1);
        }

        /** @brief Match every enabled surface/profile exactly; missing or stale generated partitions cannot fall back. */
        [[nodiscard]] Result<void> SelectSurfaces(const Runtime::RuntimeSceneDefinition &definition, const Runtime::RuntimeSceneView &scene,
                                                  Detail::NavigationAssetSceneState &state, std::vector<LoadedNavMeshAsset> &assets,
                                                  std::vector<NavigationLoadedSurface> &surfaces) {
            // Reserve before taking pointers into move-only loaded partition vectors.
            assets.reserve(definition.AssetDependencies().size());
            for (const auto &entity : definition.Entities()) {
                if (!entity.components.navigationSurface || !entity.components.navigationSurface->enabled)
                    continue;
                const auto &surface = *entity.components.navigationSurface;
                const auto assetIndex = ResolveAsset(surface.definition, scene, state, assets);
                if (assetIndex.HasError())
                    return Result<void>::Failure(assetIndex.ErrorValue());
                for (const auto profile : surface.profiles) {
                    const auto &partitions = assets[assetIndex.Value()].partitions;
                    const auto found = std::ranges::find_if(partitions, [&](const auto &partition) {
                        return partition.surface == surface.id && partition.profile == profile;
                    });
                    if (found == partitions.end())
                        return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
                    surfaces.emplace_back(surface.definition, surface.generation, std::to_address(found));
                }
            }
            return Result<void>::Success();
        }

        /** @brief Required packaged NavMesh content cannot be bypassed by an empty surface projection. */
        [[nodiscard]] Result<void> ValidateRequiredCoverage(const Runtime::RuntimeSceneDefinition &definition,
                                                            const Runtime::RuntimeSceneView &scene,
                                                            Detail::NavigationAssetSceneState &state,
                                                            std::vector<LoadedNavMeshAsset> &assets,
                                                            const std::span<const NavigationLoadedSurface> surfaces) {
            for (const auto &dependency : definition.AssetDependencies()) {
                if (dependency.expectedType.Value() != Assets::NavMeshAssetTypeName)
                    continue;
                if (const auto resolved = ResolveAsset(dependency.id, scene, state, assets); resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                if (std::ranges::none_of(surfaces, [&dependency](const auto &surface) {
                    return surface.definition == dependency.id;
                }))
                    return Result<void>::Failure(MakeError(NavigationErrors::SceneSurfaceMissing));
            }
            return Result<void>::Success();
        }

        /** @brief Enabled grounded agents require an eligible declared partition in the same packaged Scene world. */
        [[nodiscard]] Result<void> ValidateAgentCoverage(const Runtime::RuntimeSceneDefinition &definition,
                                                         const std::span<const NavigationLoadedSurface> surfaces) {
            for (const auto &entity : definition.Entities()) {
                if (!entity.components.navigationAgent || !entity.components.navigationAgent->enabled)
                    continue;
                const auto profile = entity.components.navigationAgent->profile;
                if (std::ranges::none_of(surfaces, [profile](const auto &surface) {
                    return surface.partition && surface.partition->profile == profile;
                }))
                    return Result<void>::Failure(MakeError(NavigationErrors::SceneSurfaceMissing));
            }
            return Result<void>::Success();
        }

        /** @brief Retired query-pinned worlds consume the same finite provider budget as the active world. */
        [[nodiscard]] Result<std::size_t> AvailableProviderBudget(const Detail::NavigationAssetSceneState &state,
                                                                  bool needsProvider = true) {
            const auto retainedBytes = state.accounting->providerBytes.load();
            if ((needsProvider && state.accounting->worlds.load() >= state.limits.maximumLiveWorlds) ||
                retainedBytes >= state.limits.maximumReservedProviderBytes)
                return Result<std::size_t>::Failure(MakeError(NavigationErrors::CapacityExceeded));
            return Result<std::size_t>::Success(state.limits.maximumReservedProviderBytes - retainedBytes);
        }

        /** @brief Rejects work outside the qualified builder and available Scene storage ceilings. */
        [[nodiscard]] bool FitsRebuildBudget(const Detail::NavigationAssetSceneState &state, const std::size_t bytes) {
            const auto budget = AvailableProviderBudget(state);
            return budget.HasValue() && bytes > 0 && bytes <= NavMeshArtifactLimits::MaximumOwnedBytes && bytes <= budget.Value();
        }

        using RuntimeTilePins = std::shared_ptr<const Detail::NavigationAssetRebuildTiles>;
        using RebuildTiles = Detail::NavigationAssetRebuildTiles::Tiles;

        /** @brief Measures the pointer index, excluding the tile allocations it references. */
        [[nodiscard]] std::size_t TileIndexBytes(const RebuildTiles &tiles) noexcept {
            return tiles.capacity() * sizeof(RebuildTiles::value_type);
        }

        /** @brief Separates the worker's complete portable closure measurement from its native provider charge. */
        [[nodiscard]] Result<std::size_t> TileClosureBytes(const RebuildTiles &tiles, std::size_t budget) {
            const auto bytes = TileIndexBytes(tiles);
            if (bytes > budget)
                return Result<std::size_t>::Failure(MakeError(NavigationErrors::CapacityExceeded));
            auto total = bytes;
            for (const auto &tile : tiles) {
                const auto tileBytes = tile->StorageBytes();
                if (tileBytes > budget - total)
                    return Result<std::size_t>::Failure(MakeError(NavigationErrors::CapacityExceeded));
                total += tileBytes;
            }
            return Result<std::size_t>::Success(total);
        }

        /** @brief Inherits charges for exact reused allocations and refunds partially pinned closures on failure. */
        [[nodiscard]] Result<RuntimeTilePins> PinTileClosure(RebuildTiles tiles, const std::span<const RebuildTiles::value_type> previous,
                                                             const std::shared_ptr<Detail::NavigationAssetWorldAccounting> &accounting,
                                                             const std::optional<NavigationProjectProfile> &policy, std::size_t budget) {
            using Return = Result<RuntimeTilePins>;
            const auto indexBytes = TileIndexBytes(tiles);
            if (indexBytes > budget)
                return Return::Failure(MakeError(NavigationErrors::CapacityExceeded));
            budget -= indexBytes;
            for (std::size_t i = 0; i < tiles.size(); ++i) {
                auto &tile = tiles[i];
                if (!previous.empty() && tile == previous[i]) {
                    tile = previous[i];
                    continue;
                }
                const auto bytes = tile->StorageBytes() + sizeof(Detail::NavigationAssetRebuildTile);
                if (bytes > budget)
                    return Return::Failure(MakeError(NavigationErrors::CapacityExceeded));
                auto pin = std::make_shared<Detail::NavigationAssetRebuildTile>(std::move(tile), accounting, bytes);
                tile = {pin, pin->contents.get()};
                budget -= bytes;
            }
            return Return::Success(std::make_shared<Detail::NavigationAssetRebuildTiles>(std::move(tiles), accounting, indexBytes, policy));
        }

        /** @brief Retains only the exactly selected non-streamed partition for subsequent rebuild authorizations. */
        [[nodiscard]] Result<RuntimeTilePins> PinInitialRebuildTiles(const Detail::NavigationAssetSceneState &state,
                                                                     std::span<const NavigationLoadedSurface> surfaces,
                                                                     std::span<const LoadedNavMeshAsset> assets) {
            using Return = Result<RuntimeTilePins>;
            if (!state.selection.runtimeRebuild)
                return Return::Success({});
            if (state.selection.streamCoverage || surfaces.size() != 1 || !surfaces.front().partition ||
                surfaces.front().partition->tiles.empty())
                return Return::Failure(MakeError(NavigationErrors::OperationUnsupported));
            const auto budget = AvailableProviderBudget(state);
            if (budget.HasError())
                return Return::Failure(budget.ErrorValue());
            const auto asset = std::ranges::find(assets, surfaces.front().definition, &LoadedNavMeshAsset::id);
            const auto &policy = (asset != assets.end() && asset->contentProvenance) ? asset->contentProvenance->projectProfile
                                                                                     : state.selection.requiredProfile;
            return PinTileClosure(surfaces.front().partition->tiles, {}, state.accounting, policy, budget.Value());
        }

        /** @brief Close preparation on shutdown or invalid host bounds, independently of provider availability. */
        [[nodiscard]] bool ValidPreparationBounds(const Detail::NavigationAssetSceneState &state) noexcept {
            return !state.closed && state.limits.maximumLiveWorlds > 0 && state.limits.maximumLiveWorlds <= 64 &&
                   state.limits.maximumReservedProviderBytes > 0;
        }

        /** @brief Transfer preparation byte pins into the unique backend lifetime owner. */
        [[nodiscard]] std::vector<Assets::AssetPayloadLease> TakeTilePins(const std::span<LoadedNavMeshAsset> assets) {
            std::vector<Assets::AssetPayloadLease> bytes;
            for (auto &asset : assets) {
                for (auto &tile : asset.tileBytes)
                    bytes.push_back(std::move(tile));
            }
            return bytes;
        }

        /** @brief Validates the complete stream selection before invoking the provider or pinning new bytes. */
        [[nodiscard]] Result<std::vector<NavigationLoadedSurface>> StreamedSurfaces(const Detail::NavigationAssetSceneState &state,
                                                                                    std::span<const NavigationStreamedSurface> selections) {
            using Surfaces = Result<std::vector<NavigationLoadedSurface>>;
            const auto &limits = state.limits.assets;
            if (selections.size() > limits.maximumPartitions)
                return Surfaces::Failure(MakeError(NavigationErrors::CapacityExceeded));
            std::vector<NavigationLoadedSurface> surfaces;
            surfaces.reserve(selections.size());
            auto remaining = limits.maximumDecodedBytes;
            for (const auto &selection : selections) {
                const auto &asset = selection.asset;
                const auto &active = *state.provenance;
                const auto root = std::ranges::find(active, asset.id, &NavigationAssetProvenance::id);
                if (root == active.end() || root->registryRevision != asset.registryRevision ||
                    root->cookedContentDigest != asset.cookedContentDigest || root->cacheKeyDigest != asset.cacheKeyDigest ||
                    root->sourceDigest != asset.sourceDigest)
                    return Surfaces::Failure(MakeError(NavigationErrors::StaleSnapshot));
                const auto &required = state.selection.requiredProfile;
                if (required && (!asset.contentProvenance || !asset.contentProvenance->projectProfile ||
                                 !asset.contentProvenance->projectProfile->MatchesAuthority(*required)))
                    return Surfaces::Failure(MakeError(NavigationErrors::ProjectProfileStale));
                if (selection.partitionIndex >= asset.partitions.size() || selection.surfaceGeneration == 0 ||
                    asset.scope != state.selection.scope)
                    return Surfaces::Failure(MakeError(NavigationErrors::NavMeshArtifactInvalid));
                const auto &partition = asset.partitions[selection.partitionIndex];
                if (partition.scope != asset.scope || partition.tiles.empty() ||
                    std::ranges::any_of(surfaces, [&partition](const auto &surface) {
                    return surface.partition->surface == partition.surface && surface.partition->profile == partition.profile;
                }))
                    return Surfaces::Failure(MakeError(NavigationErrors::NavMeshArtifactInvalid));
                for (const auto &tile : partition.tiles) {
                    if (!tile || tile->Key().surface != partition.surface || tile->Key().profile != partition.profile ||
                        tile->Key().scope != state.selection.scope)
                        return Surfaces::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
                    const auto bytes = tile->StorageBytes();
                    if (bytes > remaining || (limits.maximumBytesPerTile && bytes > *limits.maximumBytesPerTile))
                        return Surfaces::Failure(MakeError(NavigationErrors::CapacityExceeded));
                    remaining -= bytes;
                }
                surfaces.push_back({asset.id, selection.surfaceGeneration, &partition});
            }
            return Surfaces::Success(std::move(surfaces));
        }

        /** @brief Retains the canonical cache allocation for each tile captured by the detached backend. */
        [[nodiscard]] Result<std::vector<Assets::AssetPayloadLease>> PinStreamedTiles(Assets::AssetPayloadCache &cache,
                                                                                      std::span<const NavigationLoadedSurface> surfaces) {
            using Pins = Result<std::vector<Assets::AssetPayloadLease>>;
            std::vector<Assets::AssetPayloadLease> pins;
            for (const auto &surface : surfaces) {
                for (const auto &tile : surface.partition->tiles) {
                    auto pin = cache.Admit(std::as_bytes(tile->Bytes()));
                    if (pin.HasError())
                        return Pins::Failure(pin.ErrorValue());
                    pins.push_back(std::move(pin).Value());
                }
            }
            return Pins::Success(std::move(pins));
        }

        /** @brief Builds a provider against the same charged limits for both initial and streamed activation. */
        [[nodiscard]] Result<NavigationPreparedAssetBackend> PrepareProvider(const NavigationWorldActivationDescriptor &descriptor,
                                                                             std::span<const NavigationLoadedSurface> surfaces,
                                                                             const Detail::NavigationAssetSceneState &state) {
            using Provider = Result<NavigationPreparedAssetBackend>;
            const auto budget = AvailableProviderBudget(state);
            if (budget.HasError())
                return Provider::Failure(budget.ErrorValue());
            auto prepared = state.factory(descriptor, surfaces, budget.Value());
            if (prepared.HasError())
                return prepared;
            const auto &candidate = prepared.Value();
            if (!candidate.backend || candidate.reservedProviderBytes == 0 || candidate.reservedProviderBytes > budget.Value())
                return Provider::Failure(MakeError(NavigationErrors::CapacityExceeded));
            return prepared;
        }

        /** @brief Checks any declared cooked project policy against the new provider's exact capability revision. */
        [[nodiscard]] Result<void> ValidateAssetProfile(const LoadedNavMeshAsset &asset,
                                                        const NavigationProviderCapabilities &capabilities) {
            if (!asset.contentProvenance || !asset.contentProvenance->projectProfile)
                return Result<void>::Success();
            const auto admitted =
                AdmitNavigationProjectProfile(*asset.contentProvenance->projectProfile, capabilities, capabilities.revision);
            return admitted.HasValue() ? Result<void>::Success() : Result<void>::Failure(admitted.ErrorValue());
        }

        /** @brief Creates an isolated lifecycle whose complete initial state is ready to publish. */
        [[nodiscard]] Result<std::shared_ptr<NavigationWorldLifecycle>> FinalizeWorld(const NavigationWorldActivationDescriptor &descriptor,
                                                                                      std::unique_ptr<INavigationQueryBackend> backend) {
            using World = Result<std::shared_ptr<NavigationWorldLifecycle>>;
            auto lifecycle = NavigationWorldLifecycle::Create(1);
            if (lifecycle.HasError())
                return World::Failure(lifecycle.ErrorValue());
            auto world = std::make_shared<NavigationWorldLifecycle>(std::move(lifecycle).Value());
            const auto staged = backend ? world->Stage(descriptor, std::move(backend)) : world->StageInitialEmptyTopology(descriptor);
            if (staged.HasError())
                return World::Failure(staged.ErrorValue());
            if (const auto committed = world->CommitAtSafePoint(descriptor.scene, descriptor.sceneGeneration); committed.HasError())
                return World::Failure(committed.ErrorValue());
            return World::Success(std::move(world));
        }

        /** @brief Finalize a detached lifecycle; aggregate publication then has no fallible Stage/Commit operation. */
        [[nodiscard]] Result<std::shared_ptr<NavigationWorldLifecycle>> PrepareWorld(
            const NavigationWorldActivationDescriptor &descriptor, const std::span<const NavigationLoadedSurface> surfaces,
            std::vector<LoadedNavMeshAsset> &assets, Detail::NavigationAssetSceneState &state,
            std::shared_ptr<const std::vector<NavigationAssetProvenance>> provenance, RuntimeTilePins rebuildTiles) {
            if (state.selection.streamCoverage)
                return FinalizeWorld(descriptor, {});
            auto prepared = PrepareProvider(descriptor, surfaces, state);
            if (prepared.HasError())
                return Result<std::shared_ptr<NavigationWorldLifecycle>>::Failure(prepared.ErrorValue());
            const auto capabilities = prepared.Value().backend->Capabilities();
            for (const auto &asset : assets) {
                const auto admitted = ValidateAssetProfile(asset, capabilities);
                if (admitted.HasError())
                    return Result<std::shared_ptr<NavigationWorldLifecycle>>::Failure(admitted.ErrorValue());
            }
            auto bytes = TakeTilePins(assets);
            auto backend = std::make_unique<PinnedAssetBackend>(std::move(prepared).Value(), std::move(bytes), state.accounting,
                                                                std::move(provenance), std::move(rebuildTiles));
            return FinalizeWorld(descriptor, std::move(backend));
        }

        /** @brief Pins the retiring provider, allowing a previously committed empty topology. */
        [[nodiscard]] Result<NavigationWorldReadLease> PinPredecessor(const NavigationWorldLifecycle &world, NavigationWorldId id) {
            auto lease = world.Acquire(id);
            if (lease.HasError() && lease.ErrorValue().code.Value() == NavigationErrors::NoNavigationData.code.Value())
                return Result<NavigationWorldReadLease>::Success({});
            return lease;
        }

        /** @brief Uses the same accounting for the prepared successor and every active or pinned predecessor. */
        [[nodiscard]] Result<std::unique_ptr<INavigationQueryBackend>> PinRebuildStorage(Detail::RuntimeRebuildAdoption &adoption,
                                                                                         const Detail::NavigationAssetSceneState &state,
                                                                                         RebuildTiles tiles,
                                                                                         std::unique_ptr<INavigationQueryBackend> backend,
                                                                                         std::size_t reserved) {
            using Return = Result<std::unique_ptr<INavigationQueryBackend>>;
            const auto tileBytes = TileClosureBytes(tiles, reserved);
            if (tileBytes.HasError())
                return Return::Failure(tileBytes.ErrorValue());
            const auto providerBytes = reserved - tileBytes.Value();
            const auto budget = AvailableProviderBudget(state, backend != nullptr);
            if (budget.HasError() || providerBytes > budget.Value())
                return Return::Failure(MakeError(NavigationErrors::CapacityExceeded));
            const auto &policy = state.rebuildTiles->policy;
            if (backend && policy) {
                const auto capabilities = backend->Capabilities();
                if (auto admitted = AdmitNavigationProjectProfile(*policy, capabilities, capabilities.revision); admitted.HasError())
                    return Return::Failure(admitted.ErrorValue());
            }
            auto pinned =
                PinTileClosure(std::move(tiles), state.rebuildTiles->tiles, state.accounting, policy, budget.Value() - providerBytes);
            if (pinned.HasError())
                return Return::Failure(pinned.ErrorValue());
            adoption.tiles = std::move(pinned).Value();
            if (backend) {
                backend = std::make_unique<PinnedAssetBackend>(NavigationPreparedAssetBackend{std::move(backend), providerBytes},
                                                               std::vector<Assets::AssetPayloadLease>{}, state.accounting, state.provenance,
                                                               adoption.tiles);
            }
            return Return::Success(std::move(backend));
        }

        /** @brief Fences publication after observing host authority, including incidental Scene replacement. */
        [[nodiscard]] bool MatchesRuntimeRoot(const Detail::NavigationAssetSceneState &state, const NavigationWorldLifecycle &world,
                                              const Detail::NavigationAssetRebuildTiles &tiles,
                                              const NavigationWorldActivationDescriptor &expected) {
            const auto active = world.ActiveDescriptor();
            return !state.closed && state.active.get() == &world && state.rebuildTiles.get() == &tiles && active.HasValue() &&
                   active.Value() == expected;
        }

        /** @brief Checks that the expected Scene and topology still own the only publication slot. */
        [[nodiscard]] Result<void> CheckRuntimeAdoptionRoot(const Detail::NavigationAssetSceneState &state,
                                                            const NavigationWorldLifecycle &world,
                                                            const Detail::NavigationAssetRebuildTiles &tiles,
                                                            const NavigationWorldActivationDescriptor &expected) {
            if (!MatchesRuntimeRoot(state, world, tiles, expected))
                return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
            if (world.HasStagedCandidate())
                return Result<void>::Failure(MakeError(NavigationErrors::CapabilityUnavailable));
            return Result<void>::Success();
        }

        /** @brief Stages an accounted successor and transfers exact rollback authority only after staging succeeds. */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> StageRuntimeAdoption(
            const std::shared_ptr<Detail::NavigationAssetSceneState> &state, const std::shared_ptr<NavigationWorldLifecycle> &world,
            NavigationGeneration next, Detail::RuntimeRebuildAdoption adoption, std::unique_ptr<INavigationQueryBackend> backend) {
            using Return = Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>;
            auto successor = adoption.expected.activation;
            successor.topology = next;
            auto predecessor = PinPredecessor(*world, successor.world);
            if (predecessor.HasError())
                return Return::Failure(predecessor.ErrorValue());
            const auto expected = adoption.expected.activation;
            auto prepared =
                std::make_unique<NavigationStreamedTopologyPublication>(state, world, successor, std::move(predecessor).Value(),
                                                                        NavigationTopologyChange::Replacement, std::move(adoption));
            if (auto staged =
                    backend ? world->StageTopology(expected, next, std::move(backend)) : world->StageEmptyTopology(expected, next);
                staged.HasError())
                return Return::Failure(staged.ErrorValue());
            prepared->OwnStagedTopology();
            return Return::Success(std::move(prepared));
        }
    }  // namespace

    /** @copydoc NavigationAssetSceneActivationParticipant::NavigationAssetSceneActivationParticipant */
    NavigationAssetSceneActivationParticipant::NavigationAssetSceneActivationParticipant(
        Assets::AssetPayloadCache &cache, AssetCookTargetId target, NavigationAssetBackendFactory factory,
        const NavigationAssetSceneLimits &limits, std::optional<std::vector<NavMeshAssetContentExpectation>> content,
        NavigationAssetSceneSelection selection)
        : state_(std::make_shared<Detail::NavigationAssetSceneState>()) {
        state_->cache = &cache;
        state_->target = std::move(target);
        state_->factory = std::move(factory);
        state_->limits = limits;
        state_->content = std::move(content);
        state_->selection = std::move(selection);
    }

    /** @copydoc NavigationAssetSceneActivationParticipant::~NavigationAssetSceneActivationParticipant */
    NavigationAssetSceneActivationParticipant::~NavigationAssetSceneActivationParticipant() {
        Shutdown();
    }

    /** @copydoc NavigationAssetSceneActivationParticipant::Prepare */
    Result<std::unique_ptr<Runtime::SceneActivationCandidate>> NavigationAssetSceneActivationParticipant::Prepare(
        const Runtime::RuntimeSceneDefinition &definition, const Runtime::RuntimeSceneView &scene) {
        if (!ValidPreparationBounds(*state_)) {
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
        // Legacy composition keeps its existing immediate unavailable diagnostic. Strict package composition
        // first validates whether enabled authored navigation requires a provider; inert empty Scenes do not.
        if (!state_->factory && !state_->content.has_value())
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(MakeError(NavigationErrors::CapabilityUnavailable));
        const auto descriptor = Descriptor(scene);
        if (descriptor.HasError())
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(descriptor.ErrorValue());
        try {
            std::vector<LoadedNavMeshAsset> assets;
            std::vector<NavigationLoadedSurface> surfaces;
            if (const auto selected = SelectSurfaces(definition, scene, *state_, assets, surfaces); selected.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(selected.ErrorValue());
            if (state_->selection.requiredProfile && surfaces.empty())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(
                    MakeError(NavigationErrors::SceneSurfaceMissing));
            if (state_->content || state_->selection.requiredProfile) {
                if (const auto required = ValidateRequiredCoverage(definition, scene, *state_, assets, surfaces); required.HasError())
                    return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(required.ErrorValue());
                if (const auto coverage = ValidateAgentCoverage(definition, surfaces); coverage.HasError())
                    return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(coverage.ErrorValue());
            }
            auto provenance = std::make_shared<std::vector<NavigationAssetProvenance>>();
            provenance->reserve(assets.size());
            for (const auto &asset : assets) {
                provenance->push_back(
                    {asset.id, asset.registryRevision, asset.cookedContentDigest, asset.sourceDigest, asset.cacheKeyDigest});
            }
            std::shared_ptr<NavigationWorldLifecycle> world;
            auto rebuildTiles = PinInitialRebuildTiles(*state_, surfaces, assets);
            if (rebuildTiles.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(rebuildTiles.ErrorValue());
            if (!surfaces.empty()) {
                if (!state_->factory)
                    return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(
                        MakeError(NavigationErrors::CapabilityUnavailable));
                auto prepared = PrepareWorld(descriptor.Value(), surfaces, assets, *state_, provenance, rebuildTiles.Value());
                if (prepared.HasError())
                    return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(prepared.ErrorValue());
                world = std::move(prepared).Value();
            }
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(
                std::make_unique<AssetWorldCandidate>(state_, scene, std::move(world), std::move(provenance),
                                                      std::move(rebuildTiles).Value()));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc NavigationAssetSceneActivationParticipant::Acquire */
    Result<NavigationWorldReadLease> NavigationAssetSceneActivationParticipant::Acquire() const {
        if (state_->closed || !state_->active)
            return Result<NavigationWorldReadLease>::Failure(MakeError(NavigationErrors::NoNavigationData));
        const auto descriptor = state_->active->ActiveDescriptor();
        if (descriptor.HasError())
            return Result<NavigationWorldReadLease>::Failure(descriptor.ErrorValue());
        return state_->active->Acquire(descriptor.Value().world);
    }

    /** @copydoc NavigationAssetSceneActivationParticipant::ActiveWorld */
    std::shared_ptr<NavigationWorldLifecycle> NavigationAssetSceneActivationParticipant::ActiveWorld() const noexcept {
        return state_->closed ? nullptr : state_->active;
    }

    /** @copydoc NavigationAssetSceneActivationParticipant::ActiveRuntimeRebuildTiles */
    std::shared_ptr<const Detail::NavigationAssetRebuildTiles::Tiles> NavigationAssetSceneActivationParticipant::ActiveRuntimeRebuildTiles()
        const noexcept {
        const auto &pins = state_->rebuildTiles;
        return pins && !state_->closed ? std::shared_ptr<const Detail::NavigationAssetRebuildTiles::Tiles>{pins, &pins->tiles} : nullptr;
    }

    /** @copydoc NavigationAssetSceneActivationParticipant::ReserveRuntimeRebuild */
    Result<NavigationRuntimeRebuildReservation> NavigationAssetSceneActivationParticipant::ReserveRuntimeRebuild(
        const NavigationRuntimeRebuildFence &expected, const std::size_t maximumOwnedBytes) {
        using Return = Result<NavigationRuntimeRebuildReservation>;
        const auto world = ActiveWorld();
        if (!world || !state_->rebuildTiles)
            return Return::Failure(MakeError(NavigationErrors::CapabilityUnavailable));
        if (const auto active = world->ActiveDescriptor(); active.HasError() || active.Value() != expected.activation)
            return Return::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (world->HasStagedCandidate() || state_->accounting->rebuildReserved.load())
            return Return::Failure(MakeError(NavigationErrors::AdmissionRejected));
        static_cast<void>(world->CollectRetired());
        if (!FitsRebuildBudget(*state_, maximumOwnedBytes))
            return Return::Failure(MakeError(NavigationErrors::CapacityExceeded));
        try {
            NavigationRuntimeRebuildReservation reservation;
            reservation.expected_ = expected;
            reservation.maximumOwnedBytes_ = maximumOwnedBytes;
            reservation.state_ = std::make_shared<const NavigationRuntimeRebuildReservation::State>(state_->accounting, maximumOwnedBytes);
            return Return::Success(std::move(reservation));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc NavigationAssetSceneActivationParticipant::PrepareRuntimeRebuildPublication */
    Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> NavigationAssetSceneActivationParticipant::PrepareRuntimeRebuildPublication(
        NavigationRuntimeTileRebuildCandidate rebuild, NavigationRuntimeRebuildAuthority observe, CancellationToken cancellation) {
        using Return = Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>;
        const auto world = ActiveWorld();
        const auto active = state_->rebuildTiles;
        if (!world || !active)
            return Return::Failure(MakeError(NavigationErrors::CapabilityUnavailable));
        auto &reservation = rebuild.reservation_.state_;
        if (rebuild.preparedByJob_ && !NavigationRuntimeRebuildReservation::State::HeldBy(reservation, state_->accounting))
            return Return::Failure(MakeError(NavigationErrors::AdmissionRejected));
        try {
            Detail::RuntimeRebuildAdoption adoption{rebuild.expected_, std::move(observe), std::move(cancellation), {}};
            if (auto checked = adoption.CheckAuthority(); checked.HasError())
                return Return::Failure(checked.ErrorValue());
            if (auto current = CheckRuntimeAdoptionRoot(*state_, *world, *active, rebuild.expected_.activation); current.HasError())
                return Return::Failure(current.ErrorValue());
            if (!rebuild.MatchesPredecessor(active->tiles))
                return Return::Failure(MakeError(NavigationErrors::StaleSnapshot));
            static_cast<void>(world->CollectRetired());
            // Only this Scene owner can admit new work. Transfer its worker charge to actual staged allocations.
            reservation.reset();
            const auto reserved = rebuild.ReservedBytes();
            auto backend = PinRebuildStorage(adoption, *state_, std::move(rebuild.tiles_), std::move(rebuild.backend_), reserved);
            if (backend.HasError())
                return Return::Failure(backend.ErrorValue());
            return StageRuntimeAdoption(state_, world, rebuild.next_, std::move(adoption), std::move(backend).Value());
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc NavigationAssetSceneActivationParticipant::StageStreamedTopology */
    Result<void> NavigationAssetSceneActivationParticipant::StageStreamedTopology(const NavigationWorldActivationDescriptor &expected,
                                                                                  NavigationGeneration next,
                                                                                  std::span<const NavigationStreamedSurface> selections) {
        if (state_->selection.runtimeRebuild)
            return Result<void>::Failure(MakeError(NavigationErrors::OperationUnsupported));
        const auto world = ActiveWorld();
        if (!world || !state_->provenance)
            return Result<void>::Failure(MakeError(NavigationErrors::NoNavigationData));
        const auto current = world->ActiveDescriptor();
        if (current.HasError() || current.Value() != expected || next <= expected.topology)
            return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (world->HasStagedCandidate())
            return Result<void>::Failure(MakeError(NavigationErrors::CapabilityUnavailable));
        static_cast<void>(world->CollectRetired());
        if (selections.empty())
            return world->StageEmptyTopology(expected, next);
        if (!state_->factory)
            return Result<void>::Failure(MakeError(NavigationErrors::CapabilityUnavailable));
        try {
            const auto surfaces = StreamedSurfaces(*state_, selections);
            if (surfaces.HasError())
                return Result<void>::Failure(surfaces.ErrorValue());
            auto descriptor = expected;
            descriptor.topology = next;
            const auto provenance = state_->provenance;
            auto prepared = PrepareProvider(descriptor, surfaces.Value(), *state_);
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            if (world != ActiveWorld() || provenance != state_->provenance)
                return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
            const auto capabilities = prepared.Value().backend->Capabilities();
            for (const auto &selection : selections) {
                const auto admitted = ValidateAssetProfile(selection.asset, capabilities);
                if (admitted.HasError())
                    return Result<void>::Failure(admitted.ErrorValue());
            }
            auto pins = PinStreamedTiles(*state_->cache, surfaces.Value());
            if (pins.HasError())
                return Result<void>::Failure(pins.ErrorValue());
            return world->StageTopology(expected, next,
                                        std::make_unique<PinnedAssetBackend>(std::move(prepared).Value(), std::move(pins).Value(),
                                                                             state_->accounting, provenance));
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc NavigationAssetSceneActivationParticipant::PrepareStreamedPublication */
    Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> NavigationAssetSceneActivationParticipant::PrepareStreamedPublication(
        const NavigationWorldActivationDescriptor &expected, NavigationGeneration next, std::span<const NavigationStreamedSurface> surfaces,
        NavigationTopologyChange change) {
        using Candidate = Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>;
        const auto world = ActiveWorld();
        if (!world)
            return Candidate::Failure(MakeError(NavigationErrors::NoNavigationData));
        NavigationWorldReadLease predecessor;
        if (auto lease = world->Acquire(expected.world); lease.HasValue())
            predecessor = std::move(lease).Value();
        else if (lease.ErrorValue().code.Value() != NavigationErrors::NoNavigationData.code.Value())
            return Candidate::Failure(lease.ErrorValue());
        auto candidate = expected;
        candidate.topology = next;
        try {
            auto prepared =
                std::make_unique<NavigationStreamedTopologyPublication>(state_, world, candidate, std::move(predecessor), change);
            if (auto staged = StageStreamedTopology(expected, next, surfaces); staged.HasError())
                return Candidate::Failure(staged.ErrorValue());
            prepared->OwnStagedTopology();
            return Candidate::Success(std::move(prepared));
        } catch (const std::bad_alloc &) {
            return Candidate::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc NavigationAssetSceneActivationParticipant::ActiveAssetProvenance */
    std::span<const NavigationAssetProvenance> NavigationAssetSceneActivationParticipant::ActiveAssetProvenance() const noexcept {
        return state_->provenance ? std::span<const NavigationAssetProvenance>{*state_->provenance}
                                  : std::span<const NavigationAssetProvenance>{};
    }

    /** @copydoc NavigationAssetSceneActivationParticipant::Snapshot */
    NavigationAssetWorldSnapshot NavigationAssetSceneActivationParticipant::Snapshot() const noexcept {
        return {state_->accounting->worlds.load(), state_->accounting->providerBytes.load(), state_->cache->Snapshot()};
    }

    /** @copydoc NavigationAssetSceneActivationParticipant::Shutdown */
    void NavigationAssetSceneActivationParticipant::Shutdown() const noexcept {
        state_->closed = true;
        if (state_->active)
            state_->active->BeginShutdown();
        state_->active.reset();
        state_->provenance.reset();
        state_->rebuildTiles.reset();
    }
}  // namespace Horo::Navigation
