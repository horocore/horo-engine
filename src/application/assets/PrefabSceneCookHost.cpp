#include "PrefabSceneCookState.h"

namespace Horo::Application {
    /** @copydoc PrefabSceneCookHost::PrefabSceneCookHost */
    PrefabSceneCookHost::PrefabSceneCookHost(JobSystem &jobs, std::shared_ptr<const Assets::CookerCatalogSnapshot> catalog,
                                             const Assets::AssetRegistry &registry, const ProjectCompatibilityInspector &compatibility,
                                             Editor::ProjectMutationCoordinator &mutations,
                                             const Editor::ProjectMigrationTransactionService &migrations) noexcept
        : jobs_(jobs), catalog_(std::move(catalog)), registry_(registry), compatibility_(compatibility), mutations_(mutations),
          migrations_(migrations) {}

    /** @copydoc PrefabSceneCookHost::Cook */
    Result<Assets::AssetCookReport> PrefabSceneCookHost::Cook(const PrefabSceneCookRequest &request,
                                                              const CancellationToken &cancellation) {
        return CookImpl(request, cancellation, nullptr);
    }

    /** @copydoc PrefabSceneCookHost::CookImpl */
    Result<Assets::AssetCookReport> PrefabSceneCookHost::CookImpl(const PrefabSceneCookRequest &request,
                                                                  const CancellationToken &cancellation,
                                                                  const Release::ReleaseExecutionPlan *releasePlan) {
        if (cancellation.IsCancellationRequested())
            return Result<Assets::AssetCookReport>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
        if (!catalog_ || !request.assets.publicationFiles || !request.assets.newPublicationOperationId)
            return Result<Assets::AssetCookReport>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        auto limits = Prefab::PrefabLimitProfile::Create(request.prefabPolicy);
        if (limits.HasError())
            return Result<Assets::AssetCookReport>::Failure(limits.ErrorValue());
        auto attempt = request.assets.newPublicationOperationId();
        if (attempt.HasError())
            return Result<Assets::AssetCookReport>::Failure(attempt.ErrorValue());
        auto projectLease =
            mutations_.TryAcquire({request.assets.sourceRoot, Editor::ProjectMutationOwner::Asset, attempt.Value().ToString()});
        if (projectLease.HasError())
            return Result<Assets::AssetCookReport>::Failure(projectLease.ErrorValue());
        const auto recovery = migrations_.InspectPendingRecovery(request.assets.sourceRoot);
        if (recovery.action != Editor::MigrationRecoveryAction::None)
            return Result<Assets::AssetCookReport>::Failure(recovery.diagnostic.value_or(MakeError(PrefabSceneCookErrors::Invalid)));
        auto host = PrefabCookDetail::CaptureHost(request, compatibility_, releasePlan);
        if (host.HasError())
            return Result<Assets::AssetCookReport>::Failure(host.ErrorValue());
        auto captured = Assets::AssetCookInputSnapshot::Capture(request.assets.sourceRoot, registry_.Snapshot(), request.assets.limits,
                                                                request.maximumCapturedBytes, cancellation);
        if (captured.HasError())
            return Result<Assets::AssetCookReport>::Failure(captured.ErrorValue());
        auto inputs = std::make_shared<const Assets::AssetCookInputSnapshot>(std::move(captured).Value());
        const auto capturedRevision = inputs->Registry().Revision();
        auto composed = PrefabCookDetail::PrepareCatalog(request, host.Value(), *inputs, limits.Value(), *catalog_, cancellation);
        if (composed.HasError())
            return Result<Assets::AssetCookReport>::Failure(composed.ErrorValue());
        std::vector<Assets::AssetRecord> runtimeRecords;
        for (const auto &record : inputs->Registry().Records()) {
            if (record.type.Value() != "core.prefab")
                runtimeRecords.push_back(record);
        }
        Assets::AssetRegistry selected;
        if (selected.Publish(std::move(runtimeRecords)).status != Assets::AssetRegistryBuildStatus::Complete)
            return Result<Assets::AssetCookReport>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        Assets::AssetCookRequest cook = request.assets;
        cook.registry = selected.Snapshot();
        cook.pinnedInputs = inputs;
        // This static host owns its composition. The dynamic host explicitly supplies a dependent phase in HORO-1068.
        cook.dependentPhase.reset();
        // Capture references are synchronous: AssetCook joins all accepted work before returning, and projectLease outlives it.
        cook.validateHostInputs = [this, &request, &host, &cancellation, releasePlan, capturedRevision] {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
            if (registry_.Snapshot().Revision() != capturedRevision)
                return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Stale));
            if (auto verified = PrefabCookDetail::VerifyHost(request, host.Value(), compatibility_, releasePlan); verified.HasError())
                return verified;
            return request.assets.validateHostInputs ? request.assets.validateHostInputs() : Result<void>::Success();
        };
        Assets::AssetCookService operation{jobs_, composed.Value()};
        return operation.Cook(cook, cancellation);
    }

    /** @copydoc PrefabSceneCookHost::CookForRelease */
    Result<Release::ReleaseCookedPayload> PrefabSceneCookHost::CookForRelease(const Release::ReleaseExecutionPlan &plan,
                                                                              Release::IReleasePreflightFactsProvider &facts,
                                                                              const PrefabSceneCookRequest &request,
                                                                              const CancellationToken &cancellation) {
        if (request.assets.sourceRoot != plan.ProjectRoot())
            return Result<Release::ReleaseCookedPayload>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        PrefabSceneCookRequest releaseRequest = request;
        releaseRequest.assets.validateHostInputs = [&plan, &facts, &request] {
            auto current = facts.Capture(plan);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (!Release::ValidateReleaseInputFreeze(plan, current.Value()).empty())
                return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Stale));
            return request.assets.validateHostInputs ? request.assets.validateHostInputs() : Result<void>::Success();
        };
        auto cooked = CookImpl(releaseRequest, cancellation, &plan);
        if (cooked.HasError())
            return Result<Release::ReleaseCookedPayload>::Failure(cooked.ErrorValue());
        const auto &generation = cooked.Value().generation;
        return Result<Release::ReleaseCookedPayload>::Success({generation.generationRoot, generation.manifestDigest});
    }
}  // namespace Horo::Application
