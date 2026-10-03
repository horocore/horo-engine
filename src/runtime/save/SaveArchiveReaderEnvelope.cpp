#include "SaveArchiveReaderInternal.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace Horo::Runtime::SaveArchiveReaderDetail {
    namespace {
        constexpr std::array<std::byte, 8> ArchiveMagic{std::byte{'H'}, std::byte{'O'}, std::byte{'R'}, std::byte{'O'},
                                                        std::byte{'S'}, std::byte{'A'}, std::byte{'V'}, std::byte{'E'}};
        constexpr std::size_t SignatureHashByteLength = 32;
        constexpr std::size_t SignatureKeyIdByteLength = 16;
        constexpr std::size_t SignatureDescriptorByteLength = 20;

        struct EnvelopeFields final {
            std::uint32_t archiveVersion{};
            std::uint32_t flags{};
            std::uint64_t payloadLength{};
            std::uint32_t trailerLength{};
        };

        /** @brief Reads the exact fixed envelope header before trusting any declared length. */
        [[nodiscard]] Result<EnvelopeFields> ReadEnvelopeHeader(const std::span<const std::byte> archive) {
            if (archive.size() < SaveArchivePreambleByteLength || !std::equal(ArchiveMagic.begin(), ArchiveMagic.end(), archive.begin()))
                return Result<EnvelopeFields>::Failure(ReaderError(SaveErrors::ArchiveEnvelopeInvalid, 0, "envelope"));
            std::size_t offset = ArchiveMagic.size();
            EnvelopeFields fields;
            std::uint32_t reserved{};
            if (!ReadLittleEndian(archive, offset, fields.archiveVersion) || !ReadLittleEndian(archive, offset, fields.flags) ||
                !ReadLittleEndian(archive, offset, fields.payloadLength) || !ReadLittleEndian(archive, offset, fields.trailerLength) ||
                !ReadLittleEndian(archive, offset, reserved))
                return Result<EnvelopeFields>::Failure(ReaderError(SaveErrors::ArchiveEnvelopeInvalid, offset, "envelope/fields"));
            if (fields.flags != 0 || reserved != 0)
                return Result<EnvelopeFields>::Failure(ReaderError(SaveErrors::ArchiveEnvelopeInvalid, 12, "envelope/reserved"));
            return Result<EnvelopeFields>::Success(fields);
        }

        /** @brief Applies supported format and finite payload/trailer bounds. */
        [[nodiscard]] Result<void> ValidateEnvelopeHeader(const EnvelopeFields &fields, const SaveArchiveReaderLimits &limits) {
            if (fields.archiveVersion == 0)
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveEnvelopeInvalid, 8, "envelope/archiveFormatVersion"));
            if (fields.archiveVersion > 2)
                return Result<void>::Failure(ReaderError(SaveErrors::VersionUnsupportedNewer, 8, "envelope/archiveFormatVersion"));
            if (fields.payloadLength > limits.maximumStoredPayloadBytes || fields.payloadLength > std::numeric_limits<std::size_t>::max())
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveFramingLimitExceeded, 16, "envelope/payloadLength"));
            if (fields.trailerLength != SaveArchiveUnsignedTrailerByteLength && fields.trailerLength != SaveArchiveSignedTrailerByteLength)
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveFramingLimitExceeded, 24, "envelope/trailerLength"));
            return Result<void>::Success();
        }

        /** @brief Requires declared payload and trailer to cover the exact archive. */
        [[nodiscard]] Result<std::pair<std::size_t, std::size_t>> ValidateEnvelopeLength(const EnvelopeFields &fields,
                                                                                         const std::span<const std::byte> archive) {
            const auto payloadSize = static_cast<std::size_t>(fields.payloadLength);
            const auto trailerSize = static_cast<std::size_t>(fields.trailerLength);
            if (payloadSize > archive.size() - SaveArchivePreambleByteLength ||
                trailerSize > archive.size() - SaveArchivePreambleByteLength - payloadSize ||
                SaveArchivePreambleByteLength + payloadSize + trailerSize != archive.size())
                return Result<std::pair<std::size_t, std::size_t>>::Failure(
                    ReaderError(SaveErrors::ArchivePayloadTruncated, 16, "envelope/fileLength"));
            return Result<std::pair<std::size_t, std::size_t>>::Success({payloadSize, trailerSize});
        }

        /** @brief Checks signed and unsigned trailer shapes before exposing a signature claim. */
        [[nodiscard]] Result<void> ValidateSignatureDescriptor(const SaveArchiveSignatureAlgorithm algorithm, const std::size_t trailerSize,
                                                               const std::uint16_t signatureLength,
                                                               const std::span<const std::uint8_t> keyId, const std::size_t offset) {
            if (algorithm == SaveArchiveSignatureAlgorithm::None) {
                if (trailerSize != SaveArchiveUnsignedTrailerByteLength || signatureLength != 0 || !IsZero(keyId))
                    return Result<void>::Failure(ReaderError(SaveErrors::ArchiveEnvelopeInvalid, offset, "trailer/unsigned"));
            } else if (algorithm == SaveArchiveSignatureAlgorithm::Ed25519) {
                if (trailerSize != SaveArchiveSignedTrailerByteLength || signatureLength != 64 || IsZero(keyId))
                    return Result<void>::Failure(ReaderError(SaveErrors::ArchiveEnvelopeInvalid, offset, "trailer/signed"));
            } else {
                return Result<void>::Failure(
                    ReaderError(SaveErrors::ArchiveEnvelopeInvalid, offset - SignatureDescriptorByteLength, "trailer/signatureAlgorithm"));
            }
            return Result<void>::Success();
        }

        /** @brief Reads trailer integrity and non-secret signature metadata. */
        [[nodiscard]] Result<std::pair<SaveArchiveSignatureInfo, Sha256Digest>> ReadTrailer(const std::span<const std::byte> trailer,
                                                                                            const std::size_t trailerOffsetInArchive) {
            std::size_t offset = 0;
            std::array<std::uint8_t, SignatureHashByteLength> digestBytes{};
            std::uint16_t signatureAlgorithmValue{};
            std::array<std::uint8_t, SignatureKeyIdByteLength> keyId{};
            std::uint16_t signatureByteLength{};
            if (!ReadArray(trailer, offset, digestBytes) || !ReadLittleEndian(trailer, offset, signatureAlgorithmValue) ||
                !ReadArray(trailer, offset, keyId) || !ReadLittleEndian(trailer, offset, signatureByteLength) ||
                signatureByteLength > trailer.size() - offset || signatureByteLength != trailer.size() - offset)
                return Result<std::pair<SaveArchiveSignatureInfo, Sha256Digest>>::Failure(
                    ReaderError(SaveErrors::ArchiveEnvelopeInvalid, trailerOffsetInArchive, "trailer/fields"));
            const auto algorithm = static_cast<SaveArchiveSignatureAlgorithm>(signatureAlgorithmValue);
            if (auto valid = ValidateSignatureDescriptor(algorithm, trailer.size(), signatureByteLength, keyId, offset); valid.HasError())
                return Result<std::pair<SaveArchiveSignatureInfo, Sha256Digest>>::Failure(valid.ErrorValue());
            return Result<std::pair<SaveArchiveSignatureInfo, Sha256Digest>>::Success(
                {{.algorithm = algorithm, .signerKeyId = keyId, .signatureByteLength = signatureByteLength}, {.bytes = digestBytes}});
        }
    }  // namespace

    Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>> ReadEnvelope(const std::span<const std::byte> archive,
                                                                                      const SaveArchiveReaderLimits &limits,
                                                                                      SaveArchiveSignatureInfo &signature) {
        if (archive.size() > limits.maximumArchiveBytes)
            return Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>>::Failure(
                ReaderError(SaveErrors::ArchiveFramingLimitExceeded, 0, "envelope/archiveBytes"));
        if (archive.size() > limits.maximumReadWorkBytes)
            return Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>>::Failure(ReadWorkError(0, "envelope/readWork"));
        auto fields = ReadEnvelopeHeader(archive);
        if (fields.HasError())
            return Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>>::Failure(fields.ErrorValue());
        if (auto valid = ValidateEnvelopeHeader(fields.Value(), limits); valid.HasError())
            return Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>>::Failure(valid.ErrorValue());
        auto lengths = ValidateEnvelopeLength(fields.Value(), archive);
        if (lengths.HasError())
            return Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>>::Failure(lengths.ErrorValue());
        const auto [payloadSize, trailerSize] = lengths.Value();
        auto trailer = ReadTrailer(archive.subspan(SaveArchivePreambleByteLength + payloadSize, trailerSize),
                                   SaveArchivePreambleByteLength + payloadSize);
        if (trailer.HasError())
            return Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>>::Failure(trailer.ErrorValue());
        auto [trailerSignature, trailerDigest] = std::move(trailer).Value();
        auto version = ArchiveFormatVersion::Create(fields.Value().archiveVersion);
        if (version.HasError())
            return Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>>::Failure(
                ReaderError(SaveErrors::ArchiveEnvelopeInvalid, 8, "envelope/archiveFormatVersion"));
        signature = std::move(trailerSignature);
        SaveArchivePreamble preamble{.archiveFormatVersion = std::move(version).Value(),
                                     .flags = fields.Value().flags,
                                     .payloadByteLength = fields.Value().payloadLength,
                                     .trailerByteLength = fields.Value().trailerLength};
        SaveArchiveIntegrityManifest integrity{.algorithm = {},
                                               .archiveContent = {.value = std::move(trailerDigest)},
                                               .preambleByteLength = SaveArchivePreambleByteLength,
                                               .payloadByteLength = fields.Value().payloadLength,
                                               .trailerByteLength = fields.Value().trailerLength};
        return Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>>::Success({std::move(preamble), integrity});
    }
}  // namespace Horo::Runtime::SaveArchiveReaderDetail
