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

        /** @brief Generate one opaque, non-secret identity using the shared credential format. */
        Result<CredentialReference> RandomReference(SecureRandomSource &random) {
            std::array<std::byte, ReferenceBytes> randomBytes{};
            if (auto filled = random.Fill(randomBytes); filled.HasError())
                return Result<CredentialReference>::Failure(filled.ErrorValue());
            std::string value{"cred:v1:"};
            value.reserve(value.size() + randomBytes.size() * 2U);
            for (const auto byte : randomBytes) {
                const auto octet = std::to_integer<unsigned int>(byte);
                value.push_back(Hex[octet >> 4U]);
                value.push_back(Hex[octet & 0x0fU]);
            }
            return CredentialReference::Parse(value);
        }

        /** @brief Compare stable error identity without retaining provider-supplied text. */
        bool Matches(const Error &error, const ErrorCodeDescriptor &descriptor) {
            return error.domain.Value() == descriptor.domain.Value() && error.code.Value() == descriptor.code.Value();
        }

        /** @brief Preserve known lifecycle errors while dropping unrestricted native diagnostics. */
        Error RedactCredentialFailure(const Error &error) {
            for (const auto *failure : {&SecurityErrors::InvalidInput, &SecurityErrors::CredentialExpired,
                                        &SecurityErrors::CredentialRevoked, &SecurityErrors::CredentialNotFound}) {
                if (Matches(error, *failure))
                    return MakeError(*failure);
            }
            return MakeError(SecurityErrors::CredentialBackendUnavailable);
        }
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
        try {
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
        } catch (...) {
            // Provider exceptions may contain protected data. Keep them inside the credential boundary.
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
            auto reference = RandomReference(*random_);
            if (reference.HasError() || !records_.contains(std::string{reference.Value().Value()}))
                return reference;
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

    /** @copydoc PersistentCredentialStore::PersistentCredentialStore */
    PersistentCredentialStore::PersistentCredentialStore(std::shared_ptr<CredentialBackend> backend,
                                                         std::shared_ptr<SecureRandomSource> random)
        : backend_(std::move(backend)), random_(std::move(random)) {}

    /** @copydoc PersistentCredentialStore::Available */
    bool PersistentCredentialStore::Available() const noexcept {
        return backend_ && backend_->Available();
    }

    /** @copydoc PersistentCredentialStore::Enroll */
    Result<CredentialReference> PersistentCredentialStore::Enroll(SecureBytes secret) {
        using Outcome = Result<CredentialReference>;
        if (secret.Empty())
            return Outcome::Failure(MakeError(SecurityErrors::InvalidInput));
        if (!Available())
            return Outcome::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
        if (!random_)
            return Outcome::Failure(MakeError(SecurityErrors::EntropyUnavailable));
        try {
            for (std::size_t attempt = 0; attempt < MaximumReferenceAttempts; ++attempt) {
                auto reference = RandomReference(*random_);
                if (reference.HasError())
                    return Outcome::Failure(MakeError(SecurityErrors::EntropyUnavailable));
                const auto existing = backend_->Resolve(reference.Value());
                if (existing.HasValue())
                    continue;
                if (!Matches(existing.ErrorValue(), SecurityErrors::CredentialNotFound))
                    return Outcome::Failure(RedactCredentialFailure(existing.ErrorValue()));
                if (auto stored = backend_->Put(reference.Value(), std::move(secret)); stored.HasError())
                    return Outcome::Failure(RedactCredentialFailure(stored.ErrorValue()));
                return reference;
            }
            return Outcome::Failure(MakeError(SecurityErrors::EntropyUnavailable));
        } catch (...) {
            return Outcome::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
        }
    }

    /** @copydoc PersistentCredentialStore::Resolve */
    Result<SecureBytes> PersistentCredentialStore::Resolve(const CredentialReference &reference, std::uint64_t) {
        using Outcome = Result<SecureBytes>;
        if (reference.Value().empty())
            return Outcome::Failure(MakeError(SecurityErrors::CredentialNotFound));
        if (!Available())
            return Outcome::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
        try {
            auto resolved = backend_->Resolve(reference);
            if (resolved.HasError())
                return Outcome::Failure(RedactCredentialFailure(resolved.ErrorValue()));
            if (resolved.Value().Empty())
                return Outcome::Failure(MakeError(SecurityErrors::CredentialNotFound));
            return resolved;
        } catch (...) {
            return Outcome::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
        }
    }

    /** @copydoc PersistentCredentialStore::Revoke */
    Result<void> PersistentCredentialStore::Revoke(const CredentialReference &reference) {
        if (reference.Value().empty())
            return Result<void>::Failure(MakeError(SecurityErrors::InvalidInput));
        if (!backend_)
            return Result<void>::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
        const auto removed = backend_->Remove(reference);
        if (removed.HasError() && !Matches(removed.ErrorValue(), SecurityErrors::CredentialNotFound))
            return Result<void>::Failure(RedactCredentialFailure(removed.ErrorValue()));
        return Result<void>::Success();
    }
}  // namespace Horo::Security
