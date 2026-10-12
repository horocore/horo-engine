#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateCaptureControls.h"
#include "UiTemplateHostState.h"

namespace Horo::Application {

    UiTemplateInputLease::UiTemplateInputLease(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc UiTemplateInputLease::Capture */
    Result<UiTemplateInputLease> UiTemplateInputLease::Capture(const std::filesystem::path &root, Assets::AssetRegistrySnapshot registry,
                                                               const Editor::ProjectMutationCoordinator &mutations,
                                                               const Packages::PackageRestoreService *packages, const bool requirePublisher,
                                                               const Assets::AssetCookLimits &limits, const std::uint64_t maximumBytes,
                                                               const CancellationToken &cancellation) {
        using Output = Result<UiTemplateInputLease>;
        try {
            auto projectLease = mutations.TryAcquire({root, Editor::ProjectMutationOwner::Asset, "ui-template-input-capture"});
            if (projectLease.HasError())
                return Output::Failure(projectLease.ErrorValue());
            auto controls = Detail::ReadTemplateControls(root, packages ? packages->ActiveGraph() : nullptr);
            if (controls.HasError())
                return Output::Failure(controls.ErrorValue());
            auto identity = Detail::ValidateTemplatePackages(controls.Value(), requirePublisher);
            if (identity.HasError())
                return Output::Failure(identity.ErrorValue());
            auto source = Assets::AssetCookInputSnapshot::Capture(root, registry, limits, maximumBytes, cancellation);
            if (source.HasError())
                return Output::Failure(source.ErrorValue());
            // Keep the real project lease through the final control/graph recheck and capability allocation.
            const auto unchanged = Detail::VerifyTemplateControls(root, controls.Value(), packages ? packages->ActiveGraph() : nullptr);
            if (unchanged.HasError())
                return Output::Failure(unchanged.ErrorValue());
            return Output::Success(UiTemplateInputLease{std::make_shared<const State>(
                State{std::move(source).Value(), std::move(registry), controls.Value().graph, std::move(identity).Value()})});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiTemplates::UiErrors::TemplateGraphBudgetExceeded));
        }
    }
}  // namespace Horo::Application
