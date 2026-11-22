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
            bool closed{};
        };
    }  // namespace Detail

    /** @brief Rolls back detached navigation work unless the containing structural transaction commits it. */
    class NavigationStreamedTopologyPublication final : public Runtime::SceneStructuralCandidate {
    public:
        NavigationStreamedTopologyPublication(std::shared_ptr<Detail::NavigationAssetSceneState> state,
                                              std::shared_ptr<NavigationWorldLifecycle> world,
                                              NavigationWorldActivationDescriptor candidate, NavigationWorldReadLease predecessor)
            : state_(std::move(state)), world_(std::move(world)), candidate_(candidate), predecessor_(std::move(predecessor)) {}

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
            return world_->ValidateStagedTopology(candidate_);
        }

        void Publish() noexcept override {
            world_->PublishValidatedTopology(candidate_);
            ownsStage_ = false;
        }

        Result<void> AfterPublication() override {
            NavigationWorldLifecycle::NotifyRetired(predecessor_);
            predecessor_ = {};
            static_cast<void>(world_->CollectRetired());
            return Result<void>::Success();
        }

    private:
        std::shared_ptr<Detail::NavigationAssetSceneState> state_;
        std::shared_ptr<NavigationWorldLifecycle> world_;
        NavigationWorldActivationDescriptor candidate_;
        NavigationWorldReadLease predecessor_; /**< Pins retirement capacity through the no-fail publication phase. */
        bool ownsStage_{};
    };

    namespace {
        /** @brief Backend lifetime, not cache residency or Scene borrows, owns immutable tile and reservation pins. */
        class PinnedAssetBackend final : public INavigationQueryBackend {
        public:
            PinnedAssetBackend(NavigationPreparedAssetBackend prepared, std::vector<Assets::AssetPayloadLease> bytes,
                               std::shared_ptr<Detail::NavigationAssetWorldAccounting> accounting,
                               std::shared_ptr<const std::vector<NavigationAssetProvenance>> provenance)
                : backend_(std::move(prepared.backend)), bytes_(std::move(bytes)), accounting_(std::move(accounting)),
                  reservation_(prepared.reservedProviderBytes), provenance_(std::move(provenance)) {
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
        };

        /** @brief Publication swaps a fully finalized detached owner, while shutdown targets only this candidate. */
        class AssetWorldCandidate final : public Runtime::SceneActivationCandidate {
        public:
            AssetWorldCandidate(std::shared_ptr<Detail::NavigationAssetSceneState> state, const Runtime::RuntimeSceneView &scene,
                                std::shared_ptr<NavigationWorldLifecycle> world,
                                std::shared_ptr<const std::vector<NavigationAssetProvenance>> provenance)
                : state_(std::move(state)), scene_(scene), world_(std::move(world)), provenance_(std::move(provenance)) {}

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
            }

            void Shutdown() noexcept override {
                if (world_)
                    world_->BeginShutdown();
                if (state_->active == world_) {
                    state_->active.reset();
                    state_->provenance.reset();
                }
                world_.reset();
            }

        private:
            std::shared_ptr<Detail::NavigationAssetSceneState> state_;
            Runtime::RuntimeSceneView scene_;
            std::shared_ptr<NavigationWorldLifecycle> world_;
            std::shared_ptr<const std::vector<NavigationAssetProvenance>> provenance_;
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
                                           {.limits = state.limits.assets, .expectation = expectation, .scope = state.selection.scope});
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
        [[nodiscard]] Result<std::size_t> AvailableProviderBudget(const Detail::NavigationAssetSceneState &state) {
            const auto retainedBytes = state.accounting->providerBytes.load();
            if (state.accounting->worlds.load() >= state.limits.maximumLiveWorlds ||
                retainedBytes >= state.limits.maximumReservedProviderBytes)
                return Result<std::size_t>::Failure(MakeError(NavigationErrors::CapacityExceeded));
            return Result<std::size_t>::Success(state.limits.maximumReservedProviderBytes - retainedBytes);
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

        /** @brief Finalize a detached lifecycle; aggregate publication then has no fallible Stage/Commit operation. */
        [[nodiscard]] Result<std::shared_ptr<NavigationWorldLifecycle>> PrepareWorld(
            const NavigationWorldActivationDescriptor &descriptor, const std::span<const NavigationLoadedSurface> surfaces,
            std::vector<LoadedNavMeshAsset> &assets, Detail::NavigationAssetSceneState &state,
            std::shared_ptr<const std::vector<NavigationAssetProvenance>> provenance) {
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
                                                                std::move(provenance));
            auto lifecycle = NavigationWorldLifecycle::Create(1);
            if (lifecycle.HasError())
                return Result<std::shared_ptr<NavigationWorldLifecycle>>::Failure(lifecycle.ErrorValue());
            auto world = std::make_shared<NavigationWorldLifecycle>(std::move(lifecycle).Value());
            if (const auto staged = world->Stage(descriptor, std::move(backend)); staged.HasError())
                return Result<std::shared_ptr<NavigationWorldLifecycle>>::Failure(staged.ErrorValue());
            if (const auto committed = world->CommitAtSafePoint(descriptor.scene, descriptor.sceneGeneration); committed.HasError())
                return Result<std::shared_ptr<NavigationWorldLifecycle>>::Failure(committed.ErrorValue());
            return Result<std::shared_ptr<NavigationWorldLifecycle>>::Success(std::move(world));
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
            if (!surfaces.empty()) {
                if (!state_->factory)
                    return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(
                        MakeError(NavigationErrors::CapabilityUnavailable));
                auto prepared = PrepareWorld(descriptor.Value(), surfaces, assets, *state_, provenance);
                if (prepared.HasError())
                    return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(prepared.ErrorValue());
                world = std::move(prepared).Value();
            }
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(
                std::make_unique<AssetWorldCandidate>(state_, scene, std::move(world), std::move(provenance)));
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

    /** @copydoc NavigationAssetSceneActivationParticipant::StageStreamedTopology */
    Result<void> NavigationAssetSceneActivationParticipant::StageStreamedTopology(const NavigationWorldActivationDescriptor &expected,
                                                                                  NavigationGeneration next,
                                                                                  std::span<const NavigationStreamedSurface> selections) {
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
        const NavigationWorldActivationDescriptor &expected, NavigationGeneration next,
        std::span<const NavigationStreamedSurface> surfaces) {
        using Candidate = Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>;
        const auto world = ActiveWorld();
        if (!world)
            return Candidate::Failure(MakeError(NavigationErrors::NoNavigationData));
        NavigationWorldReadLease predecessor;
        auto lease = world->Acquire(expected.world);
        if (lease.HasValue())
            predecessor = std::move(lease).Value();
        else if (lease.ErrorValue().code.Value() != NavigationErrors::NoNavigationData.code.Value())
            return Candidate::Failure(lease.ErrorValue());
        auto candidate = expected;
        candidate.topology = next;
        try {
            auto prepared = std::make_unique<NavigationStreamedTopologyPublication>(state_, world, candidate, std::move(predecessor));
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
    }
}  // namespace Horo::Navigation
