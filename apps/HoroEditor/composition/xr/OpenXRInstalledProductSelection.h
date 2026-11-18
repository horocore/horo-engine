#pragma once
/** @file OpenXRInstalledProductSelection.h
 * @brief Explicit installed-product XR input at the application composition root; never project-owned trust.
 */
#include "Horo/XR/XRPassthrough.h"
#include "OpenXRLoaderLease.h"
#include "PackageProjectHost.h"

namespace Horo::Application::Internal {
    /** @brief Explicit platform authority for opaque runtime reconstruction passthrough.
     * @details Implementations use current platform permission evidence only. The Play owner supplies
     * separate operator consent and the native runtime owns its compositor feature. The port performs
     * no prompts, I/O, reentrancy or camera access, and retains no rendered frames.
     */
    class IOpenXRPassthroughPermission {
    public:
        virtual ~IOpenXRPassthroughPermission() = default;
        /** @brief Whether this platform composition supports requesting the installed runtime feature. */
        [[nodiscard]] virtual bool SupportsReconstruction() const noexcept = 0;
        /** @brief Whether authorization currently covers this exact session, calibrated World and operator grant. */
        [[nodiscard]] virtual bool Allows(const XR::XRPassthroughScope &scope) const noexcept = 0;
    };

    /** @brief Authenticated installer-selected component, deployment signature and existing trust authorities.
     * @details Project configuration may request XR, but cannot construct this host policy from package names or booleans.
     * An absent policy remains unavailable; no built-in key, unsigned gate or ambient path search is substituted.
     */
    struct OpenXRInstalledProductPolicy final {
        Packages::HoroPackageId package;
        Packages::PackagePath componentDescriptor;
        Packages::PackagePath deploymentDocument;
        OpenXRComponentHostPolicy component;
        Security::DetachedSignatureEnvelope deploymentSignature;
        std::shared_ptr<const Security::ArtifactVerifier> verifier;
        std::shared_ptr<const Security::NativeArtifactGate> nativeGate;
        std::filesystem::path temporaryParent;
        std::shared_ptr<const IOpenXRPassthroughPermission> passthroughPermission{}; /**< Absent unless the platform owns this route. */
    };

    /** @brief One fully verified candidate; no native loader/session or support publication exists yet. */
    struct OpenXRInstalledProductCandidate final {
        VerifiedOpenXRComponent component;
        VerifiedOpenXRDeployment deployment;
    };

    /** @brief Reads the SAME canonical package host's sealed snapshot and verifies explicit first-party deployment input.
     * @param packages Canonical application package authority, used by the session's ordinary package features too.
     * @param projectRoot Exact canonical current project root.
     * @param generation Exact package host generation captured by executable startup.
     * @param policy Explicit authenticated installed-product selection/trust; null cannot admit XR.
     * @param cancellation Cooperative preparation cancellation.
     * @return Verified retained candidate or original snapshot/archive/signature failure.
     * @post Does not restore/install/activate packages, open native code or publish availability.
     */
    [[nodiscard]] Result<OpenXRInstalledProductCandidate> SelectInstalledOpenXRProduct(const PackageProjectHost &packages,
                                                                                       const std::filesystem::path &projectRoot,
                                                                                       std::uint64_t generation,
                                                                                       const OpenXRInstalledProductPolicy *policy,
                                                                                       const CancellationToken &cancellation);
}  // namespace Horo::Application::Internal
