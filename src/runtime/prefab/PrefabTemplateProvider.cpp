#include "Horo/Prefab/PrefabTemplateProvider.h"

#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PreparedPrefabSpawn.h"
#include "PrefabPreparedGroup.h"
#include "PrefabTemplateProviderInternal.h"

#include <algorithm>
#include <utility>

namespace Horo::Prefab {
    /** @copydoc PrefabTemplateLease::PrefabTemplateLease */
    PrefabTemplateLease::PrefabTemplateLease(std::shared_ptr<const Detail::PrefabTemplateAllocation> allocation,
                                             Runtime::SceneRuntimeId scene) noexcept
        : allocation_(std::move(allocation)), scene_(scene) {}

    /** @copydoc PrefabTemplateLease::Template */
    const CookedPrefab *PrefabTemplateLease::Template() const noexcept {
        return allocation_ ? &allocation_->value : nullptr;
    }

    /** @copydoc PrefabTemplateLease::RegistryRevision */
    Assets::AssetRegistryRevision PrefabTemplateLease::RegistryRevision() const noexcept {
        return allocation_ ? allocation_->registry.Revision() : Assets::AssetRegistryRevision{};
    }

    /** @copydoc PrefabTemplateLease::Scene */
    Runtime::SceneRuntimeId PrefabTemplateLease::Scene() const noexcept {
        return scene_;
    }

    /** @copydoc PrefabTemplateLease::Artifact */
    Assets::AssetPayloadLease PrefabTemplateLease::Artifact() const noexcept {
        return allocation_ ? allocation_->root : Assets::AssetPayloadLease{};
    }

    /** @copydoc PrefabTemplateLease::Target */
    const AssetCookTargetId *PrefabTemplateLease::Target() const noexcept {
        return allocation_ ? &allocation_->target : nullptr;
    }

    /** @copydoc PrefabTemplateLease::Dependencies */
    std::span<const PrefabTemplateDependencyLease> PrefabTemplateLease::Dependencies() const noexcept {
        return allocation_ ? std::span<const PrefabTemplateDependencyLease>{allocation_->dependencies}
                           : std::span<const PrefabTemplateDependencyLease>{};
    }

    /** @copydoc PrefabTemplateProviderLimits::IsValid */
    bool PrefabTemplateProviderLimits::IsValid() const noexcept {
        return maximumOutstanding > 0 && maximumOutstanding <= 1024 && maximumConcurrentLoads > 0 &&
               maximumConcurrentLoads <= maximumOutstanding && maximumTemplates > 0 && maximumTemplates <= 128 &&
               maximumArtifactBytes > 0 && maximumArtifactBytes <= 256U * 1024U * 1024U && maximumRetainedBytes >= maximumArtifactBytes;
    }

    /** @copydoc PrefabTemplateLoadHandle::PrefabTemplateLoadHandle */
    PrefabTemplateLoadHandle::PrefabTemplateLoadHandle(std::shared_ptr<Detail::PrefabTemplateRequest> request) noexcept
        : request_(std::move(request)) {}

    /** @copydoc PrefabTemplateLoadHandle::~PrefabTemplateLoadHandle */
    PrefabTemplateLoadHandle::~PrefabTemplateLoadHandle() {
        RequestCancel();
    }

    /** @copydoc PrefabTemplateLoadHandle::operator= */
    PrefabTemplateLoadHandle &PrefabTemplateLoadHandle::operator=(PrefabTemplateLoadHandle &&other) noexcept {
        if (this != &other) {
            RequestCancel();
            request_ = std::move(other.request_);
        }
        return *this;
    }

    /** @copydoc PrefabTemplateLoadHandle::State */
    PrefabTemplateLoadState PrefabTemplateLoadHandle::State() const noexcept {
        return request_ ? request_->status : PrefabTemplateLoadState::Failed;
    }

    /** @copydoc PrefabTemplateLoadHandle::RequestCancel */
    void PrefabTemplateLoadHandle::RequestCancel() const noexcept {
        if (request_)
            request_->cancellation.RequestCancellation();
    }

