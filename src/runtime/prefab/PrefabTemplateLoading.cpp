#include "Horo/Assets/AssetCook.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabTemplateProviderInternal.h"

#include <algorithm>

namespace Horo::Prefab::Detail {
    namespace {
        /** @brief Distinguishes live worker admission from a completed unconsumed handle. */
        bool InFlight(const Assets::AssetLoadHandle &handle) noexcept {
            const auto state = handle.State();
            return state == Assets::AssetLoadState::Queued || state == Assets::AssetLoadState::Running;
        }

        /** @brief Verifies the exact published envelope before any payload or resource pin is admitted. */
        Result<Assets::AssetCookArtifact> Verify(const Assets::AssetLoadResult &loaded, const PrefabTemplateRequest &request,
                                                 const Assets::AssetDependency &expected, const Sha256Digest &digest,
                                                 std::size_t maximumBytes) {
            if (loaded.id != expected.id || loaded.sourceRegistryRevision != request.registry.Revision())
                return Result<Assets::AssetCookArtifact>::Failure(MakeError(PrefabErrors::ResolutionStale));
            if (loaded.bytes.size() > maximumBytes)
                return Result<Assets::AssetCookArtifact>::Failure(MakeError(PrefabErrors::CookPayloadTooLarge));
            if (ComputeSha256(std::as_bytes(std::span{loaded.bytes})) != digest)
                return Result<Assets::AssetCookArtifact>::Failure(
                    MakeError(PrefabErrors::CorruptedPayload, "Published artifact digest differs."));
            auto artifact = Assets::DecodeCookedArtifact(loaded.bytes, {.maximumArtifactBytes = maximumBytes});
            if (artifact.HasError())
                return artifact;
            if (artifact.Value().id != expected.id || artifact.Value().type != expected.expectedType)
                return Result<Assets::AssetCookArtifact>::Failure(MakeError(PrefabErrors::DependencyTypeMismatch));
            if (artifact.Value().target != request.root.target)
                return Result<Assets::AssetCookArtifact>::Failure(MakeError(PrefabErrors::CookArtifactInvalid, "Cook target differs."));
            return artifact;
        }
    }  // namespace

    /** @copydoc PrefabTemplateProviderState::PrefabTemplateProviderState */
    PrefabTemplateProviderState::PrefabTemplateProviderState(Assets::AssetRegistry &assetRegistry, Assets::AssetLoadService &assetLoads,
                                                             Runtime::RuntimeSceneService &sceneService, PrefabLimitProfile capturedProfile,
                                                             PrefabTemplateProviderLimits capacities)
        : registry(assetRegistry), loads(assetLoads), scenes(sceneService), profile(std::move(capturedProfile)), limits(capacities) {
        if (!limits.IsValid())
            return;
        const auto entries = limits.maximumTemplates * (profile.Policy().maximumReferencedAssets + 1);
        auto cacheResult = Assets::AssetPayloadCache::Create(entries, limits.maximumRetainedBytes);
        if (cacheResult.HasValue())
            payloads = std::move(cacheResult).Value();
        requests.reserve(limits.maximumOutstanding);
        cache.reserve(limits.maximumTemplates);
    }

    /** @copydoc PrefabTemplateProviderState::CheckOwner */
    Result<void> PrefabTemplateProviderState::CheckOwner() const {
        if (std::this_thread::get_id() != owner)
            return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Prefab operation requires the host owner lane."));
        return Result<void>::Success();
    }

    /** @copydoc PrefabTemplateProviderState::CheckAdmission */
    Result<void> PrefabTemplateProviderState::CheckAdmission(const Assets::AssetRegistrySnapshot &snapshot,
                                                             Runtime::SceneRuntimeId scene) const {
        if (closed || !payloads)
            return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Prefab provider is closed or invalid."));
        const auto active = scenes.ActiveScene();
        if (!active || active->RuntimeId() != scene)
            return Result<void>::Failure(MakeError(PrefabErrors::SceneUnavailable));
        if (snapshot.Revision().value == 0 || registry.Snapshot().Revision() != snapshot.Revision())
            return Result<void>::Failure(MakeError(PrefabErrors::ResolutionStale));
        return Result<void>::Success();
    }

