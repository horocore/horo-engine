#include "Horo/Runtime/Save/SaveArchiveAuthenticity.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>
#include <utility>

namespace Horo::Runtime {
    namespace {
        constexpr std::array<std::byte, 8> Magic{std::byte{'H'}, std::byte{'O'}, std::byte{'R'}, std::byte{'O'},
                                                 std::byte{'S'}, std::byte{'A'}, std::byte{'V'}, std::byte{'E'}};
        constexpr std::string_view SignatureDomain = "HoroSave.Signature.v1";

        template <typename Value> [[nodiscard]] Value ReadLe(const std::span<const std::byte> bytes, const std::size_t offset) noexcept {
            Value value{};
            for (std::size_t i = 0; i < sizeof(Value); ++i)
                value |= static_cast<Value>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (i * 8U);
            return value;
        }

        struct TrailerView final {
            std::span<const std::byte> bytes;
            SaveArchiveSignatureAlgorithm algorithm{};
        };

        [[nodiscard]] Result<TrailerView> Preflight(const std::span<const std::byte> archive, const std::size_t maximumArchiveBytes) {
            if (maximumArchiveBytes == 0 || archive.size() > maximumArchiveBytes ||
                archive.size() < SaveArchivePreambleByteLength + SaveArchiveUnsignedTrailerByteLength ||
                !std::equal(Magic.begin(), Magic.end(), archive.begin()))
                return Result<TrailerView>::Failure(MakeError(SaveErrors::ArchiveEnvelopeInvalid));
            if (ReadLe<std::uint32_t>(archive, 8) != 1 || ReadLe<std::uint32_t>(archive, 12) != 0 ||
                ReadLe<std::uint32_t>(archive, 28) != 0)
                return Result<TrailerView>::Failure(MakeError(SaveErrors::ArchiveEnvelopeInvalid));
            const auto payloadBytes = ReadLe<std::uint64_t>(archive, 16);
            const auto trailerBytes = ReadLe<std::uint32_t>(archive, 24);
            if ((trailerBytes != SaveArchiveUnsignedTrailerByteLength && trailerBytes != SaveArchiveSignedTrailerByteLength) ||
                payloadBytes > archive.size() - SaveArchivePreambleByteLength ||
                trailerBytes != archive.size() - SaveArchivePreambleByteLength - payloadBytes)
                return Result<TrailerView>::Failure(MakeError(SaveErrors::ArchiveEnvelopeInvalid));
            const auto trailer = archive.last(trailerBytes);
            const auto algorithm = static_cast<SaveArchiveSignatureAlgorithm>(ReadLe<std::uint16_t>(trailer, 32));
            const auto signatureBytes = ReadLe<std::uint16_t>(trailer, 50);
            const bool zeroKey = std::ranges::all_of(trailer.subspan(34, 16), [](const std::byte value) {
                return value == std::byte{};
            });
            if (algorithm == SaveArchiveSignatureAlgorithm::None) {
                if (trailerBytes != SaveArchiveUnsignedTrailerByteLength || signatureBytes != 0 || !zeroKey)
                    return Result<TrailerView>::Failure(MakeError(SaveErrors::ArchiveEnvelopeInvalid));
            } else if (algorithm == SaveArchiveSignatureAlgorithm::Ed25519) {
                if (trailerBytes != SaveArchiveSignedTrailerByteLength || signatureBytes != 64 || zeroKey)
                    return Result<TrailerView>::Failure(MakeError(SaveErrors::ArchiveEnvelopeInvalid));
            } else {
                return Result<TrailerView>::Failure(MakeError(SaveErrors::ProtectionUnsupported));
            }
            return Result<TrailerView>::Success({trailer, algorithm});
        }

        [[nodiscard]] std::vector<std::byte> SignatureMessage(const std::span<const std::byte> trailer) {
            std::vector<std::byte> message;
            message.reserve(SignatureDomain.size() + 1 + 2 + 16 + 2 + 32);
            for (const char value : SignatureDomain)
                message.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
            message.push_back(std::byte{});
            message.push_back(trailer[32]);
            message.push_back(trailer[33]);
            message.insert(message.end(), trailer.begin() + 34, trailer.begin() + 50);
            message.push_back(trailer[50]);
            message.push_back(trailer[51]);
            message.insert(message.end(), trailer.begin(), trailer.begin() + 32);
            return message;
        }

        [[nodiscard]] const ErrorCodeDescriptor &SafeVerifierError(const Error &error) noexcept {
            if (error.domain.Value() != SaveErrors::ProtectionUnavailable.domain.Value())
                return SaveErrors::ProtectionAuthenticationFailed;
            const auto &code = error.code.Value();
            if (code == SaveErrors::ProtectionKeyRotated.code.Value())
                return SaveErrors::ProtectionKeyRotated;
            if (code == SaveErrors::ProtectionKeyRevoked.code.Value())
                return SaveErrors::ProtectionKeyRevoked;
            if (code == SaveErrors::ProtectionUnsupported.code.Value())
                return SaveErrors::ProtectionUnsupported;
            if (code == SaveErrors::ProtectionUnavailable.code.Value())
                return SaveErrors::ProtectionUnavailable;
            return SaveErrors::ProtectionAuthenticationFailed;
        }
    }  // namespace

    /** @copydoc AdmitSignedSaveArchive */
    Result<ValidatedSaveArchive> AdmitSignedSaveArchive(std::vector<std::byte> archive, const SaveSignaturePolicy policy,
                                                        const std::span<const std::byte> trustedScope,
                                                        SaveArchiveSignatureProvider *provider, const SaveArchiveReader &reader,
                                                        const std::size_t maximumArchiveBytes) {
        if (trustedScope.empty() || trustedScope.size() > 4'096 ||
            (policy != SaveSignaturePolicy::Disabled && policy != SaveSignaturePolicy::Optional && policy != SaveSignaturePolicy::Required))
            return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ProtectionInvalid));
        auto trailer = Preflight(archive, maximumArchiveBytes);
        if (trailer.HasError())
            return Result<ValidatedSaveArchive>::Failure(trailer.ErrorValue());
        if (trailer.Value().algorithm == SaveArchiveSignatureAlgorithm::None) {
            if (policy == SaveSignaturePolicy::Required)
                return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::SignatureRequired));
        } else {
            if (policy == SaveSignaturePolicy::Disabled)
                return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::SignatureDisallowed));
            if (provider == nullptr)
                return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ProtectionUnavailable));
            if (!provider->SupportsEd25519())
                return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ProtectionUnsupported));
            std::array<std::uint8_t, 16> keyId{};
            for (std::size_t i = 0; i < keyId.size(); ++i)
                keyId[i] = std::to_integer<std::uint8_t>(trailer.Value().bytes[34 + i]);
            const auto message = SignatureMessage(trailer.Value().bytes);
            auto verified = provider->Verify(keyId, trustedScope, message, trailer.Value().bytes.subspan(52, 64));
            if (verified.HasError())
                return Result<ValidatedSaveArchive>::Failure(MakeError(SafeVerifierError(verified.ErrorValue())));
        }
        // Reader checks the signed digest against exact archive bytes before metadata or payload decode.
        return reader.Read(std::make_shared<const std::vector<std::byte>>(std::move(archive)));
    }
}  // namespace Horo::Runtime