    /** @copydoc PrefabTemplateProvider::PrefabTemplateProvider */
    PrefabTemplateProvider::PrefabTemplateProvider(Assets::AssetRegistry &registry, Assets::AssetLoadService &loads,
                                                   Runtime::RuntimeSceneService &scenes, PrefabLimitProfile profile,
                                                   PrefabTemplateProviderLimits limits, PrefabMetricHandles metrics)
        : state_(std::make_unique<Detail::PrefabTemplateProviderState>(registry, loads, scenes, std::move(profile), limits,
                                                                       std::move(metrics))) {}

    /** @copydoc PrefabTemplateProvider::LimitProfile */
    const PrefabLimitProfile &PrefabTemplateProvider::LimitProfile() const noexcept {
        return state_->profile;
    }

    /** @copydoc PrefabTemplateProvider::Metrics */
    const PrefabMetricHandles &PrefabTemplateProvider::Metrics() const noexcept {
        return state_->metrics;
    }

    /** @copydoc PrefabTemplateProvider::~PrefabTemplateProvider */
    PrefabTemplateProvider::~PrefabTemplateProvider() {
        Shutdown();
    }

    namespace {
        /** @brief Pure exact request identity check before provider/catalog admission. */
        bool ValidLoadIdentity(const PrefabTemplateLoadRequest &input) noexcept {
            return input.asset.IsValid() && input.target.IsValid() && input.artifactDigest != Sha256Digest{};
        }

        /** @brief Reads the same canonical residency table without creating a second cache or authority. */
        std::shared_ptr<const Detail::PrefabTemplateAllocation> FindCached(std::span<const Detail::PrefabTemplateCacheEntry> entries,
                                                                           const Detail::PrefabTemplateRequest &request) {
            for (const auto &entry : entries)
                if (const auto &allocation = entry.resident; allocation && allocation->value.Data().assetId == request.root.asset &&
                                                             allocation->registry.Revision() == request.registry.Revision() &&
                                                             allocation->root.Digest() == request.root.artifactDigest &&
                                                             allocation->target == request.root.target)
                    return allocation;
            return {};
        }
    }  // namespace