    /** @copydoc PrefabTemplateProviderState::EvictAllocation */
    void PrefabTemplateProviderState::EvictAllocation(const PrefabTemplateAllocation &allocation) noexcept {
        payloads->Evict(allocation.root.Digest());
        for (const auto &dependency : allocation.dependencies)
            payloads->Evict(dependency.artifact.Digest());
    }

    /** @copydoc PrefabTemplateProviderState::DropResidency */
    void PrefabTemplateProviderState::DropResidency() noexcept {
        for (auto &entry : cache) {
            if (entry.resident)
                EvictAllocation(*entry.resident);
            entry.resident.reset();
        }
    }

    /** @copydoc PrefabTemplateProviderState::ReleasePins */
    void PrefabTemplateProviderState::ReleasePins(PrefabTemplateRequest &request) noexcept {
        if (payloads) {
            payloads->Evict(request.rootLease.Digest());
            for (const auto &dependency : request.dependencies)
                payloads->Evict(dependency.artifact.Digest());
        }
        request.rootLease = {};
        request.dependencies.clear();
        request.decoded.reset();
        request.result.reset();
    }

    /** @copydoc PrefabTemplateProviderState::Schedule */
    Result<void> PrefabTemplateProviderState::Schedule(PrefabTemplateRequest &request) {
        const auto id = request.decoded ? request.decoded->Data().dependencies[request.dependencies.size()].asset.id : request.root.asset;
        auto loaded = loads.LoadAsync(request.registry, id, request.cancellation.Token());
        if (loaded.HasError())
            return Result<void>::Failure(std::move(loaded).ErrorValue());
        request.pending.emplace(std::move(loaded).Value());
        return Result<void>::Success();
    }

    /** @copydoc PrefabTemplateProviderState::Consume */
    Result<void> PrefabTemplateProviderState::Consume(PrefabTemplateRequest &request) {
        auto loaded = request.pending->TakeResult();
        request.pending.reset();
        if (loaded.HasError())
            return Result<void>::Failure(std::move(loaded).ErrorValue());
        const auto *record = request.registry.Find(request.root.asset);
        const auto expected = request.decoded ? request.decoded->Data().dependencies[request.dependencies.size()].asset
                                              : Assets::AssetDependency{request.root.asset, record->type};
        const auto digest = request.decoded ? request.decoded->Data().dependencies[request.dependencies.size()].artifactDigest
                                            : request.root.artifactDigest;
        auto artifact = Verify(loaded.Value(), request, expected, digest, limits.maximumArtifactBytes);
        if (artifact.HasError())
            return Result<void>::Failure(std::move(artifact).ErrorValue());
        if (!request.decoded) {
            std::erase_if(cache, [](const auto &entry) {
                return entry.allocation.expired();
            });
            const auto preparing = std::count_if(requests.begin(), requests.end(), [](const auto &candidate) {
                return candidate->decoded.has_value();
            });
            if (cache.size() + static_cast<std::size_t>(preparing) >= limits.maximumTemplates)
                return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Live template allocation capacity exhausted."));
            auto decoded = CookedPrefab::Parse(std::as_bytes(std::span{artifact.Value().payload}), request.root.asset, profile);
            if (decoded.HasError())
                return Result<void>::Failure(std::move(decoded).ErrorValue());
            for (const auto &dependency : decoded.Value().Data().dependencies) {
                const auto *dependencyRecord = request.registry.Find(dependency.asset.id);
                if (!dependencyRecord)
                    return Result<void>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
                if (dependencyRecord->type != dependency.asset.expectedType)
                    return Result<void>::Failure(MakeError(PrefabErrors::DependencyTypeMismatch));
            }
            request.dependencies.reserve(decoded.Value().Data().dependencies.size());
            auto lease = payloads->Admit(std::as_bytes(std::span{loaded.Value().bytes}));
            if (lease.HasError())
                return Result<void>::Failure(std::move(lease).ErrorValue());
            request.rootLease = std::move(lease).Value();
            request.decoded.emplace(std::move(decoded).Value());
        } else {
            auto lease = payloads->Admit(std::as_bytes(std::span{loaded.Value().bytes}));
            if (lease.HasError())
                return Result<void>::Failure(std::move(lease).ErrorValue());
            request.dependencies.push_back({expected, std::move(lease).Value()});
        }
        return Result<void>::Success();
    }

