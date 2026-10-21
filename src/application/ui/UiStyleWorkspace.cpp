#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiStyleWorkspaceInternal.h"

#include <algorithm>

namespace Horo::Application {
    namespace {
        using namespace Runtime::Ui;
        using namespace UiStyleWorkspaceInternal;

        /** @brief Compose only explicit domain/resource contributions into an ordinary immutable generic catalog. */
        Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>> ResourceCatalog(const UiStyleWorkspaceConfig &config) {
            Assets::CookerCatalog catalog;
            for (const auto &contribution : config.resourceCookers) {
                if (contribution.assetType.Value() == UiStyleAssetTypeName)
                    return Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>>::Failure(
                        MakeError(UiErrors::StyleIdentityConflict));
                if (auto registered = catalog.Register(contribution); registered.HasError())
                    return Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>>::Failure(registered.ErrorValue());
            }
            return catalog.Publish();
        }

        /** @brief Bind the dependent style cook to actual first-phase resource envelopes and one selected-root policy. */
        std::shared_ptr<const Assets::AssetCookDependentPhase> StylePhase(std::shared_ptr<const Capture> capture,
                                                                          const AssetCookTargetId target,
                                                                          const RuntimeStyleGeneration generation) {
            auto phase = std::make_shared<Assets::AssetCookDependentPhase>();
            phase->resourceIds = capture->resources;
            phase->makeCatalog =
                [capture, target,
                 generation](const std::span<const Assets::AssetCookCandidateArtifactView> resources,
                             const CancellationToken &cancellation) -> Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>> {
                using R = Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>>;
                if (cancellation.IsCancellationRequested())
                    return R::Failure(MakeError(UiErrors::AssetLoadCancelled));
                auto schema = Schema(*capture, resources);
                if (schema.HasError())
                    return R::Failure(schema.ErrorValue());
                auto contribution = MakeUiStyleCookerContribution(capture->documents, std::move(schema).Value(), target, generation);
                if (contribution.HasError())
                    return R::Failure(contribution.ErrorValue());
                Assets::CookerCatalog catalog;
                if (auto registered = catalog.Register(std::move(contribution).Value()); registered.HasError())
                    return R::Failure(registered.ErrorValue());
                return catalog.Publish();
            };
            return phase;
        }
    }  // namespace

    /** @copydoc PrepareUiStyleWorkspace */
    Result<UiStyleWorkspaceCandidate> PrepareUiStyleWorkspace(JobSystem &jobs, const UiStyleWorkspaceConfig &config,
                                                              Assets::AssetRegistrySnapshot assets, const Assets::AssetId workspace,
                                                              const std::uint64_t request, const RuntimeStyleAssetId theme,
                                                              const RuntimeStyleGeneration generation,
                                                              const CancellationToken &cancellation) {
        using R = Result<UiStyleWorkspaceCandidate>;
        if (config.maximumCapturedBytes == 0 || config.maximumCapturedBytes > MaximumUiStyleWorkspaceBytes ||
            config.cook.limits.maximumSourceBytes > MaximumUiStyleWorkspaceBytes ||
            config.cook.limits.maximumArtifactBytes > MaximumUiStyleWorkspaceBytes)
            return R::Failure(MakeError(UiErrors::CapacityExceeded));
        if (!workspace.IsValid() || request == 0 || !theme.IsValid() || !generation.IsValid() || !config.acquireSourceLease ||
            !config.currentAssets || config.cook.target.Value() != "headless-null" || !config.cook.publicationFiles ||
            !config.cook.cacheFiles || !config.cook.newPublicationOperationId)
            return R::Failure(MakeError(UiErrors::StyleLifecycleUnavailable));
        if (cancellation.IsCancellationRequested())
            return R::Failure(MakeError(UiErrors::AssetLoadCancelled));
        const auto revision = assets.Revision();
        if (config.currentAssets() != revision)
            return R::Failure(MakeError(UiErrors::AssetRegistryStale));
        auto lease = config.acquireSourceLease();
        if (lease.HasError())
            return R::Failure(lease.ErrorValue());
        if (!lease.Value())
            return R::Failure(MakeError(UiErrors::StyleLifecycleUnavailable));
        auto captured = CaptureSources(config, assets, theme, cancellation);
        if (captured.HasError())
            return R::Failure(captured.ErrorValue());
        auto capture = std::make_shared<const Capture>(std::move(captured).Value());
        auto catalog = ResourceCatalog(config);
        if (catalog.HasError())
            return R::Failure(catalog.ErrorValue());
        auto cook = config.cook;
        cook.registry = capture->selected;
        cook.pinnedInputs = capture->inputs;
        cook.dependentPhase = StylePhase(capture, cook.target, generation);
        const auto previous = cook.validateHostInputs;
        cook.validateHostInputs = [&config, revision, previous, capture, cancellation]() -> Result<void> {
            if (config.currentAssets() != revision)
                return Result<void>::Failure(MakeError(UiErrors::AssetRegistryStale));
            if (const auto unchanged = capture->inputs->VerifyUnchanged(cancellation); unchanged.HasError())
                return unchanged;
            return previous ? previous() : Result<void>::Success();
        };
        Assets::AssetCookService service{jobs, std::move(catalog).Value()};
        auto cooked = service.Cook(cook, cancellation);
        if (cooked.HasError())
            return R::Failure(cooked.ErrorValue());
        auto candidate =
            LoadCandidate(jobs, config, *capture, cooked.Value().generation, workspace, request, theme, generation, cancellation);
        if (candidate.HasError())
            return candidate;
        if (config.currentAssets() != revision)
            return R::Failure(MakeError(UiErrors::AssetRegistryStale));
        auto result = std::move(candidate).Value();
        result.sourceLease = std::move(lease).Value();
        result.cookedAssets = cooked.Value().cookedAssets;
        result.cacheHits = cooked.Value().cacheHits;
        return R::Success(std::move(result));
    }
}  // namespace Horo::Application
