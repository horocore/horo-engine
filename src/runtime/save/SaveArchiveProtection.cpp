#include "Horo/Runtime/Save/SaveArchiveProtection.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace Horo::Runtime {
    namespace {
        [[nodiscard]] bool Nonzero(const std::array<std::uint8_t, 16> &id) noexcept {
            return std::ranges::any_of(id, [](const std::uint8_t value) {
                return value != 0;
            });
        }

        [[nodiscard]] bool ValidLimits(const SaveProtectionLimits &limits, const std::span<const std::byte> associatedData) noexcept {
            return limits.maximumArchiveBytes != 0 && limits.maximumNonceBytes != 0 && limits.maximumAssociatedDataBytes != 0 &&
                   !associatedData.empty() && associatedData.size() <= limits.maximumAssociatedDataBytes;
        }

        [[nodiscard]] const ErrorCodeDescriptor &SafeProviderError(const Error &error) noexcept {
            // Provider diagnostic text may contain credential details. Return only a declared safe code.
            if (error.domain.Value() != SaveErrors::ProtectionUnavailable.domain.Value())
                return SaveErrors::ProtectionUnavailable;
            const auto &code = error.code.Value();
            if (code == SaveErrors::ProtectionKeyRotated.code.Value())
                return SaveErrors::ProtectionKeyRotated;
            if (code == SaveErrors::ProtectionKeyRevoked.code.Value())
                return SaveErrors::ProtectionKeyRevoked;
            if (code == SaveErrors::ProtectionUnsupported.code.Value())
                return SaveErrors::ProtectionUnsupported;
            if (code == SaveErrors::ProtectionAuthenticationFailed.code.Value())
                return SaveErrors::ProtectionAuthenticationFailed;
            return SaveErrors::ProtectionUnavailable;
        }

        [[nodiscard]] bool ValidCapabilities(const SaveProtectionCapabilities &capabilities, const SaveProtectionLimits &limits) noexcept {
            return capabilities.authenticatedEncryption && capabilities.minimumNonceBytes != 0 &&
                   capabilities.minimumNonceBytes <= capabilities.maximumNonceBytes &&
                   capabilities.maximumNonceBytes <= limits.maximumNonceBytes && capabilities.maximumSealedBytes != 0;
        }
    }  // namespace

    /** @copydoc ProtectSaveArchive */
    Result<SaveProtectedArchive> ProtectSaveArchive(const std::span<const std::byte> plaintext, const SaveProtectionIdentity &identity,
                                                    const std::span<const std::byte> associatedData,
                                                    SaveArchiveProtectionProvider *provider, const SaveProtectionLimits &limits) {
        if (!ValidLimits(limits, associatedData) || !Nonzero(identity.providerId) || !Nonzero(identity.keyReference) || plaintext.empty() ||
            plaintext.size() > limits.maximumArchiveBytes)
            return Result<SaveProtectedArchive>::Failure(MakeError(SaveErrors::ProtectionInvalid));
        if (provider == nullptr)
            return Result<SaveProtectedArchive>::Failure(MakeError(SaveErrors::ProtectionUnavailable));
        if (provider->Id() != identity.providerId)
            return Result<SaveProtectedArchive>::Failure(MakeError(SaveErrors::ProtectionUnsupported));
        const auto capabilities = provider->Capabilities();
        if (!ValidCapabilities(capabilities, limits))
            return Result<SaveProtectedArchive>::Failure(MakeError(SaveErrors::ProtectionUnsupported));

        auto sealed = provider->Seal(identity.keyReference, associatedData, plaintext);
        if (sealed.HasError())
            return Result<SaveProtectedArchive>::Failure(MakeError(SafeProviderError(sealed.ErrorValue())));
        auto result = std::move(sealed).Value();
        if (result.identity.providerId != identity.providerId || result.identity.keyReference != identity.keyReference ||
            result.nonce.size() < capabilities.minimumNonceBytes || result.nonce.size() > capabilities.maximumNonceBytes ||
            result.sealedBytes.empty() || result.sealedBytes.size() > capabilities.maximumSealedBytes ||
            result.sealedBytes.size() > limits.maximumArchiveBytes)
            return Result<SaveProtectedArchive>::Failure(MakeError(SaveErrors::ProtectionInvalid));
        return Result<SaveProtectedArchive>::Success(std::move(result));
    }

    /** @copydoc AdmitProtectedSaveArchive */
    Result<ValidatedSaveArchive> AdmitProtectedSaveArchive(const SaveProtectedArchive &archive,
                                                           const SaveProtectionIdentity &expectedIdentity,
                                                           const std::span<const std::byte> associatedData,
                                                           SaveArchiveProtectionProvider *provider, const SaveArchiveReader &reader,
                                                           const SaveProtectionLimits &limits) {
        if (!ValidLimits(limits, associatedData) || !Nonzero(expectedIdentity.providerId) || !Nonzero(expectedIdentity.keyReference) ||
            archive.nonce.empty() || archive.nonce.size() > limits.maximumNonceBytes || archive.sealedBytes.empty() ||
            archive.sealedBytes.size() > limits.maximumArchiveBytes)
            return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ProtectionInvalid));
        if (archive.identity.providerId != expectedIdentity.providerId || archive.identity.keyReference != expectedIdentity.keyReference)
            return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ProtectionUnsupported));
        if (provider == nullptr)
            return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ProtectionUnavailable));
        if (provider->Id() != expectedIdentity.providerId)
            return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ProtectionUnsupported));
        const auto capabilities = provider->Capabilities();
        if (!ValidCapabilities(capabilities, limits))
            return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ProtectionUnsupported));
        if (archive.nonce.size() < capabilities.minimumNonceBytes || archive.nonce.size() > capabilities.maximumNonceBytes ||
            archive.sealedBytes.size() > capabilities.maximumSealedBytes)
            return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ProtectionInvalid));

        // Open is the only producer of plaintext. Its contract requires full tag/AAD verification first.
        auto plaintext = provider->Open(archive, associatedData);
        if (plaintext.HasError())
            return Result<ValidatedSaveArchive>::Failure(MakeError(SafeProviderError(plaintext.ErrorValue())));
        if (plaintext.Value().empty() || plaintext.Value().size() > limits.maximumArchiveBytes)
            return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ProtectionInvalid));
        auto owned = std::make_shared<const std::vector<std::byte>>(std::move(plaintext).Value());
        return reader.Read(std::move(owned));
    }

    /** @copydoc AdmitUnencryptedLocalSaveArchive */
    Result<ValidatedSaveArchive> AdmitUnencryptedLocalSaveArchive(std::vector<std::byte> archive, const SaveProtectionPolicy policy,
                                                                  const SaveArchiveReader &reader) {
        if (policy != SaveProtectionPolicy::UnencryptedLocal)
            return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ProtectionRequired));
        return reader.Read(std::make_shared<const std::vector<std::byte>>(std::move(archive)));
    }
}  // namespace Horo::Runtime
