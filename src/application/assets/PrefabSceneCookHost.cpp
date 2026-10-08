#include "PrefabSceneCookState.h"

#include <algorithm>

namespace Horo::Application {
    namespace {
        /** @brief Selects resources and explicit template roots without changing captured source closure or revision authority. */
        Result<Assets::AssetRegistrySnapshot> SelectRuntimeRecords(const Assets::AssetCookInputSnapshot &inputs,
                                                                   const std::span<const Assets::AssetId> runtimeRoots) {
            std::vector<Assets::AssetRecord> runtimeRecords;
            for (const auto &record : inputs.Registry().Records()) {
                if (record.type.Value() != "core.prefab" || std::ranges::find(runtimeRoots, record.id) != runtimeRoots.end())
                    runtimeRecords.push_back(record);
            }
            Assets::AssetRegistry selected;
            if (selected.Publish(std::move(runtimeRecords)).status != Assets::AssetRegistryBuildStatus::Complete)
                return Result<Assets::AssetRegistrySnapshot>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            return Result<Assets::AssetRegistrySnapshot>::Success(selected.Snapshot());
        }

        /** @brief Borrows capture authorities only during the joined cook; rechecks the live registry and host before publication. */
        Result<void> ValidateCurrentHost(const PrefabSceneCookRequest &request, const Assets::AssetRegistry &registry,
                                         const ProjectCompatibilityInspector &compatibility, const PrefabCookDetail::HostCapture &capture,
                                         const CancellationToken &cancellation, const Release::ReleaseExecutionPlan *releasePlan,
                                         const Assets::AssetRegistryRevision capturedRevision) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
            if (registry.Snapshot().Revision() != capturedRevision)
                return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Stale));
            if (auto verified = PrefabCookDetail::VerifyHost(request, capture, compatibility, releasePlan); verified.HasError())
                return verified;
            return request.assets.validateHostInputs ? request.assets.validateHostInputs() : Result<void>::Success();
        }
    }  // namespace

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
        if (!catalog_ || !request.assets.publicationFiles || !request.assets.newPublicationOperationId ||
            request.runtimePrefabRoots.size() > request.assets.limits.maximumAssets)
            return Result<Assets::AssetCookReport>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        auto limits = Prefab::PrefabLimitProfile::Create(request.prefabPolicy);
        if (limits.HasError())
            return Result<Assets::AssetCookReport>::Failure(limits.ErrorValue());
        auto attempt = request.assets.newPublicationOperationId();
        if (attempt.HasError())
            return Result<Assets::AssetCookReport>::Failure(attempt.ErrorValue());
        if (auto projectLease =
                mutations_.TryAcquire({request.assets.sourceRoot, Editor::ProjectMutationOwner::Asset, attempt.Value().ToString()});
            projectLease.HasError())
            return Result<Assets::AssetCookReport>::Failure(projectLease.ErrorValue());
        else
            return CookWithProjectLease(request, limits.Value(), cancellation, releasePlan, std::move(projectLease).Value());
    }

    /** @copydoc PrefabSceneCookHost::CookWithProjectLease */
    Result<Assets::AssetCookReport> PrefabSceneCookHost::CookWithProjectLease(const PrefabSceneCookRequest &request,
                                                                              const Prefab::PrefabLimitProfile &limits,
                                                                              const CancellationToken &cancellation,
                                                                              const Release::ReleaseExecutionPlan *releasePlan,
                                                                              Editor::ProjectMutationLease) {
        if (const auto recovery = migrations_.InspectPendingRecovery(request.assets.sourceRoot);
            recovery.action != Editor::MigrationRecoveryAction::None)
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
        auto composed = PrefabCookDetail::PrepareCatalog(request, host.Value(), *inputs, limits, *catalog_, cancellation);
        if (composed.HasError())
            return Result<Assets::AssetCookReport>::Failure(composed.ErrorValue());
        auto selected = SelectRuntimeRecords(*inputs, request.runtimePrefabRoots);
        if (selected.HasError())
            return Result<Assets::AssetCookReport>::Failure(selected.ErrorValue());
        Assets::AssetCookRequest cook = request.assets;
        cook.registry = std::move(selected).Value();
        cook.pinnedInputs = inputs;
        cook.dependentPhase = composed.Value().templates;
        // Capture references are synchronous: AssetCook joins all accepted work before returning, and the owned project lease outlives it.
        cook.validateHostInputs = [this, &request, &host, &cancellation, releasePlan, capturedRevision] {
            return ValidateCurrentHost(request, registry_, compatibility_, host.Value(), cancellation, releasePlan, capturedRevision);
        };
        Assets::AssetCookService operation{jobs_, composed.Value().catalog};
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
