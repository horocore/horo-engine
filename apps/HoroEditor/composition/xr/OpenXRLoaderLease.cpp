#include "OpenXRLoaderLease.h"

#include "Horo/XR/XRErrors.h"
#include "PackageContentStaging.h"

namespace Horo::Application::Internal {
    /** @brief One rollback-safe private artifact owner; library wrappers retain it until native unload completes. */
    struct OpenXRLoaderLease::Files final {
        Files(const Files &) = delete;
        Files &operator=(const Files &) = delete;

        VerifiedOpenXRComponent component;
        VerifiedOpenXRDeployment deployment;
        std::filesystem::path root;
        std::optional<Security::VerifiedArtifactEvidence> nativeEvidence;

        Files(VerifiedOpenXRComponent admittedComponent, VerifiedOpenXRDeployment admittedDeployment)
            : component(std::move(admittedComponent)), deployment(std::move(admittedDeployment)) {}

        ~Files() {
            // Every loaded native wrapper is destroyed before its Files reference. Removal failures leave inert private
            // storage behind; they never justify unloading a live native user or adopting an existing directory on retry.
            if (!root.empty()) {
                std::error_code ignored;
                std::filesystem::remove_all(root, ignored);
            }
        }
    };

    /** @brief Retains already verified native storage; aliasing borrows share its exact control block. */
    struct OpenXRLoaderLease::RetainedLibrary final {
        // Reverse destruction unloads the native implementation BEFORE retiring its exact files/provenance.
        std::shared_ptr<const Files> files;
        std::unique_ptr<Platform::DynamicLibrary> native;
    };

    namespace {
        /** @brief Rejects unsupported deployment tuples and unbound evidence before touching files or native code. */
        Result<void> AdmitLoad(const VerifiedOpenXRComponent &component, const VerifiedOpenXRDeployment &deployment,
                               const std::shared_ptr<const Security::NativeArtifactGate> &gate, const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(XR::XRErrors::LoaderPreflightCancelled));
            if (!gate || deployment.ComponentIdentity() != component.IdentityDigest() || !component.Manifest().dependencies.empty())
                return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnavailable));
            // The first native profile is desktop Linux; other deployment paths remain explicitly unimplemented.
#if !defined(__linux__) || defined(__ANDROID__)
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnsupported));
#else
            if (component.Manifest().platform.operatingSystem != "linux")
                return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnsupported));
            return Result<void>::Success();
#endif
        }
    }  // namespace

    /** @copydoc OpenXRLoaderLease::Prepare */
    Result<std::shared_ptr<const OpenXRLoaderLease>> OpenXRLoaderLease::Prepare(VerifiedOpenXRComponent component,
                                                                                VerifiedOpenXRDeployment deployment,
                                                                                const std::filesystem::path &temporaryParent,
                                                                                std::shared_ptr<const Security::NativeArtifactGate> gate,
                                                                                const CancellationToken &cancellation) {
        using Outcome = Result<std::shared_ptr<const OpenXRLoaderLease>>;
        if (const auto admitted = AdmitLoad(component, deployment, gate, cancellation); admitted.HasError())
            return Outcome::Failure(admitted.ErrorValue());
        auto files = std::make_shared<Files>(std::move(component), std::move(deployment));
        auto root = Packages::Detail::CreatePrivatePackageRoot(temporaryParent, XR::XRErrors::LoaderOpenFailed);
        if (root.HasError())
            return Outcome::Failure(root.ErrorValue());
        files->root = std::move(root).Value();
        if (const auto staged =
                Packages::Detail::MaterializeVerifiedPackageRoot(*files->component.Install(), files->component.PackageIndex(),
                                                                 files->component.Descriptor(), files->root, cancellation,
                                                                 {XR::XRErrors::LoaderPreflightInvalid, XR::XRErrors::LoaderOpenFailed,
                                                                  XR::XRErrors::LoaderPreflightCancelled});
            staged.HasError())
            return Outcome::Failure(staged.ErrorValue());
        const auto artifact = files->root / files->component.Manifest().loader.path.Value();
        auto evidence = gate->Verify(artifact);
        if (evidence.HasError())
            return Outcome::Failure(evidence.ErrorValue());
        if (const auto &publisher = files->component.Install()->Graph()->packages[files->component.PackageIndex()].publisher;
            !publisher || !publisher->publisher || evidence.Value().PublisherId() != publisher->publisher->Value() ||
            evidence.Value().KeyId() != publisher->keyId || evidence.Value().ArtifactDigest() != files->component.Manifest().loader.digest)
            return Outcome::Failure(MakeError(XR::XRErrors::RuntimeRejected));
        files->nativeEvidence.emplace(std::move(evidence).Value());
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(XR::XRErrors::LoaderPreflightCancelled));
        auto native = Platform::LoadDynamicLibrary(artifact.string());
        if (native.HasError())
            return Outcome::Failure(native.ErrorValue());
        if (!native.Value()->GetSymbol("xrGetInstanceProcAddr"))
            return Outcome::Failure(MakeError(XR::XRErrors::LoaderIncompatible));
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(XR::XRErrors::LoaderPreflightCancelled));
        const auto retained = std::make_shared<RetainedLibrary>(files, std::move(native).Value());
        std::shared_ptr<const Platform::DynamicLibrary> library{retained, retained->native.get()};
        return Outcome::Success(std::make_shared<OpenXRLoaderLease>(ConstructionKey{}, std::move(files), std::move(library)));
    }

    /** @copydoc OpenXRLoaderLease::OpenXRLoaderLease */
    OpenXRLoaderLease::OpenXRLoaderLease(const ConstructionKey &, std::shared_ptr<const Files> files,
                                         std::shared_ptr<const Platform::DynamicLibrary> library)
        : files_(std::move(files)), library_(std::move(library)) {}

    /** @copydoc OpenXRLoaderLease::Library */
    std::shared_ptr<const Platform::DynamicLibrary> OpenXRLoaderLease::Library() const noexcept {
        return library_;
    }

    /** @copydoc OpenXRLoaderLease::Component */
    const VerifiedOpenXRComponent &OpenXRLoaderLease::Component() const noexcept {
        return files_->component;
    }
}  // namespace Horo::Application::Internal
