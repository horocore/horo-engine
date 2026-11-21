#include "HostReleaseManifestSignatureVerifier.h"

#include "Horo/Security/ArtifactSignature.h"
#include "HostSignatureDocument.h"

namespace Horo::Application::Internal {
    /** @copydoc HostReleaseManifestSignatureVerifier::HostReleaseManifestSignatureVerifier */
    HostReleaseManifestSignatureVerifier::HostReleaseManifestSignatureVerifier(const Security::ArtifactVerifier &verifier) noexcept
        : verifier_(verifier) {}

    /** @copydoc HostReleaseManifestSignatureVerifier::Verify */
    Result<void> HostReleaseManifestSignatureVerifier::Verify(std::span<const std::byte> canonicalManifest,
                                                              std::span<const std::byte> detachedEnvelope) const {
        const auto detached = DecodeDetachedSignatureDocument(detachedEnvelope);
        if (detached.HasError())
            return Result<void>::Failure(detached.ErrorValue());
        const auto verified = verifier_.Verify(canonicalManifest, detached.Value());
        if (verified.HasError())
            return Result<void>::Failure(verified.ErrorValue());
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
