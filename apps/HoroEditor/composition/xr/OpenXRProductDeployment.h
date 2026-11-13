#pragma once
/** @file OpenXRProductDeployment.h
 * @brief Sealed signed first-party deployment admission; no platform loader policy is inferred from project metadata.
 */
#include "Horo/Security/ArtifactSignature.h"
#include "OpenXRComponentManifest.h"

namespace Horo::Application::Internal {
    /** @brief Signed deployment attestation bound to one exact component and explicit installed-product policy.
     * @details The initial profile permits no extra bundled dependencies. Remaining OS ABI dependencies belong to
     * the exact installer-qualified deployment contract. RTLD_LOCAL and an absolute library path do not prove that
     * contract; absent configured trusted product policy remains unavailable before native loading.
     */
    class VerifiedOpenXRDeployment final {
    public:
        /** @brief Verifies a bounded deployment document against actual host trust roots and exact component provenance.
         * @param component Already sealed package/install candidate.
         * @param bytes Exact first-party deployment document, at most 16 KiB.
         * @param envelope Detached signature for those bytes.
         * @param verifier Existing host-composed crypto and trusted-root authority.
         * @param policy Installer-owned deployment contract, publisher/key and actual host tuple.
         * @return Sealed admission or original verification/typed unavailable diagnostic.
         * @pre Policy comes from the authenticated installed-product host, never project configuration or filename inference.
         * @post No native library is opened and no runtime or supported tuple is discovered or advertised.
         */
        [[nodiscard]] static Result<VerifiedOpenXRDeployment> Verify(const VerifiedOpenXRComponent &component,
                                                                     std::span<const std::byte> bytes,
                                                                     const Security::DetachedSignatureEnvelope &envelope,
                                                                     const Security::ArtifactVerifier &verifier,
                                                                     const OpenXRComponentHostPolicy &policy);
        /** @brief Returns exact full component identity bound at verification. @return Immutable digest. */
        [[nodiscard]] const Sha256Digest &ComponentIdentity() const noexcept;
        /** @brief Returns actual trusted signature evidence retained through native retirement. @return Immutable evidence. */
        [[nodiscard]] const Security::VerifiedArtifactEvidence &Evidence() const noexcept;

    private:
        /** @brief Stores only completely verified exact deployment evidence. */
        VerifiedOpenXRDeployment(const Sha256Digest &component, Security::VerifiedArtifactEvidence evidence);
        Sha256Digest component_;
        Security::VerifiedArtifactEvidence evidence_;
    };
}  // namespace Horo::Application::Internal
