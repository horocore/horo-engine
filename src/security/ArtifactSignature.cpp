#include "Horo/Security/ArtifactSignature.h"

#include "Horo/Security/SecurityErrors.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <span>
#include <string_view>

namespace Horo::Security {
    namespace {
        constexpr std::string_view SignatureDomain = "horo.security.artifact-signature.v1";
        constexpr std::size_t MaximumIdentityBytes = 256;
        constexpr std::size_t MaximumPublicKeyBytes = 1024;
        constexpr std::size_t MaximumSignatureBytes = 1024;
        constexpr std::uintmax_t MaximumNativeArtifactBytes = 1024ULL * 1024ULL * 1024ULL;

        [[nodiscard]] bool ValidIdentity(const std::string_view value) noexcept {
            return !value.empty() && value.size() <= MaximumIdentityBytes && std::ranges::all_of(value, [](const unsigned char character) {
                return character >= 0x21U && character <= 0x7eU;
            });
        }

        void Append(std::vector<std::byte> &output, const std::string_view value) {
            output.insert(output.end(), reinterpret_cast<const std::byte *>(value.data()),
                          reinterpret_cast<const std::byte *>(value.data() + value.size()));
        }

        [[nodiscard]] Result<std::vector<char>> ReadArtifact(const std::filesystem::path &path) {
            std::error_code ec;
            const std::uintmax_t size = std::filesystem::file_size(path, ec);
            if (ec || size > MaximumNativeArtifactBytes)
                return Result<std::vector<char>>::Failure(
                    MakeError(SecurityErrors::MissingEvidence, "Native artifact is unavailable or exceeds the verification bound."));
            std::ifstream input{path, std::ios::binary};
            if (!input)
                return Result<std::vector<char>>::Failure(
                    MakeError(SecurityErrors::MissingEvidence, "Native artifact could not be opened for verification."));
            std::vector<char> bytes(static_cast<std::size_t>(size));
            input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!input || static_cast<std::size_t>(input.gcount()) != bytes.size() || input.peek() != std::char_traits<char>::eof())
                return Result<std::vector<char>>::Failure(
                    MakeError(SecurityErrors::StaleEvidence, "Native artifact changed while it was being verified."));
            return Result<std::vector<char>>::Success(std::move(bytes));
        }

        [[nodiscard]] Result<void> ValidateEnvelope(const DetachedSignatureEnvelope &envelope) {
            if (!ValidIdentity(envelope.publisherId) || !ValidIdentity(envelope.keyId) || envelope.signature.empty() ||
                envelope.signature.size() > MaximumSignatureBytes)
                return Result<void>::Failure(MakeError(SecurityErrors::InvalidInput, "Signature envelope is malformed."));
            if (envelope.algorithm != SignatureAlgorithm::EcdsaP256Sha256)
                return Result<void>::Failure(MakeError(SecurityErrors::UnsupportedAlgorithm));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<TrustedSigningKey> ResolveTrustedKey(const TrustedRootStore *trustedRoots,
                                                                  const DetachedSignatureEnvelope &envelope) {
            if (trustedRoots == nullptr)
                return Result<TrustedSigningKey>::Failure(MakeError(SecurityErrors::UnknownSigningKey));
            std::optional<TrustedSigningKey> key = trustedRoots->Find(envelope.publisherId, envelope.keyId);
            if (!key)
                return Result<TrustedSigningKey>::Failure(MakeError(SecurityErrors::UnknownSigningKey));
            if (key->algorithm != envelope.algorithm)
                return Result<TrustedSigningKey>::Failure(MakeError(SecurityErrors::UnsupportedAlgorithm));
            return Result<TrustedSigningKey>::Success(std::move(*key));
        }
    }  // namespace

    /** @copydoc TrustedRootStore::Add */
    Result<void> TrustedRootStore::Add(TrustedSigningKey key) {
        if (!ValidIdentity(key.publisherId) || !ValidIdentity(key.keyId) || key.publicKey.empty() ||
            key.publicKey.size() > MaximumPublicKeyBytes)
            return Result<void>::Failure(
                MakeError(SecurityErrors::InvalidInput, "Trusted signing key is malformed or exceeds its bounds."));
        std::unique_lock lock{mutex_};
        if (std::ranges::any_of(keys_, [&](const TrustedSigningKey &candidate) {
            return candidate.publisherId == key.publisherId && candidate.keyId == key.keyId;
        }))
            return Result<void>::Failure(MakeError(SecurityErrors::InvalidInput, "Trusted signing key identity is already registered."));
        keys_.push_back(std::move(key));
        return Result<void>::Success();
    }

