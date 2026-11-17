#include "Horo/Security/CredentialStore.h"

#include "Horo/Foundation/Configuration.h"
#include "Horo/Security/SecurityErrors.h"

#include <array>
#include <utility>

namespace Horo::Security {
    namespace {
        constexpr std::size_t ReferenceBytes = 24;
        constexpr std::size_t MaximumReferenceAttempts = 4;
        constexpr char Hex[] = "0123456789abcdef";
    }  // namespace

    /** @copydoc ResolveConfigurationCredential */
    Result<SecureBytes> ResolveConfigurationCredential(const ConfigurationSnapshot &snapshot, const SettingKey &key,
                                                       CredentialProvider *provider, const std::uint64_t nowMillis) {
        const auto *setting = snapshot.FindResolved(key);
        if (setting == nullptr || setting->sensitivity != SettingSensitivity::SecretReference)
            return Result<SecureBytes>::Failure(MakeError(SecurityErrors::InvalidInput));
        const auto *reference = std::get_if<CredentialReference>(&setting->value);
        if (reference == nullptr || reference->Value().empty())
            return Result<SecureBytes>::Failure(MakeError(SecurityErrors::CredentialNotFound));
        if (provider == nullptr)
            return Result<SecureBytes>::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
        auto resolved = provider->Resolve(*reference, nowMillis);
        if (resolved.HasValue()) {
            if (resolved.Value().Empty())
                return Result<SecureBytes>::Failure(MakeError(SecurityErrors::CredentialNotFound));
            return resolved;
        }
        for (const auto *failure : {&SecurityErrors::CredentialBackendUnavailable, &SecurityErrors::CredentialExpired,
                                    &SecurityErrors::CredentialRevoked, &SecurityErrors::CredentialNotFound}) {
            if (resolved.ErrorValue().code.Value() == failure->code.Value())
                return Result<SecureBytes>::Failure(MakeError(*failure));
        }
        return Result<SecureBytes>::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
    }

    /** @copydoc CredentialVault::CredentialVault */
    CredentialVault::CredentialVault(std::shared_ptr<CredentialBackend> backend, std::shared_ptr<SecureRandomSource> random)
        : backend_(std::move(backend)), random_(std::move(random)) {}

    CredentialVault::~CredentialVault() {
        Clear();
    }

    /** @copydoc CredentialVault::SetBackend */
    void CredentialVault::SetBackend(std::shared_ptr<CredentialBackend> backend) noexcept {
        backend_ = std::move(backend);
    }

    Result<CredentialReference> CredentialVault::NewReference() const {
        if (!random_)
            return Result<CredentialReference>::Failure(MakeError(SecurityErrors::EntropyUnavailable));
        for (std::size_t attempt = 0; attempt < MaximumReferenceAttempts; ++attempt) {
            std::array<std::byte, ReferenceBytes> randomBytes{};
            if (auto filled = random_->Fill(randomBytes); filled.HasError())
                return Result<CredentialReference>::Failure(filled.ErrorValue());
            std::string value{"cred:v1:"};
            value.reserve(value.size() + randomBytes.size() * 2U);
            for (const std::byte byte : randomBytes) {
                const auto octet = std::to_integer<unsigned int>(byte);
                value.push_back(Hex[octet >> 4U]);
                value.push_back(Hex[octet & 0x0fU]);
            }
            if (!records_.contains(value))
                return Result<CredentialReference>::Success(CredentialReference{std::move(value)});
        }
        return Result<CredentialReference>::Failure(
            MakeError(SecurityErrors::EntropyUnavailable, "Secure entropy produced repeated credential references."));
    }

    Result<CredentialVault::Record *> CredentialVault::FindRecord(const CredentialReference &reference) {
        const auto record = records_.find(std::string{reference.Value()});
        if (record == records_.end())
            return Result<Record *>::Failure(MakeError(SecurityErrors::CredentialNotFound));
        return Result<Record *>::Success(&record->second);
    }

    /** @copydoc CredentialVault::Put */
    Result<CredentialReference> CredentialVault::Put(SecureBytes secret, const std::uint64_t expiresAtMillis) {
        if (secret.Empty())
            return Result<CredentialReference>::Failure(MakeError(SecurityErrors::InvalidInput, "Credential material must not be empty."));
        if (!backend_ || !backend_->Available())
            return Result<CredentialReference>::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
        auto reference = NewReference();
        if (reference.HasError())
            return reference;
        if (auto stored = backend_->Put(reference.Value(), std::move(secret)); stored.HasError())
            return Result<CredentialReference>::Failure(stored.ErrorValue());
        records_.try_emplace(std::string{reference.Value().Value()}, Record{.expiresAtMillis = expiresAtMillis});
        return reference;
    }

    /** @copydoc CredentialVault::Resolve */
    Result<SecureBytes> CredentialVault::Resolve(const CredentialReference &reference, const std::uint64_t nowMillis) {
        auto record = FindRecord(reference);
        if (record.HasError())
            return Result<SecureBytes>::Failure(record.ErrorValue());
        if (record.Value()->revoked)
            return Result<SecureBytes>::Failure(MakeError(SecurityErrors::CredentialRevoked));
        if (record.Value()->expiresAtMillis != 0 && nowMillis >= record.Value()->expiresAtMillis) {
            static_cast<void>(backend_ ? backend_->Remove(reference) : Result<void>::Success());
            record.Value()->revoked = true;
            return Result<SecureBytes>::Failure(MakeError(SecurityErrors::CredentialExpired));
        }
        if (!backend_ || !backend_->Available())
            return Result<SecureBytes>::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
        return backend_->Resolve(reference);
    }

    /** @copydoc CredentialVault::Revoke */
    Result<void> CredentialVault::Revoke(const CredentialReference &reference) {
        auto record = FindRecord(reference);
        if (record.HasError())
            return Result<void>::Failure(record.ErrorValue());
        if (!backend_ || !backend_->Available())
            return Result<void>::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
        if (auto removed = backend_->Remove(reference); removed.HasError())
            return removed;
        record.Value()->revoked = true;
        return Result<void>::Success();
    }

    /** @copydoc CredentialVault::Rotate */
    Result<void> CredentialVault::Rotate(const CredentialReference &reference, SecureBytes replacement,
                                         const std::uint64_t expiresAtMillis) {
        auto record = FindRecord(reference);
        if (record.HasError())
            return Result<void>::Failure(record.ErrorValue());
        if (record.Value()->revoked)
            return Result<void>::Failure(MakeError(SecurityErrors::CredentialRevoked));
        if (replacement.Empty())
            return Result<void>::Failure(MakeError(SecurityErrors::InvalidInput, "Credential material must not be empty."));
        if (!backend_ || !backend_->Available())
            return Result<void>::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
        if (auto stored = backend_->Put(reference, std::move(replacement)); stored.HasError())
            return stored;
        record.Value()->expiresAtMillis = expiresAtMillis;
        return Result<void>::Success();
    }

    /** @copydoc CredentialVault::Clear */
    void CredentialVault::Clear() noexcept {
        if (backend_ && backend_->Available()) {
            for (const auto &[value, record] : records_) {
                static_cast<void>(record);
                static_cast<void>(backend_->Remove(CredentialReference{value}));
            }
        }
        records_.clear();
    }
}  // namespace Horo::Security
