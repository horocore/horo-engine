#pragma once

/**
 * @file SaveArchiveAuthenticity.h
 * @brief Host-selected v1 save-signature policy and provider verification seam.
 */

#include "Horo/Runtime/Save/SaveArchiveReader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Runtime {
    /** @brief Trusted host signature policy; archive contents cannot select a weaker mode. */
    enum class SaveSignaturePolicy : std::uint8_t {
        Disabled,
        Optional,
        Required,
    };

    /** @brief Save-specific signer boundary; trusted roots and keys stay inside its host provider. */
    class SaveArchiveSignatureProvider {
    public:
        virtual ~SaveArchiveSignatureProvider() = default;
        /** @brief Reports support for the v1 Ed25519 signature contract. @return True only while supported. */
        [[nodiscard]] virtual bool SupportsEd25519() const noexcept = 0;
        /**
         * @brief Verifies a v1 signature and host-authorized signer scope before any archive decode.
         * @param signerKeyId Non-secret ID from the bounded v1 trailer, never a supplied trust root.
         * @param trustedScope Host-composed nonempty project/account/world and operation scope.
         * @param message Exact v1 domain-separated signature message.
         * @param signature Exact 64-byte Ed25519 signature from the bounded v1 trailer.
         * @return Success only after trusted-root, scope, and signature checks; no secret diagnostics.
         */
        [[nodiscard]] virtual Result<void> Verify(std::array<std::uint8_t, 16> signerKeyId, std::span<const std::byte> trustedScope,
                                                  std::span<const std::byte> message, std::span<const std::byte> signature) = 0;
    };

    /**
     * @brief Applies host-selected signature policy before decoding a bounded v1 archive.
     * @param archive Complete untrusted v1 bytes, moved into the validated result on success.
     * @param policy Trusted host policy independent of the archive.
     * @param trustedScope Host-composed nonempty authorized signature scope.
     * @param provider Trusted verifier; null fails closed for signed input.
     * @param reader Bounded archive reader, invoked only after required signature verification.
     * @param maximumArchiveBytes Finite outer bound applied before preflight.
     * @return Owned validated archive or a safe typed policy, provider, signature, or framing failure.
     */
    [[nodiscard]] Result<ValidatedSaveArchive> AdmitSignedSaveArchive(std::vector<std::byte> archive, SaveSignaturePolicy policy,
                                                                      std::span<const std::byte> trustedScope,
                                                                      SaveArchiveSignatureProvider *provider,
                                                                      const SaveArchiveReader &reader,
                                                                      std::size_t maximumArchiveBytes = 64ULL << 20U);
}  // namespace Horo::Runtime