    /** @copydoc TrustedRootStore::Revoke */
    Result<void> TrustedRootStore::Revoke(const std::string_view publisherId, const std::string_view keyId) {
        std::unique_lock lock{mutex_};
        const auto key = std::ranges::find_if(keys_, [&](const TrustedSigningKey &candidate) {
            return candidate.publisherId == publisherId && candidate.keyId == keyId;
        });
        if (key == keys_.end())
            return Result<void>::Failure(MakeError(SecurityErrors::UnknownSigningKey));
        keys_.erase(key);
        return Result<void>::Success();
    }

    /** @copydoc TrustedRootStore::Find */
    std::optional<TrustedSigningKey> TrustedRootStore::Find(const std::string_view publisherId, const std::string_view keyId) const {
        std::shared_lock lock{mutex_};
        const auto key = std::ranges::find_if(keys_, [&](const TrustedSigningKey &candidate) {
            return candidate.publisherId == publisherId && candidate.keyId == keyId;
        });
        return key != keys_.end() ? std::optional<TrustedSigningKey>{*key} : std::nullopt;
    }

    VerifiedArtifactEvidence::VerifiedArtifactEvidence(const Sha256Digest &digest, std::string publisherId, std::string keyId)
        : digest_(digest), publisherId_(std::move(publisherId)), keyId_(std::move(keyId)) {}

    /** @copydoc VerifiedArtifactEvidence::ArtifactDigest */
    const Sha256Digest &VerifiedArtifactEvidence::ArtifactDigest() const noexcept {
        return digest_;
    }

    /** @copydoc VerifiedArtifactEvidence::PublisherId */
    std::string_view VerifiedArtifactEvidence::PublisherId() const noexcept {
        return publisherId_;
    }

    /** @copydoc VerifiedArtifactEvidence::KeyId */
    std::string_view VerifiedArtifactEvidence::KeyId() const noexcept {
        return keyId_;
    }

    /** @copydoc ComputeSignaturePayloadDigest */
    Sha256Digest ComputeSignaturePayloadDigest(const DetachedSignatureEnvelope &envelope) {
        std::vector<std::byte> payload;
        payload.reserve(SignatureDomain.size() + envelope.publisherId.size() + envelope.keyId.size() +
                        envelope.artifactDigest.bytes.size() + 3U);
        Append(payload, SignatureDomain);
        payload.push_back(static_cast<std::byte>(envelope.algorithm));
        Append(payload, envelope.publisherId);
        payload.push_back(std::byte{0});
        Append(payload, envelope.keyId);
        payload.push_back(std::byte{0});
        for (const std::uint8_t byte : envelope.artifactDigest.bytes)
            payload.push_back(static_cast<std::byte>(byte));
        return ComputeSha256(payload);
    }

    /** @copydoc ArtifactVerifier::ArtifactVerifier */
    ArtifactVerifier::ArtifactVerifier(std::shared_ptr<const SignatureProvider> provider,
                                       std::shared_ptr<const TrustedRootStore> trustedRoots)
        : provider_(std::move(provider)), trustedRoots_(std::move(trustedRoots)) {}

    /** @copydoc ArtifactVerifier::Verify */
    Result<VerifiedArtifactEvidence> ArtifactVerifier::Verify(const std::span<const std::byte> artifact,
                                                              const DetachedSignatureEnvelope &envelope) const {
        return VerifyDigest(ComputeSha256(artifact), envelope);
    }

