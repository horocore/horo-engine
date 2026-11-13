#pragma once

/**
 * @file CredentialStore.h
 * @brief Opaque credential references and fail-closed credential lifecycle contracts.
 */

#include "Horo/Foundation/CredentialReference.h"
#include "Horo/Security/SecureMemory.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Horo {
    class ConfigurationSnapshot;
    class SettingKey;
}  // namespace Horo

namespace Horo::Security {
    /** @brief Opaque, non-secret reference to provider-owned credential material. */
    using CredentialReference = Horo::CredentialReference;

    /** @brief Provider boundary for secure credential persistence. */
    class CredentialBackend {
    public:
        virtual ~CredentialBackend() = default;
        /** @brief Reports whether this backend can currently perform operations. @return True only while the provider is usable. */
        [[nodiscard]] virtual bool Available() const noexcept = 0;
        /** @brief Stores or replaces credential material. @param reference Opaque storage key. @param secret Material consumed on every
         * path. @return Typed outcome. */
        [[nodiscard]] virtual Result<void> Put(const CredentialReference &reference, SecureBytes secret) = 0;
        /** @brief Resolves short-lived credential material. @param reference Opaque storage key. @return Move-only secret or typed failure.
         */
        [[nodiscard]] virtual Result<SecureBytes> Resolve(const CredentialReference &reference) = 0;
        /** @brief Erases provider-owned material. @param reference Opaque storage key. @return Typed outcome. */
        [[nodiscard]] virtual Result<void> Remove(const CredentialReference &reference) noexcept = 0;
    };

    /** @brief Operation-local credential lookup authority, explicitly composed by the consuming host.
     * @details Implementations enforce existence, expiry and revocation before exposing material. Provider ownership
     * and thread rules belong to the host. The synchronous provider must outlive the consuming call.
     */
    class CredentialProvider {
    public:
        virtual ~CredentialProvider() = default;
        /** @brief Resolves one live opaque identity for the consuming operation.
         * @param reference Provider-owned identity. @param nowMillis Monotonic observation time.
         * @return Move-only secret or a typed security failure. */
        [[nodiscard]] virtual Result<SecureBytes> Resolve(const CredentialReference &reference, std::uint64_t nowMillis) = 0;
    };

    /** @brief Coordinates opaque references, expiry, rotation, revocation, and provider recovery. */
    class CredentialVault final : public CredentialProvider {
    public:
        /**
         * @brief Creates a fail-closed vault with explicit provider and entropy composition.
         * @param backend Credential provider; null is treated as unavailable.
         * @param random Entropy provider used to create unguessable references; null is treated as unavailable.
         */
        CredentialVault(std::shared_ptr<CredentialBackend> backend, std::shared_ptr<SecureRandomSource> random);
        ~CredentialVault() override;
        CredentialVault(const CredentialVault &) = delete;
        CredentialVault &operator=(const CredentialVault &) = delete;

        /** @brief Replaces the unavailable provider so a host can recover without recreating the vault. @param backend Replacement
         * provider, or null to fail closed. */
        void SetBackend(std::shared_ptr<CredentialBackend> backend) noexcept;

        /**
         * @brief Stores a secret behind a new opaque reference.
         * @param secret Move-only secret consumed on every path.
         * @param expiresAtMillis Exclusive monotonic expiry, or zero for no expiry.
         * @return New opaque reference or a typed failure.
         */
        [[nodiscard]] Result<CredentialReference> Put(SecureBytes secret, std::uint64_t expiresAtMillis = 0);

        /** @brief Resolves a live reference into a short-lived move-only secret. @param reference Opaque reference to resolve. @param
         * nowMillis Monotonic observation time. @return Secret or typed failure. */
        [[nodiscard]] Result<SecureBytes> Resolve(const CredentialReference &reference, std::uint64_t nowMillis = 0) override;

        /** @brief Permanently revokes a reference and erases provider material. @param reference Opaque reference to revoke. @return Typed
         * outcome. */
        [[nodiscard]] Result<void> Revoke(const CredentialReference &reference);

        /** @brief Replaces provider material while preserving the opaque reference. @param reference Opaque reference to rotate. @param
         * replacement New material consumed on every path. @param expiresAtMillis New exclusive expiry. @return Typed outcome. */
        [[nodiscard]] Result<void> Rotate(const CredentialReference &reference, SecureBytes replacement, std::uint64_t expiresAtMillis = 0);

        /** @brief Revokes every reference created by this vault. */
        void Clear() noexcept;

    private:
        struct Record {
            std::uint64_t expiresAtMillis{};
            bool revoked{};
        };

        [[nodiscard]] Result<CredentialReference> NewReference() const;
        [[nodiscard]] Result<Record *> FindRecord(const CredentialReference &reference);
        std::shared_ptr<CredentialBackend> backend_;
        std::shared_ptr<SecureRandomSource> random_;
        std::unordered_map<std::string, Record> records_;
    };

    /**
     * @brief Resolves a snapshot reference through an explicitly composed provider inside a consuming operation.
     * @param snapshot Immutable configuration captured by the operation.
     * @param key Secret-reference setting to resolve.
     * @param provider Borrowed lifecycle authority that outlives this synchronous call, or null when unavailable.
     * @param nowMillis Explicit host observation time in the same monotonic domain as provider expiry.
     * @return Move-only secret, or a safe typed failure with provider diagnostic text removed.
     * @note The caller owns the returned secret only for the consuming operation and must not cache or publish it.
     * Calls follow the provider's thread contract. The provider retains expiry and revocation authority.
     */
    [[nodiscard]] Result<SecureBytes> ResolveConfigurationCredential(const ConfigurationSnapshot &snapshot, const SettingKey &key,
                                                                     CredentialProvider *provider, std::uint64_t nowMillis);
}  // namespace Horo::Security
