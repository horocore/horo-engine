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
            std::shared_ptr<NavigationAssetWorldAccounting> accounting{std::make_shared<NavigationAssetWorldAccounting>()};
            std::shared_ptr<NavigationWorldLifecycle> active;
            std::shared_ptr<const std::vector<NavigationAssetProvenance>> provenance;
            bool closed{};
        };
    }  // namespace Detail

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
            AssetWorldCandidate(std::shared_ptr<Detail::NavigationAssetSceneState> state, const Runtime::RuntimeSceneView scene,
                                std::shared_ptr<NavigationWorldLifecycle> world,
                                std::shared_ptr<const std::vector<NavigationAssetProvenance>> provenance)
                : state_(std::move(state)), scene_(scene), world_(std::move(world)), provenance_(std::move(provenance)) {}

            [[nodiscard]] Result<void> ValidatePublication() const override {
                if (state_->closed || !scene_.IsCurrent()) {
                    return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
                }
                return Result<void>::Success();
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
        [[nodiscard]] Result<NavigationWorldActivationDescriptor> Descriptor(const Runtime::RuntimeSceneView scene) {
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

        /** @brief Exact dependencies must already belong to the same prepared Scene closure; no query-time I/O occurs. */
        [[nodiscard]] Result<void> ValidateDependencies(const LoadedNavMeshAsset &asset, const Runtime::RuntimeSceneView scene,
                                                        const Detail::NavigationAssetSceneState &state) {
            for (const auto &dependency : asset.dependencies) {
                const auto resolved = scene.FindAsset(dependency.asset.id);
                if (!resolved || !resolved->type || *resolved->type != dependency.asset.expectedType) {
                    return Result<void>::Failure(MakeError(NavigationErrors::NoNavigationData));
                }
                const auto verified = Assets::DecodeCookedArtifact(resolved->bytes, state.limits.assets.cook);
                if (verified.HasError())
                    return Result<void>::Failure(verified.ErrorValue());
                if (verified.Value().id != dependency.asset.id || verified.Value().type != dependency.asset.expectedType ||
                    verified.Value().target != state.target ||
                    ComputeSha256(std::as_bytes(resolved->bytes)) != dependency.cookedContentDigest) {
                    return Result<void>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
                }
            }
            return Result<void>::Success();
        }

        /** @brief Resolve one canonical asset once per Scene preparation, even when multiple surfaces share it. */
        [[nodiscard]] Result<std::size_t> ResolveAsset(const Assets::AssetId id, const Runtime::RuntimeSceneView scene,
                                                       Detail::NavigationAssetSceneState &state, std::vector<LoadedNavMeshAsset> &assets) {
            if (const auto found = std::ranges::find(assets, id, &LoadedNavMeshAsset::id); found != assets.end())
                return Result<std::size_t>::Success(static_cast<std::size_t>(found - assets.begin()));
            const auto resolved = scene.FindAsset(id);
            if (!resolved || !resolved->type)
                return Result<std::size_t>::Failure(MakeError(NavigationErrors::NoNavigationData));
            const Assets::AssetDependency metadata{id, *resolved->type};
            auto loaded =
                LoadNavMeshAsset(metadata, scene.AssetRegistryRevision(), resolved->bytes, state.target, *state.cache, state.limits.assets);
            if (loaded.HasError())
                return Result<std::size_t>::Failure(loaded.ErrorValue());
            if (const auto dependencies = ValidateDependencies(loaded.Value(), scene, state); dependencies.HasError())
                return Result<std::size_t>::Failure(dependencies.ErrorValue());
            assets.push_back(std::move(loaded).Value());
            return Result<std::size_t>::Success(assets.size() - 1);
        }

        /** @brief Match every enabled surface/profile exactly; missing or stale generated partitions cannot fall back. */
        [[nodiscard]] Result<void> SelectSurfaces(const Runtime::RuntimeSceneDefinition &definition, const Runtime::RuntimeSceneView scene,
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
                        return partition.surface == surface.id && partition.surfaceGeneration == surface.generation &&
                               partition.data.Header().profile.id == profile;
                    });
                    if (found == partitions.end())
                        return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
                    surfaces.emplace_back(surface.definition, std::to_address(found));
                }
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

        /** @brief Close preparation on invalid host bounds or absent explicit composition. */
        [[nodiscard]] bool ValidState(const Detail::NavigationAssetSceneState &state) noexcept {
            return !state.closed && static_cast<bool>(state.factory) && state.limits.maximumLiveWorlds > 0 &&
                   state.limits.maximumLiveWorlds <= 64 && state.limits.maximumReservedProviderBytes > 0;
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

        /** @brief Finalize a detached lifecycle; aggregate publication then has no fallible Stage/Commit operation. */
        [[nodiscard]] Result<std::shared_ptr<NavigationWorldLifecycle>> PrepareWorld(
            const NavigationWorldActivationDescriptor &descriptor, const std::span<const NavigationLoadedSurface> surfaces,
            std::vector<LoadedNavMeshAsset> &assets, Detail::NavigationAssetSceneState &state,
            std::shared_ptr<const std::vector<NavigationAssetProvenance>> provenance) {
            const auto budget = AvailableProviderBudget(state);
            if (budget.HasError())
                return Result<std::shared_ptr<NavigationWorldLifecycle>>::Failure(budget.ErrorValue());
            const auto available = budget.Value();
            auto prepared = state.factory(descriptor, surfaces, available);
            if (prepared.HasError())
                return Result<std::shared_ptr<NavigationWorldLifecycle>>::Failure(prepared.ErrorValue());
            if (!prepared.Value().backend || prepared.Value().reservedProviderBytes == 0 ||
                prepared.Value().reservedProviderBytes > available) {
                return Result<std::shared_ptr<NavigationWorldLifecycle>>::Failure(MakeError(NavigationErrors::CapacityExceeded));
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
    NavigationAssetSceneActivationParticipant::NavigationAssetSceneActivationParticipant(Assets::AssetPayloadCache &cache,
                                                                                         AssetCookTargetId target,
                                                                                         NavigationAssetBackendFactory factory,
                                                                                         const NavigationAssetSceneLimits &limits)
        : state_(std::make_shared<Detail::NavigationAssetSceneState>()) {
        state_->cache = &cache;
        state_->target = std::move(target);
        state_->factory = std::move(factory);
        state_->limits = limits;
    }

    /** @copydoc NavigationAssetSceneActivationParticipant::~NavigationAssetSceneActivationParticipant */
    NavigationAssetSceneActivationParticipant::~NavigationAssetSceneActivationParticipant() {
        Shutdown();
    }

    /** @copydoc NavigationAssetSceneActivationParticipant::Prepare */
    Result<std::unique_ptr<Runtime::SceneActivationCandidate>> NavigationAssetSceneActivationParticipant::Prepare(
        const Runtime::RuntimeSceneDefinition &definition, const Runtime::RuntimeSceneView scene) {
        if (!ValidState(*state_)) {
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
        const auto descriptor = Descriptor(scene);
        if (descriptor.HasError())
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(descriptor.ErrorValue());
        try {
            std::vector<LoadedNavMeshAsset> assets;
            std::vector<NavigationLoadedSurface> surfaces;
            if (const auto selected = SelectSurfaces(definition, scene, *state_, assets, surfaces); selected.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(selected.ErrorValue());
            auto provenance = std::make_shared<std::vector<NavigationAssetProvenance>>();
            provenance->reserve(assets.size());
            for (const auto &asset : assets) {
                provenance->push_back(
                    {asset.id, asset.registryRevision, asset.cookedContentDigest, asset.sourceDigest, asset.cacheKeyDigest});
            }
            std::shared_ptr<NavigationWorldLifecycle> world;
            if (!surfaces.empty()) {
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