    /** @copydoc ArtifactVerifier::VerifyFile */
    Result<VerifiedArtifactEvidence> ArtifactVerifier::VerifyFile(const std::filesystem::path &path, const std::uint64_t expectedBytes,
                                                                  const DetachedSignatureEnvelope &envelope) const {
        std::error_code error;
        if (const auto status = std::filesystem::symlink_status(path, error); error || !std::filesystem::is_regular_file(status))
            return Result<VerifiedArtifactEvidence>::Failure(MakeError(SecurityErrors::MissingEvidence));
        if (const auto actualBytes = std::filesystem::file_size(path, error); error || actualBytes != expectedBytes)
            return Result<VerifiedArtifactEvidence>::Failure(MakeError(SecurityErrors::MissingEvidence));
        std::ifstream input{path, std::ios::binary};
        if (!input)
            return Result<VerifiedArtifactEvidence>::Failure(MakeError(SecurityErrors::MissingEvidence));

        Sha256Builder hash;
        std::array<char, 64U * 1024U> buffer{};
        std::uint64_t remaining = expectedBytes;
        while (remaining > 0U) {
            const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, buffer.size()));
            input.read(buffer.data(), count);
            if (input.gcount() != count || !hash.Update(std::as_bytes(std::span{buffer.data(), static_cast<std::size_t>(count)})))
                return Result<VerifiedArtifactEvidence>::Failure(MakeError(SecurityErrors::StaleEvidence));
            remaining -= static_cast<std::uint64_t>(count);
        }
        if (input.peek() != std::char_traits<char>::eof() || input.bad())
            return Result<VerifiedArtifactEvidence>::Failure(MakeError(SecurityErrors::StaleEvidence));
        return VerifyDigest(hash.Finalize(), envelope);
    }

    /** @brief Verifies a digest computed from exact artifact bytes against trusted detached-signature evidence. */
    Result<VerifiedArtifactEvidence> ArtifactVerifier::VerifyDigest(const Sha256Digest &actualDigest,
                                                                    const DetachedSignatureEnvelope &envelope) const {
        if (auto valid = ValidateEnvelope(envelope); valid.HasError())
            return Result<VerifiedArtifactEvidence>::Failure(valid.ErrorValue());
        if (actualDigest != envelope.artifactDigest)
            return Result<VerifiedArtifactEvidence>::Failure(MakeError(SecurityErrors::IntegrityMismatch));
        auto key = ResolveTrustedKey(trustedRoots_.get(), envelope);
        if (key.HasError())
            return Result<VerifiedArtifactEvidence>::Failure(key.ErrorValue());
        if (!provider_)
            return Result<VerifiedArtifactEvidence>::Failure(MakeError(SecurityErrors::SignatureProviderUnavailable));
        if (!provider_->Supports(envelope.algorithm))
            return Result<VerifiedArtifactEvidence>::Failure(MakeError(SecurityErrors::UnsupportedAlgorithm));
        if (auto verified =
                provider_->Verify(envelope.algorithm, key.Value().publicKey, ComputeSignaturePayloadDigest(envelope), envelope.signature);
            verified.HasError())
            return Result<VerifiedArtifactEvidence>::Failure(verified.ErrorValue());
        return Result<VerifiedArtifactEvidence>::Success(VerifiedArtifactEvidence{actualDigest, envelope.publisherId, envelope.keyId});
    }

    /** @copydoc DetachedFileArtifactGate::DetachedFileArtifactGate */
    DetachedFileArtifactGate::DetachedFileArtifactGate(std::shared_ptr<const ArtifactVerifier> verifier, EnvelopeResolver resolver)
        : verifier_(std::move(verifier)), resolver_(std::move(resolver)) {}

    /** @copydoc DetachedFileArtifactGate::Verify */
    Result<VerifiedArtifactEvidence> DetachedFileArtifactGate::Verify(const std::filesystem::path &artifactPath) const {
        if (!verifier_ || !resolver_)
            return Result<VerifiedArtifactEvidence>::Failure(MakeError(SecurityErrors::MissingEvidence));
        auto envelope = resolver_(artifactPath);
        if (envelope.HasError())
            return Result<VerifiedArtifactEvidence>::Failure(envelope.ErrorValue());
        auto bytes = ReadArtifact(artifactPath);
        if (bytes.HasError())
            return Result<VerifiedArtifactEvidence>::Failure(bytes.ErrorValue());
        return verifier_->Verify(std::as_bytes(std::span{bytes.Value()}), envelope.Value());
    }
}  // namespace Horo::Security
