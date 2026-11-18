#pragma once

/** @file HostReleaseManifestSignatureVerifier.h @brief Detached release manifest verification with explicit publisher trust. */
#include "Horo/Release/ReleaseArtifactManifest.h"

namespace Horo::Security {
    class ArtifactVerifier;
}

namespace Horo::Application::Internal {
    /** @brief Shares the real detached signature codec and trust verifier across release assembly and installed hosts. */
    class HostReleaseManifestSignatureVerifier final : public Release::IReleaseManifestSignatureVerifier {
    public:
        /** @brief Borrows an existing explicit host verifier. @param verifier Publisher trust independent of the candidate. */
        explicit HostReleaseManifestSignatureVerifier(const Security::ArtifactVerifier &verifier) noexcept;

        /** @copydoc IReleaseManifestSignatureVerifier::Verify */
        [[nodiscard]] Result<void> Verify(std::span<const std::byte> canonicalManifest,
                                          std::span<const std::byte> detachedEnvelope) const override;

    private:
        const Security::ArtifactVerifier &verifier_;
    };
}  // namespace Horo::Application::Internal
