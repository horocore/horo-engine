#include "Horo/Prefab/PrefabTemplateProvider.h"

#include "Horo/Prefab/PrefabErrors.h"
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
                                                   PrefabTemplateProviderLimits limits)
        : state_(std::make_unique<Detail::PrefabTemplateProviderState>(registry, loads, scenes, std::move(profile), limits)) {}

    /** @copydoc PrefabTemplateProvider::~PrefabTemplateProvider */
    PrefabTemplateProvider::~PrefabTemplateProvider() {
        Shutdown();
    }

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
        if (!input.asset.IsValid() || !input.target.IsValid() || input.artifactDigest == Sha256Digest{})
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
        for (const auto &entry : state_->cache) {
            if (const auto allocation = entry.resident; allocation && allocation->value.Data().assetId == request->root.asset &&
                                                        allocation->registry.Revision() == snapshot.Revision() &&
                                                        allocation->root.Digest() == request->root.artifactDigest &&
                                                        allocation->target == request->root.target) {
                request->result = allocation;
                request->status = PrefabTemplateLoadState::Succeeded;
                return Result<PrefabTemplateLoadHandle>::Success(PrefabTemplateLoadHandle(std::move(request)));
            }
        }
        // Publication precedes submission; a vector allocation failure cannot abandon accepted worker work.
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

    /** @copydoc PrefabTemplateProvider::QueuePreparedGroup */
    Result<std::vector<Runtime::DeferredEntity>> PrefabTemplateProvider::QueuePreparedGroup(
        const PrefabTemplateLease &lease, std::vector<Runtime::RuntimeComponentSet> components, const CancellationToken &cancellation,
        std::vector<std::vector<Runtime::GroupPhysicsBodyReference>> physicsReferences) {
        using Tokens = std::vector<Runtime::DeferredEntity>;
        if (auto admission = ValidateAdmission(lease); admission.HasError())
            return Result<Tokens>::Failure(admission.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<Tokens>::Failure(MakeError(PrefabErrors::Cancelled));
        const auto &entities = lease.Template()->Data().entities;
        if (components.size() != entities.size() || (!physicsReferences.empty() && physicsReferences.size() != entities.size()))
            return Result<Tokens>::Failure(MakeError(PrefabErrors::ComponentAllocationFailed));
        std::vector<Runtime::RuntimeEntityGroupEntry> entries;
        entries.reserve(entities.size());
        for (std::size_t index = 0; index < entities.size(); ++index) {
            Runtime::RuntimeEntityGroupEntry entry;
            entry.info.localTransform = entities[index].localTransform;
            entry.info.components = std::move(components[index]);
            if (!physicsReferences.empty())
                entry.physicsReferences = std::move(physicsReferences[index]);
            if (entities[index].parent)
                entry.parentInGroup = entities[index].parent->value;
            entries.push_back(std::move(entry));
        }
        std::vector<Runtime::RuntimeGroupAssetLease> resources;
        resources.reserve(lease.Dependencies().size() + 1);
        resources.emplace_back(Assets::AssetDependency{lease.Template()->Data().assetId, Assets::AssetTypeId::Parse("core.prefab").Value()},
                               lease.Artifact());
        for (const auto &dependency : lease.Dependencies())
            resources.emplace_back(dependency.metadata, dependency.artifact);
        Runtime::SceneCommandBuffer commands;
        auto tokens = commands.CreateGroup(std::move(entries), std::move(resources),
                                           {lease.Scene(), lease.RegistryRevision(), cancellation, state_->retirement.Token()});
        if (tokens.HasError())
            return tokens;
        if (auto queued = state_->scenes.QueueStructuralCommands(std::move(commands)); queued.HasError())
            return Result<Tokens>::Failure(queued.ErrorValue());
        return tokens;
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
