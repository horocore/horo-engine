#include "OpenXRInstalledProductSelection.h"

#include "Horo/XR/XRErrors.h"

namespace Horo::Application::Internal {
    /** @copydoc SelectInstalledOpenXRProduct */
    Result<OpenXRInstalledProductCandidate> SelectInstalledOpenXRProduct(const PackageProjectHost &packages,
                                                                         const std::filesystem::path &projectRoot,
                                                                         const std::uint64_t generation,
                                                                         const OpenXRInstalledProductPolicy *policy,
                                                                         const CancellationToken &cancellation) {
        using Outcome = Result<OpenXRInstalledProductCandidate>;
        if (!policy || !policy->verifier || !policy->nativeGate || policy->component.deploymentContract.empty())
            return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable, "No authenticated installed XR product policy"));
        // Snapshot trusted installer input before invoking archive/crypto callbacks; a caller alias cannot replace admission mid-call.
        const OpenXRInstalledProductPolicy selected = *policy;
        auto install = packages.InstalledRecord(projectRoot, generation);
        if (install.HasError())
            return Outcome::Failure(install.ErrorValue());
        if (!install.Value())
            return Outcome::Failure(MakeError(XR::XRErrors::LoaderAbsent, "Selected XR component is not installed"));
        auto component = VerifiedOpenXRComponent::Verify(std::move(install).Value(), selected.package, selected.componentDescriptor,
                                                         selected.component, cancellation);
        if (component.HasError())
            return Outcome::Failure(component.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(XR::XRErrors::LoaderPreflightCancelled));
        const auto &package = component.Value().Install()->Graph()->packages[component.Value().PackageIndex()];
        const auto bytes = package.archive->ReadDeclaredFile(selected.deploymentDocument, 16U * 1024U);
        if (bytes.HasError())
            return Outcome::Failure(bytes.ErrorValue());
        auto deployment = VerifiedOpenXRDeployment::Verify(component.Value(), bytes.Value(), selected.deploymentSignature,
                                                           *selected.verifier, selected.component);
        if (deployment.HasError())
            return Outcome::Failure(deployment.ErrorValue());
        // Recheck the canonical authority after all signature/archive work; retained bytes do not grant current activation.
        const auto current = packages.InstalledRecord(projectRoot, generation);
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (current.Value() != component.Value().Install())
            return Outcome::Failure(MakeError(XR::XRErrors::LoaderPreflightStale));
        return Outcome::Success({std::move(component).Value(), std::move(deployment).Value()});
    }
}  // namespace Horo::Application::Internal