    /** @copydoc PrefabTemplateProviderState::Publish */
    Result<void> PrefabTemplateProviderState::Publish(PrefabTemplateRequest &request) {
        if (auto admission = CheckAdmission(request.registry, request.scene); admission.HasError())
            return admission;
        auto allocation = std::make_shared<const PrefabTemplateAllocation>(
            PrefabTemplateAllocation{std::move(*request.decoded), request.registry, request.rootLease, std::move(request.dependencies),
                                     request.root.target, identity});
        cache.push_back({allocation, allocation});  // Reserved; no throwing publication after the owned candidate is complete.
        request.result = std::move(allocation);
        request.decoded.reset();
        request.rootLease = {};
        request.status = PrefabTemplateLoadState::Succeeded;
        return Result<void>::Success();
    }

    /** @copydoc PrefabTemplateProviderState::Pump */
    Result<void> PrefabTemplateProviderState::Pump() {
        if (closed || !payloads)
            return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected));
        const auto scene = scenes.ActiveScene();
        const auto incarnation = scene ? scene->RuntimeId() : Runtime::SceneRuntimeId{};
        const auto revision = registry.Snapshot().Revision();
        if (incarnation != activeScene || revision != activeRevision) {
            DropResidency();
            activeScene = incarnation;
            activeRevision = revision;
        }
        std::size_t inFlight{};
        for (const auto &request : requests)
            if (request->pending && InFlight(*request->pending))
                ++inFlight;
        for (auto &request : requests) {
            // Abandonment is not a capacity refund while a cancelled worker still occupies a load slot.
            if (request.use_count() == 1)
                request->cancellation.RequestCancellation();
            if (!request->error) {
                if (auto admission = CheckAdmission(request->registry, request->scene); admission.HasError()) {
                    request->error = std::move(admission).ErrorValue();
                    request->cancellation.RequestCancellation();
                }
            }
            if (request->cancellation.Token().IsCancellationRequested()) {
                if (request->pending && InFlight(*request->pending))
                    continue;
                request->pending.reset();
                ReleasePins(*request);
                if (!request->error)
                    request->error = MakeError(PrefabErrors::Cancelled);
                request->status = PrefabTemplateLoadState::Cancelled;
                continue;
            }
            Result<void> progress = Result<void>::Success();
            try {
                if (request->pending && !InFlight(*request->pending))
                    progress = Consume(*request);
                if (progress.HasValue() && request->decoded && request->dependencies.size() == request->decoded->Data().dependencies.size())
                    progress = Publish(*request);
                if (progress.HasValue() && request->status == PrefabTemplateLoadState::Loading && !request->pending &&
                    inFlight < limits.maximumConcurrentLoads) {
                    progress = Schedule(*request);
                    if (progress.HasValue())
                        ++inFlight;
                }
            } catch (const std::bad_alloc &) {
                progress = Result<void>::Failure(MakeError(PrefabErrors::ComponentAllocationFailed));
            }
            if (progress.HasError()) {
                request->error = std::move(progress).ErrorValue();
                request->cancellation.RequestCancellation();
                ReleasePins(*request);
                request->status = PrefabTemplateLoadState::Failed;
            }
        }
        std::erase_if(requests, [](const auto &request) {
            return request->status != PrefabTemplateLoadState::Loading;
        });
        return Result<void>::Success();
    }
}  // namespace Horo::Prefab::Detail
