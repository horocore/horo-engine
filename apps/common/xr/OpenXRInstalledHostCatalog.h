#pragma once
/** @file OpenXRInstalledHostCatalog.h
 * @brief Signed installer selection, separate from project configuration and the sealed package installation.
 */
#include "Horo/XR/XRFeatureNegotiation.h"
#include "OpenXRInstalledProductSelection.h"

namespace Horo::Application::Internal {
    /** @brief Immutable selected deployment produced only by exact signature and host-tuple admission.
     * @details This record grants neither installation, native loading, runtime availability nor Ready.
     * The package host must still produce a real sealed InstalledRecord, and the selected component/deployment
     * and native loader must pass their existing independent gates. Project metadata cannot supply trust roots.
     */
    class OpenXRInstalledHostCatalog final {
    public:
        /** @brief Admits one exact installer catalog using independently configured host verification roots.
         * @param bytes Complete signed catalog, at most 64 KiB. @param signature Existing detached envelope for those exact bytes.
         * @param verifier Host-composed real verifier retained by the result; null fails closed.
         * @param platform Actual executable package ABI tuple. @param temporaryParent Explicit private materialization parent.
         * @param nowUnixSeconds Actual host wall time used for expiry, never an XR/frame clock.
         * @return Sealed selection or original signature/typed schema/tuple/expiry failure.
         * @post No filesystem access, package mutation, native dispatch, provider discovery or availability publication.
         * @throws std::bad_alloc Cold preparation allocation failure; no partially accepted catalog escapes.
         */
        [[nodiscard]] static Result<OpenXRInstalledHostCatalog> Verify(std::span<const std::byte> bytes,
                                                                       const Security::DetachedSignatureEnvelope &signature,
                                                                       std::shared_ptr<const Security::ArtifactVerifier> verifier,
                                                                       const Packages::PackagePlatform &platform,
                                                                       const std::filesystem::path &temporaryParent,
                                                                       std::uint64_t nowUnixSeconds);
        /** @brief Returns exact retained installer policy. @return Borrow valid for this immutable catalog's lifetime. */
        [[nodiscard]] const OpenXRInstalledProductPolicy &Policy() const noexcept;
        /** @brief Returns the physical declaration path to read only from the selected sealed package archive. */
        [[nodiscard]] const Packages::PackagePath &Bindings() const noexcept;
        /** @brief Returns signed catalog provenance; never a substitute for sealed install evidence. */
        [[nodiscard]] const Security::VerifiedArtifactEvidence &Evidence() const noexcept;
        /** @brief Returns exact signed expiry for revalidation before startup/activation. */
        [[nodiscard]] std::uint64_t ExpiresAt() const noexcept;
        /** @brief Returns signed finite product limits; the canonical configuration/feature resolver still validates them. */
        [[nodiscard]] const XR::XRSystemLimits &Limits() const noexcept;
        /** @brief Returns signed product feature intent, never a capability availability assertion. */
        [[nodiscard]] XR::XRFeatureProfile Profile() const noexcept;

    private:
        OpenXRInstalledHostCatalog(OpenXRInstalledProductPolicy policy, Packages::PackagePath bindings,
                                   Security::VerifiedArtifactEvidence evidence, std::uint64_t expiresAt, XR::XRSystemLimits limits,
                                   XR::XRFeatureProfile profile);
        OpenXRInstalledProductPolicy policy_;
        Packages::PackagePath bindings_;
        Security::VerifiedArtifactEvidence evidence_;
        std::uint64_t expiresAt_;
        XR::XRSystemLimits limits_;
        XR::XRFeatureProfile profile_;
    };
}  // namespace Horo::Application::Internal
