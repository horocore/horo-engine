#pragma once

/**
 * @file SaveArchiveProtection.h
 * @brief Host-selected protection boundary outside the unencrypted v1 save format.
 */

#include "Horo/Runtime/Save/SaveArchiveReader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Runtime {
    /** @brief Explicit host policy; archive bytes cannot choose or relax it. */
    enum class SaveProtectionPolicy : std::uint8_t {
        UnencryptedLocal,
        RequireAuthenticatedEncryption,
    };

    /** @brief Non-secret opaque provider and key identities; all-zero identities are invalid. */
    struct SaveProtectionIdentity final {
        std::array<std::uint8_t, 16> providerId{};
        std::array<std::uint8_t, 16> keyReference{};
    };

    /** @brief Provider-advertised bounds and authenticated-encryption capability. */
    struct SaveProtectionCapabilities final {
        bool authenticatedEncryption{};
        std::size_t minimumNonceBytes{};
        std::size_t maximumNonceBytes{};
        std::size_t maximumSealedBytes{};
    };

    /** @brief Opaque protected bytes; the provider owns nonce allocation and tag format. */
    struct SaveProtectedArchive final {
        SaveProtectionIdentity identity;
        std::vector<std::byte> nonce;
        std::vector<std::byte> sealedBytes;
    };

    /** @brief Authenticated-encryption backend selected by a trusted host, never by the archive. */
    class SaveArchiveProtectionProvider {
    public:
        virtual ~SaveArchiveProtectionProvider() = default;
        /** @brief Returns stable provider identity. @return Non-secret opaque ID. */
        [[nodiscard]] virtual std::array<std::uint8_t, 16> Id() const noexcept = 0;
        /** @brief Returns currently available capability and finite bounds. @return Provider capability. */
        [[nodiscard]] virtual SaveProtectionCapabilities Capabilities() const noexcept = 0;
        /**
         * @brief Seals bytes with a fresh provider-owned nonce and an authentication tag.
         * @param keyReference Non-secret reference resolved only inside this provider.
         * @param associatedData Trusted host-composed scope and version bytes, never archive-selected.
         * @param plaintext Complete bounded v1 archive bytes.
         * @return Nonce and sealed bytes, or a typed unavailable/rotated/revoked failure.
         */
        [[nodiscard]] virtual Result<SaveProtectedArchive> Seal(std::array<std::uint8_t, 16> keyReference,
                                                                std::span<const std::byte> associatedData,
                                                                std::span<const std::byte> plaintext) = 0;
        /**
         * @brief Verifies the authentication tag before returning any plaintext.
         * @param archive Untrusted protected bytes and non-secret routing metadata.
         * @param associatedData Trusted host-composed scope and version bytes.
         * @return Complete authenticated plaintext, or a typed failure with no plaintext exposure.
         */
        [[nodiscard]] virtual Result<std::vector<std::byte>> Open(const SaveProtectedArchive &archive,
                                                                  std::span<const std::byte> associatedData) = 0;
    };

    /** @brief Finite host limits applied before invoking a provider or archive parser. */
    struct SaveProtectionLimits final {
        std::size_t maximumArchiveBytes{64ULL << 20U};
        std::size_t maximumAssociatedDataBytes{4'096};
        std::size_t maximumNonceBytes{64};
    };

    /**
     * @brief Seals complete v1 bytes through the host-selected provider without a plaintext fallback.
     * @param plaintext Complete bounded v1 archive bytes; caller retains and discards this buffer.
     * @param identity Trusted non-secret provider and key binding.
     * @param associatedData Trusted host-composed namespace, slot, generation and format binding.
     * @param provider Host-selected backend; null is unavailable.
     * @param limits Trusted outer-envelope bounds.
     * @return Opaque protected bytes, or a safe typed failure before publication.
     */
    [[nodiscard]] Result<SaveProtectedArchive> ProtectSaveArchive(std::span<const std::byte> plaintext,
                                                                  const SaveProtectionIdentity &identity,
                                                                  std::span<const std::byte> associatedData,
                                                                  SaveArchiveProtectionProvider *provider,
                                                                  const SaveProtectionLimits &limits = {});

    /**
     * @brief Admits a protected archive only after provider authentication, then parses bounded v1 bytes.
     * @param archive Untrusted protected input.
     * @param expectedIdentity Host-selected provider and key binding, not copied from the archive.
     * @param associatedData Host-composed nonempty namespace, slot, generation and format binding.
     * @param provider Host-selected backend; null is unavailable.
     * @param reader Bounded v1 archive reader.
     * @param limits Trusted outer-envelope bounds.
     * @return Owned validated v1 archive or a safe typed failure.
     */
    [[nodiscard]] Result<ValidatedSaveArchive> AdmitProtectedSaveArchive(
        const SaveProtectedArchive &archive, const SaveProtectionIdentity &expectedIdentity, std::span<const std::byte> associatedData,
        SaveArchiveProtectionProvider *provider, const SaveArchiveReader &reader, const SaveProtectionLimits &limits = {});

    /**
     * @brief Admits plaintext only under explicit local policy; required encryption never downgrades.
     * @param archive Complete untrusted v1 bytes.
     * @param policy Trusted host policy.
     * @param reader Bounded v1 archive reader.
     * @return Owned validated archive or a typed policy/parser failure.
     */
    [[nodiscard]] Result<ValidatedSaveArchive> AdmitUnencryptedLocalSaveArchive(std::vector<std::byte> archive, SaveProtectionPolicy policy,
                                                                                const SaveArchiveReader &reader);
}  // namespace Horo::Runtime