    /** @copydoc PrefabTemplateProvider::LoadAsync */
    Result<PrefabTemplateLoadHandle> PrefabTemplateProvider::LoadAsync(PrefabTemplateLoadRequest input,
                                                                       const CancellationToken &cancellation) {
        if (auto owner = state_->CheckOwner(); owner.HasError())
            return Result<PrefabTemplateLoadHandle>::Failure(owner.ErrorValue());
        const auto snapshot = state_->registry.Snapshot();
        const auto scene = state_->scenes.ActiveScene();
        if (!scene)
            return Result<PrefabTemplateLoadHandle>::Failure(MakeError(PrefabErrors::SceneUnavailable));
        if (auto admission = state_->CheckAdmission(snapshot, scene->RuntimeId()); admission.HasError())
            return Result<PrefabTemplateLoadHandle>::Failure(admission.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<PrefabTemplateLoadHandle>::Failure(MakeError(PrefabErrors::Cancelled));
        if (!ValidLoadIdentity(input))
            return Result<PrefabTemplateLoadHandle>::Failure(MakeError(PrefabErrors::IdentityInvalid));
        const auto *record = snapshot.Find(input.asset);
        if (!record)
            return Result<PrefabTemplateLoadHandle>::Failure(MakeError(PrefabErrors::AssetNotFound));
        if (record->type.Value() != "core.prefab")
            return Result<PrefabTemplateLoadHandle>::Failure(MakeError(PrefabErrors::DependencyTypeMismatch));
        if (state_->requests.size() >= state_->limits.maximumOutstanding)
            return Result<PrefabTemplateLoadHandle>::Failure(
                MakeError(PrefabErrors::AdmissionRejected, "Prefab request capacity exhausted."));

        auto request =
            std::make_shared<Detail::PrefabTemplateRequest>(std::move(input), snapshot, scene->RuntimeId(), cancellation, state_->identity);
        if (auto allocation = FindCached(state_->cache, *request)) {
            request->result = std::move(allocation);
            request->status = PrefabTemplateLoadState::Succeeded;
            return Result<PrefabTemplateLoadHandle>::Success(PrefabTemplateLoadHandle(std::move(request)));
        }
        // Publication precedes submission; a vector allocation failure cannot abandon
        // accepted worker work.
        state_->requests.push_back(request);
        return Result<PrefabTemplateLoadHandle>::Success(PrefabTemplateLoadHandle(std::move(request)));
    }

    /** @copydoc PrefabTemplateProvider::Advance */
    Result<void> PrefabTemplateProvider::Advance() {
        if (auto owner = state_->CheckOwner(); owner.HasError())
            return owner;
        return state_->Pump();
    }

    /** @copydoc PrefabTemplateProvider::TakeResult */
    Result<PrefabTemplateLease> PrefabTemplateProvider::TakeResult(const PrefabTemplateLoadHandle &handle) const {
        if (auto owner = state_->CheckOwner(); owner.HasError())
            return Result<PrefabTemplateLease>::Failure(owner.ErrorValue());
        const auto &request = handle.request_;
        if (!request || request->identity != state_->identity || request->consumed)
            return Result<PrefabTemplateLease>::Failure(MakeError(PrefabErrors::AdmissionRejected));
        if (request->error)
            return Result<PrefabTemplateLease>::Failure(*request->error);
        if (request->status != PrefabTemplateLoadState::Succeeded)
            return Result<PrefabTemplateLease>::Failure(MakeError(PrefabErrors::AssetNotLoaded));
        if (request->cancellation.Token().IsCancellationRequested())
            return Result<PrefabTemplateLease>::Failure(MakeError(PrefabErrors::Cancelled));
        PrefabTemplateLease lease(request->result, request->scene);
        if (auto admission = ValidateAdmission(lease); admission.HasError())
            return Result<PrefabTemplateLease>::Failure(admission.ErrorValue());
        request->consumed = true;
        request->result.reset();
        return Result<PrefabTemplateLease>::Success(std::move(lease));
    }

    /** @copydoc PrefabTemplateProvider::ValidateAdmission */
    Result<void> PrefabTemplateProvider::ValidateAdmission(const PrefabTemplateLease &lease) const {
        if (auto owner = state_->CheckOwner(); owner.HasError())
            return owner;
        if (!lease.allocation_ || lease.allocation_->identity != state_->identity)
            return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected));
        return state_->CheckAdmission(lease.allocation_->registry, lease.scene_);
    }

    /** @copydoc PrefabTemplateProvider::UsesSceneService */
    bool PrefabTemplateProvider::UsesSceneService(const Runtime::RuntimeSceneService &scenes) const noexcept {
        return &state_->scenes == &scenes;
    }

    namespace {
        /** @brief Validates copied cancellation, lineage and placement without constructing a Scene command. */
        Result<void> ValidateGroupOptions(const PrefabPreparedGroupOptions &options, const CancellationToken &cancellation,
                                          std::optional<Runtime::RuntimeSceneView> scene, std::size_t maximumDepth) {
            if (cancellation.IsCancellationRequested() || options.scopeCancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
            if (options.spawnLineage.size() > maximumDepth)
                return Result<void>::Failure(MakeError(PrefabErrors::SpawnDepthExceeded));
            if (options.rootPlacement && options.rootPlacement->TryToMatrix().HasError())
                return Result<void>::Failure(MakeError(PrefabErrors::InvalidPlacement));
            if (options.parent) {
                const auto active = scene;
                if (!active || active->Get(*options.parent).HasError())
                    return Result<void>::Failure(MakeError(PrefabErrors::InvalidParent));
            }
            return Result<void>::Success();
        }

        /** @brief Every optional dense projection is either absent or covers the exact complete entity table. */
        Result<void> ValidateGroupDimensions(const PrefabPreparedGroupOptions &options, std::size_t componentCount,
                                             std::size_t entityCount) {
            if (componentCount != entityCount || (!options.physicsReferences.empty() && options.physicsReferences.size() != entityCount) ||
                (!options.members.empty() && options.members.size() != entityCount) ||
                (!options.references.empty() && options.references.size() != entityCount) ||
                (!options.primitiveMeshes.empty() && options.primitiveMeshes.size() != entityCount))
                return Result<void>::Failure(MakeError(PrefabErrors::ComponentAllocationFailed));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PrefabTemplateProvider::QueuePreparedGroup */
    Result<std::vector<Runtime::DeferredEntity>> PrefabTemplateProvider::QueuePreparedGroup(
        const PrefabTemplateLease &lease, std::vector<Runtime::RuntimeComponentSet> components, const CancellationToken &cancellation,
        PrefabPreparedGroupOptions options) {
        return QueuePreparedGroup(PreparedPrefabSpawn{lease}, std::move(components), cancellation, std::move(options));
    }

    /** @copydoc PrefabTemplateProvider::QueuePreparedGroup */
    Result<std::vector<Runtime::DeferredEntity>> PrefabTemplateProvider::QueuePreparedGroup(
        const PreparedPrefabSpawn &preparedTemplate, std::vector<Runtime::RuntimeComponentSet> components,
        const CancellationToken &cancellation, PrefabPreparedGroupOptions options) {
        const auto &lease = preparedTemplate.Source();
        using Tokens = std::vector<Runtime::DeferredEntity>;
        if (auto admission = ValidateAdmission(lease); admission.HasError())
            return Result<Tokens>::Failure(admission.ErrorValue());
        if (auto valid = ValidateGroupOptions(options, cancellation, state_->scenes.ActiveScene(),
                                              state_->profile.Policy().maximumRuntimeSpawnDepth);
            valid.HasError())
            return Result<Tokens>::Failure(valid.ErrorValue());
        if (auto valid = ValidateGroupDimensions(options, components.size(), preparedTemplate.Data().entities.size()); valid.HasError())
            return Result<Tokens>::Failure(valid.ErrorValue());
        const auto receipt = options.receipt;
        const auto scopeCancellation = options.scopeCancellation;
        auto prepared = Detail::PreparePrefabGroupCommands(preparedTemplate, std::move(components), std::move(options),
                                                           {lease.Scene(), lease.RegistryRevision(), cancellation,
                                                            state_->retirement.Token(), scopeCancellation});
        if (prepared.HasError())
            return Result<Tokens>::Failure(prepared.ErrorValue());
        auto group = std::move(prepared).Value();
        if (receipt) {
            auto queued = state_->scenes.QueueTrackedStructuralCommands(std::move(group.commands));
            if (queued.HasError())
                return Result<Tokens>::Failure(queued.ErrorValue());
            *receipt = std::move(queued).Value();
        } else if (auto queued = state_->scenes.QueueStructuralCommands(std::move(group.commands)); queued.HasError())
            return Result<Tokens>::Failure(queued.ErrorValue());
        return Result<Tokens>::Success(std::move(group.entities));
    }

    /** @copydoc PrefabTemplateProvider::Evict */
    void PrefabTemplateProvider::Evict(Assets::AssetId asset) noexcept {
        for (auto &entry : state_->cache) {
            if (entry.resident && entry.resident->value.Data().assetId == asset) {
                state_->EvictAllocation(*entry.resident);
                entry.resident.reset();
            }
        }
    }

    /** @copydoc PrefabTemplateProvider::PayloadSnapshot */
    Assets::AssetPayloadCacheSnapshot PrefabTemplateProvider::PayloadSnapshot() const noexcept {
        return state_->payloads ? state_->payloads->Snapshot() : Assets::AssetPayloadCacheSnapshot{};
    }

    /** @copydoc PrefabTemplateProvider::Startup */
    Result<void> PrefabTemplateProvider::Startup(const CancellationToken &cancellation) {
        if (auto owner = state_->CheckOwner(); owner.HasError())
            return owner;
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
        if (!state_->payloads || state_->closed)
            return Result<void>::Failure(MakeError(PrefabErrors::LimitProfileInvalid));
        return Result<void>::Success();
    }

    /** @copydoc PrefabTemplateProvider::OnPhase */
    Result<void> PrefabTemplateProvider::OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &) {
        return phase == Runtime::RuntimePhase::CommitDeferredLifecycleChanges ? Advance() : Result<void>::Success();
    }

    /** @copydoc PrefabTemplateProvider::OnFixedUpdate */
    Result<void> PrefabTemplateProvider::OnFixedUpdate(const Runtime::FixedStepContext &) {
        return Result<void>::Success();
    }

    /** @copydoc PrefabTemplateProvider::Shutdown */
    void PrefabTemplateProvider::Shutdown() noexcept {
        state_->closed = true;
        state_->retirement.RequestCancellation();
        for (const auto &request : state_->requests) {
            request->cancellation.RequestCancellation();
            request->status = PrefabTemplateLoadState::Cancelled;
            state_->ReleasePins(*request);
        }
        state_->requests.clear();
        state_->DropResidency();
        if (state_->payloads)
            state_->payloads->Shutdown();
    }
}  // namespace Horo::Prefab
