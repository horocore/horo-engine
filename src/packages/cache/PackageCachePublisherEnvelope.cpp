#include "PackageCachePublisherEnvelope.h"

#include <algorithm>
#include <array>

namespace Horo::Packages::CacheDetail {
    namespace {
        const ErrorCodeDescriptor InvalidEnvelope{ErrorDomainId{"horo.packages.cache"},
                                                  ErrorCode{"packages.cache.publisher_evidence_invalid"}, ErrorSeverity::Error,
                                                  "Detached publisher cache evidence is invalid.",
                                                  "Restore exact bounded signature evidence from the authoritative source."};
        constexpr std::array<std::byte, 8> Magic{std::byte{'H'}, std::byte{'P'}, std::byte{'S'}, std::byte{'I'},
                                                 std::byte{'G'}, std::byte{1},   std::byte{0},   std::byte{0}};
        constexpr std::size_t PrefixBytes = Magic.size() + 1 + 6 + 32;

        /** @brief Applies neutral printable identity bounds without imposing package or trust policy. */
        bool ValidIdentity(const std::string_view text, const std::size_t maximum) {
            return !text.empty() && text.size() <= maximum && std::ranges::all_of(text, [](const unsigned char value) {
                return value >= 0x21 && value <= 0x7e;
            });
        }

        /** @brief Bounds storage fields and binds the envelope to its exact archive digest. */
        bool ValidEnvelope(const Security::DetachedSignatureEnvelope &value, const Sha256Digest &digest) {
            return value.algorithm == Security::SignatureAlgorithm::EcdsaP256Sha256 && value.artifactDigest == digest &&
                   ValidIdentity(value.publisherId, 255) && ValidIdentity(value.keyId, 256) && !value.signature.empty() &&
                   value.signature.size() <= 1024;
        }

        /** @brief Emits one already-bounded unsigned length in canonical big-endian order. */
        void AppendSize(std::vector<std::byte> &bytes, const std::size_t count) {
            bytes.push_back(static_cast<std::byte>((count >> 8) & 255));
            bytes.push_back(static_cast<std::byte>(count & 255));
        }

        /** @brief Reads a fixed prefix length after the caller admitted the complete prefix. */
        std::size_t ReadSize(const std::span<const std::byte> bytes, const std::size_t offset) {
            return (std::to_integer<std::size_t>(bytes[offset]) << 8) | std::to_integer<std::size_t>(bytes[offset + 1]);
        }

        /** @brief Appends admitted identity bytes without an encoding or locale conversion. */
        void AppendText(std::vector<std::byte> &bytes, const std::string_view text) {
            const auto span = std::as_bytes(std::span{text.data(), text.size()});
            bytes.insert(bytes.end(), span.begin(), span.end());
        }

        /** @brief Copies one checked storage slice into its owning identity value. */
        std::string ReadText(const std::span<const std::byte> bytes) {
            std::string value;
            value.reserve(bytes.size());
            for (const auto byte : bytes)
                value.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
            return value;
        }
    }  // namespace

    /** @copydoc EncodePublisherEnvelope */
    Result<std::vector<std::byte>> EncodePublisherEnvelope(const Security::DetachedSignatureEnvelope &signature,
                                                           const Sha256Digest &digest) {
        if (!ValidEnvelope(signature, digest))
            return Result<std::vector<std::byte>>::Failure(MakeError(InvalidEnvelope));
        std::vector<std::byte> bytes{Magic.begin(), Magic.end()};
        bytes.push_back(std::byte{1});  // Fixed cache encoding of the sole supported ECDSA algorithm.
        AppendSize(bytes, signature.publisherId.size());
        AppendSize(bytes, signature.keyId.size());
        AppendSize(bytes, signature.signature.size());
        for (const auto value : digest.bytes)
            bytes.push_back(static_cast<std::byte>(value));
        AppendText(bytes, signature.publisherId);
        AppendText(bytes, signature.keyId);
        bytes.insert(bytes.end(), signature.signature.begin(), signature.signature.end());
        return Result<std::vector<std::byte>>::Success(std::move(bytes));
    }

    /** @copydoc DecodePublisherEnvelope */
    Result<Security::DetachedSignatureEnvelope> DecodePublisherEnvelope(const std::span<const std::byte> bytes,
                                                                        const Sha256Digest &digest) {
        using Outcome = Result<Security::DetachedSignatureEnvelope>;
        if (bytes.size() < PrefixBytes || bytes.size() > MaximumPublisherEnvelopeBytes ||
            !std::ranges::equal(bytes.first(Magic.size()), Magic) || bytes[8] != std::byte{1})
            return Outcome::Failure(MakeError(InvalidEnvelope));
        const auto publisherBytes = ReadSize(bytes, 9);
        const auto keyBytes = ReadSize(bytes, 11);
        const auto signatureBytes = ReadSize(bytes, 13);
        // Field ceilings precede addition and slicing; their sum cannot overflow size_t.
        if (publisherBytes == 0 || publisherBytes > 255 || keyBytes == 0 || keyBytes > 256 || signatureBytes == 0 ||
            signatureBytes > 1024 || bytes.size() != PrefixBytes + publisherBytes + keyBytes + signatureBytes)
            return Outcome::Failure(MakeError(InvalidEnvelope));
        Security::DetachedSignatureEnvelope value;
        for (std::size_t index = 0; index < value.artifactDigest.bytes.size(); ++index)
            value.artifactDigest.bytes[index] = std::to_integer<std::uint8_t>(bytes[15 + index]);
        value.publisherId = ReadText(bytes.subspan(PrefixBytes, publisherBytes));
        value.keyId = ReadText(bytes.subspan(PrefixBytes + publisherBytes, keyBytes));
        const auto signature = bytes.subspan(PrefixBytes + publisherBytes + keyBytes, signatureBytes);
        value.signature.assign(signature.begin(), signature.end());
        if (!ValidEnvelope(value, digest))
            return Outcome::Failure(MakeError(InvalidEnvelope));
        return Outcome::Success(std::move(value));
    }
}  // namespace Horo::Packages::CacheDetail
