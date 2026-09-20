#include "Horo/Runtime/Save/SaveArchiveReader.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveArchiveReaderInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Horo::Runtime {
    namespace {
        constexpr std::array<std::byte, 8> ArchiveMagic{std::byte{'H'}, std::byte{'O'}, std::byte{'R'}, std::byte{'O'},
                                                        std::byte{'S'}, std::byte{'A'}, std::byte{'V'}, std::byte{'E'}};
        constexpr std::array<std::byte, 8> ContainerMagic{std::byte{'H'}, std::byte{'S'}, std::byte{'C'}, std::byte{'T'},
                                                          std::byte{'N'}, std::byte{'R'}, std::byte{'1'}, std::byte{0}};
        constexpr std::size_t SignatureHashByteLength = 32;
        constexpr std::size_t SignatureKeyIdByteLength = 16;
        constexpr std::size_t SignatureDescriptorByteLength = 20;
        constexpr std::size_t MaximumContainerNestingDepth = 32;
        using SaveArchiveReaderDetail::RawEntry;

        [[nodiscard]] Error ReaderError(const ErrorCodeDescriptor &descriptor, const std::size_t offset,
                                        const std::string_view path = "archive") {
            Error error = MakeError(descriptor);
            error.diagnostics.push_back(
                {DiagnosticCode{"save.archive.reader.location"},
                 DiagnosticSeverity::Error,
                 std::string{descriptor.summary},
                 {"archive", 0,
                  static_cast<std::uint32_t>(std::min(offset, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())))},
                 std::string{path}});
            return error;
        }

        template <typename Value>
        [[nodiscard]] bool ReadLittleEndian(const std::span<const std::byte> bytes, std::size_t &offset, Value &value) noexcept {
            static_assert(std::is_unsigned_v<Value>);
            if (offset > bytes.size() || sizeof(Value) > bytes.size() - offset)
                return false;
            value = 0;
            for (std::size_t index = 0; index < sizeof(Value); ++index)
                value |= static_cast<Value>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
            offset += sizeof(Value);
            return true;
        }

        [[nodiscard]] bool ReadArray(const std::span<const std::byte> bytes, std::size_t &offset,
                                     const std::span<std::uint8_t> destination) noexcept {
            if (offset > bytes.size() || destination.size() > bytes.size() - offset)
                return false;
            for (std::size_t index = 0; index < destination.size(); ++index)
                destination[index] = std::to_integer<std::uint8_t>(bytes[offset + index]);
            offset += destination.size();
            return true;
        }

        [[nodiscard]] bool IsZero(
            const std::span<const std::uint8_t> bytes) noexcept {  // NOSONAR(cpp:S1144) -- identity fields use uint8_t storage.
            return std::ranges::all_of(bytes, [](const std::uint8_t value) {
                return value == 0;
            });
        }

        [[nodiscard]] bool IsZero(const std::span<const std::byte> bytes) noexcept {
            return std::ranges::all_of(bytes, [](const std::byte value) {
                return value == std::byte{};
            });
        }

        [[nodiscard]] bool HasValidArchiveLimits(const SaveArchiveReaderLimits &limits) noexcept {
            return limits.maximumArchiveBytes != 0 && limits.maximumStoredPayloadBytes != 0 &&
                   limits.maximumStoredPayloadBytes <= limits.maximumArchiveBytes && limits.maximumDecodedBytes != 0;
        }

        [[nodiscard]] bool HasValidStructuralLimits(const SaveArchiveReaderLimits &limits) noexcept {
            return limits.maximumEntries != 0 && limits.maximumNestingDepth != 0 &&
                   limits.maximumNestingDepth <= MaximumContainerNestingDepth && limits.maximumExpansionRatio != 0;
        }

        [[nodiscard]] bool HasValidNestedLimits(const SaveArchiveReaderLimits &limits) noexcept {
            return limits.metadata.maximumNestingDepth <= limits.maximumNestingDepth &&
                   limits.chunks.maximumEntries <= limits.maximumEntries &&
                   limits.chunks.maximumPayloadBytes <= limits.maximumStoredPayloadBytes &&
                   limits.chunks.maximumDecodedChunkBytes <= limits.maximumDecodedBytes;
        }

        [[nodiscard]] bool ValidLimits(const SaveArchiveReaderLimits &limits) noexcept {
            return HasValidArchiveLimits(limits) && HasValidStructuralLimits(limits) && HasValidNestedLimits(limits);
        }

        struct EnvelopeFields final {
            std::uint32_t archiveVersion{};
            std::uint32_t flags{};
            std::uint64_t payloadLength{};
            std::uint32_t trailerLength{};
        };

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

        [[nodiscard]] Result<void> ValidateEnvelopeHeader(const EnvelopeFields &fields, const SaveArchiveReaderLimits &limits) {
            if (fields.archiveVersion == 0)
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveEnvelopeInvalid, 8, "envelope/archiveFormatVersion"));
            if (fields.archiveVersion > 1)
                return Result<void>::Failure(ReaderError(SaveErrors::VersionUnsupportedNewer, 8, "envelope/archiveFormatVersion"));
            if (fields.payloadLength > limits.maximumStoredPayloadBytes || fields.payloadLength > std::numeric_limits<std::size_t>::max())
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveFramingLimitExceeded, 16, "envelope/payloadLength"));
            if (fields.trailerLength != SaveArchiveUnsignedTrailerByteLength && fields.trailerLength != SaveArchiveSignedTrailerByteLength)
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveFramingLimitExceeded, 24, "envelope/trailerLength"));
            return Result<void>::Success();
        }

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
                return Result<std::pair<SaveArchiveSignatureInfo, Sha256Digest>>::Failure(std::move(valid).ErrorValue());
            return Result<std::pair<SaveArchiveSignatureInfo, Sha256Digest>>::Success(
                {{.algorithm = algorithm, .signerKeyId = keyId, .signatureByteLength = signatureByteLength}, {.bytes = digestBytes}});
        }

        [[nodiscard]] Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>> ReadEnvelope(
            const std::span<const std::byte> archive, const SaveArchiveReaderLimits &limits, SaveArchiveSignatureInfo &signature) {
            if (archive.size() > limits.maximumArchiveBytes)
                return Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>>::Failure(
                    ReaderError(SaveErrors::ArchiveFramingLimitExceeded, 0, "envelope/archiveBytes"));
            auto fields = ReadEnvelopeHeader(archive);
            if (fields.HasError())
                return Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>>::Failure(std::move(fields).ErrorValue());
            if (auto valid = ValidateEnvelopeHeader(fields.Value(), limits); valid.HasError())
                return Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>>::Failure(std::move(valid).ErrorValue());
            auto lengths = ValidateEnvelopeLength(fields.Value(), archive);
            if (lengths.HasError())
                return Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>>::Failure(std::move(lengths).ErrorValue());
            const auto [payloadSize, trailerSize] = lengths.Value();
            auto trailer = ReadTrailer(archive.subspan(SaveArchivePreambleByteLength + payloadSize, trailerSize),
                                       SaveArchivePreambleByteLength + payloadSize);
            if (trailer.HasError())
                return Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>>::Failure(std::move(trailer).ErrorValue());
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

        struct ContainerInfo final {
            std::size_t dataOffset{};
            std::size_t entryCount{};
        };

        struct ContainerHeader final {
            std::uint32_t version{};
            std::uint32_t flags{};
            std::uint64_t count{};
            std::uint32_t recordSize{};
            std::uint32_t reserved{};
        };

        [[nodiscard]] Result<ContainerHeader> ReadContainerHeader(const std::span<const std::byte> payload) {
            if (payload.size() < SaveArchiveContainerHeaderByteLength)
                return Result<ContainerHeader>::Failure(ReaderError(SaveErrors::ArchiveContainerInvalid, payload.size(), "container"));
            std::array<std::byte, 8> magic{};
            std::ranges::copy(payload.first(magic.size()), magic.begin());
            if (magic != ContainerMagic)
                return Result<ContainerHeader>::Failure(ReaderError(SaveErrors::ArchiveContainerInvalid, 0, "container/magic"));
            std::size_t offset = magic.size();
            ContainerHeader header;
            if (!ReadLittleEndian(payload, offset, header.version) || !ReadLittleEndian(payload, offset, header.flags) ||
                !ReadLittleEndian(payload, offset, header.count) || !ReadLittleEndian(payload, offset, header.recordSize) ||
                !ReadLittleEndian(payload, offset, header.reserved))
                return Result<ContainerHeader>::Failure(ReaderError(SaveErrors::ArchiveContainerInvalid, offset, "container/header"));
            return Result<ContainerHeader>::Success(header);
        }

        [[nodiscard]] Result<void> ValidateContainerHeader(const ContainerHeader &header, const SaveArchiveReaderLimits &limits) {
            if (header.version != 1 || header.flags != 0 || header.reserved != 0 ||
                header.recordSize != SaveArchiveContainerEntryByteLength)
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveContainerInvalid, 8, "container/header"));
            if (header.count == 0 || header.count > limits.maximumEntries || header.count > limits.chunks.maximumEntries ||
                header.count > std::numeric_limits<std::size_t>::max())
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveFramingLimitExceeded, 16, "container/entryCount"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<ContainerInfo> MakeContainerInfo(const ContainerHeader &header, const std::span<const std::byte> payload) {
            const auto entryCount = static_cast<std::size_t>(header.count);
            if (entryCount >
                (std::numeric_limits<std::size_t>::max() - SaveArchiveContainerHeaderByteLength) / SaveArchiveContainerEntryByteLength)
                return Result<ContainerInfo>::Failure(ReaderError(SaveErrors::ArchiveFramingLimitExceeded, 16, "container/table"));
            const std::size_t dataOffset = SaveArchiveContainerHeaderByteLength + entryCount * SaveArchiveContainerEntryByteLength;
            if (dataOffset > payload.size())
                return Result<ContainerInfo>::Failure(ReaderError(SaveErrors::ArchivePayloadTruncated, dataOffset, "container/table"));
            return Result<ContainerInfo>::Success({dataOffset, entryCount});
        }

        [[nodiscard]] Result<ContainerInfo> ReadContainerInfo(const std::span<const std::byte> payload,
                                                              const SaveArchiveReaderLimits &limits) {
            auto header = ReadContainerHeader(payload);
            if (header.HasError())
                return Result<ContainerInfo>::Failure(std::move(header).ErrorValue());
            if (auto valid = ValidateContainerHeader(header.Value(), limits); valid.HasError())
                return Result<ContainerInfo>::Failure(std::move(valid).ErrorValue());
            return MakeContainerInfo(header.Value(), payload);
        }

        [[nodiscard]] Result<void> ReadRawEntryIdentity(const std::span<const std::byte> payload, std::size_t &offset, RawEntry &entry,
                                                        std::uint16_t &ownerReserved, const std::size_t recordOffset) {
            if (!ReadArray(payload, offset, entry.record) || !ReadLittleEndian(payload, offset, entry.ownerLength) ||
                !ReadLittleEndian(payload, offset, ownerReserved) || !ReadArray(payload, offset, entry.owner))
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveEntryInvalid, recordOffset, "entry/identity"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ReadRawEntryRange(const std::span<const std::byte> payload, std::size_t &offset, RawEntry &entry,
                                                     const std::size_t recordOffset) {
            if (!ReadLittleEndian(payload, offset, entry.relativeOffset) || !ReadLittleEndian(payload, offset, entry.storedByteLength) ||
                !ReadLittleEndian(payload, offset, entry.decodedByteLength) || !ReadLittleEndian(payload, offset, entry.alignment))
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveEntryInvalid, recordOffset, "entry/range"));
            return Result<void>::Success();
        }

        struct RawEntryPrefix final {
            std::uint8_t kind{};
            std::uint8_t flags{};
            std::uint16_t codec{};
            std::uint32_t reserved{};
        };

        [[nodiscard]] Result<RawEntryPrefix> ReadRawEntryPrefix(const std::span<const std::byte> payload, std::size_t &offset,
                                                                const std::size_t recordOffset) {
            RawEntryPrefix prefix;
            if (!ReadLittleEndian(payload, offset, prefix.kind) || !ReadLittleEndian(payload, offset, prefix.flags) ||
                !ReadLittleEndian(payload, offset, prefix.codec) || !ReadLittleEndian(payload, offset, prefix.reserved))
                return Result<RawEntryPrefix>::Failure(ReaderError(SaveErrors::ArchiveEntryInvalid, recordOffset, "entry/fields"));
            return Result<RawEntryPrefix>::Success(prefix);
        }

        [[nodiscard]] Result<RawEntry> ReadRawEntry(const std::span<const std::byte> payload, const std::size_t recordOffset) {
            std::size_t offset = recordOffset;
            auto prefix = ReadRawEntryPrefix(payload, offset, recordOffset);
            if (prefix.HasError())
                return Result<RawEntry>::Failure(std::move(prefix).ErrorValue());
            std::uint16_t ownerReserved{};
            std::uint32_t reserved{};
            RawEntry entry;
            entry.codec = prefix.Value().codec;
            if (auto valid = ReadRawEntryIdentity(payload, offset, entry, ownerReserved, recordOffset); valid.HasError())
                return Result<RawEntry>::Failure(std::move(valid).ErrorValue());
            if (auto valid = ReadRawEntryRange(payload, offset, entry, recordOffset); valid.HasError())
                return Result<RawEntry>::Failure(std::move(valid).ErrorValue());
            if (!ReadLittleEndian(payload, offset, reserved) || !ReadArray(payload, offset, entry.decodedHash.bytes))
                return Result<RawEntry>::Failure(ReaderError(SaveErrors::ArchiveEntryInvalid, recordOffset, "entry/integrity"));
            if (prefix.Value().flags != 0 || ownerReserved != 0 || reserved != 0)
                return Result<RawEntry>::Failure(ReaderError(SaveErrors::ArchiveEntryInvalid, recordOffset, "entry/reserved"));
            entry.kind = static_cast<SaveArchiveEntryKind>(prefix.Value().kind);
            return Result<RawEntry>::Success(std::move(entry));
        }

        [[nodiscard]] Result<void> ValidateEntryKindValue(const RawEntry &entry, const std::size_t recordOffset) {
            using enum SaveArchiveEntryKind;
            if (entry.ownerLength > entry.owner.size())
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveStringInvalid, recordOffset, "entry/ownerLength"));
            if (entry.kind == Extension)
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveExtensionInvalid, recordOffset, "entry/extension"));
            if (entry.kind != Header && entry.kind != Manifest && entry.kind != Chunk)
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveEntryInvalid, recordOffset, "entry/kind"));
            if (entry.codec != static_cast<std::uint16_t>(SaveChunkCodec::Raw))
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveCodecUnsupported, recordOffset, "entry/codec"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateEntryIdentity(const RawEntry &entry, const std::size_t recordOffset, const bool sawChunk) {
            if (entry.kind == SaveArchiveEntryKind::Chunk) {
                if (entry.ownerLength == 0 || IsZero(entry.record))
                    return Result<void>::Failure(ReaderError(SaveErrors::ArchiveEntryInvalid, recordOffset, "entry/chunkIdentity"));
            } else if (sawChunk || entry.ownerLength != 0 || !IsZero(entry.record) || entry.alignment != 1) {
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveEntryInvalid, recordOffset, "entry/metadataIdentity"));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateStoredEntrySize(const RawEntry &entry, const SaveArchiveReaderLimits &limits,
                                                           const std::span<const std::byte> payload, const std::size_t dataOffset,
                                                           const std::size_t recordOffset) {
            if (entry.storedByteLength == 0 || entry.decodedByteLength == 0 || entry.storedByteLength > payload.size() - dataOffset ||
                entry.storedByteLength != entry.decodedByteLength || entry.alignment == 0 || !std::has_single_bit(entry.alignment) ||
                entry.alignment > limits.chunks.maximumAlignment)
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveEntryInvalid, recordOffset, "entry/bounds"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateEntryExpansion(const RawEntry &entry, const SaveArchiveReaderLimits &limits,
                                                          const std::size_t recordOffset) {
            if (entry.decodedByteLength > limits.maximumDecodedBytes || entry.decodedByteLength > limits.chunks.maximumDecodedChunkBytes ||
                entry.storedByteLength > std::numeric_limits<std::uint64_t>::max() / limits.maximumExpansionRatio ||
                entry.decodedByteLength > entry.storedByteLength * limits.maximumExpansionRatio)
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveDecompressionLimitExceeded, recordOffset, "entry/expansion"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::size_t> ValidateEntryRange(RawEntry &entry, const std::span<const std::byte> payload,
                                                             const std::size_t dataOffset, const std::size_t expectedRelativeOffset,
                                                             const std::size_t recordOffset) {
            if (entry.relativeOffset != expectedRelativeOffset)
                return Result<std::size_t>::Failure(ReaderError(SaveErrors::ArchiveDirectoryInvalid, recordOffset, "entry/contiguous"));
            if (entry.storedByteLength > std::numeric_limits<std::size_t>::max() - expectedRelativeOffset ||
                entry.storedByteLength > payload.size() - dataOffset - expectedRelativeOffset)
                return Result<std::size_t>::Failure(ReaderError(SaveErrors::ArchivePayloadTruncated, recordOffset, "entry/range"));
            entry.absoluteOffset = dataOffset + entry.relativeOffset;
            if (entry.absoluteOffset % entry.alignment != 0)
                return Result<std::size_t>::Failure(ReaderError(SaveErrors::ArchiveEntryInvalid, recordOffset, "entry/alignment"));
            return Result<std::size_t>::Success(expectedRelativeOffset + static_cast<std::size_t>(entry.storedByteLength));
        }

        [[nodiscard]] Result<RawEntry> ReadContainerEntry(const std::span<const std::byte> payload, const SaveArchiveReaderLimits &limits,
                                                          const ContainerInfo &info, const std::size_t index,
                                                          std::size_t &expectedRelativeOffset, bool &sawChunk,
                                                          std::uint64_t &decodedTotal) {
            const std::size_t recordOffset = SaveArchiveContainerHeaderByteLength + index * SaveArchiveContainerEntryByteLength;
            auto entry = ReadRawEntry(payload, recordOffset);
            if (entry.HasError())
                return Result<RawEntry>::Failure(std::move(entry).ErrorValue());
            auto parsedEntry = std::move(entry).Value();
            if (auto valid = ValidateEntryKindValue(parsedEntry, recordOffset); valid.HasError())
                return Result<RawEntry>::Failure(std::move(valid).ErrorValue());
            if (auto valid = ValidateEntryIdentity(parsedEntry, recordOffset, sawChunk); valid.HasError())
                return Result<RawEntry>::Failure(std::move(valid).ErrorValue());
            if (auto valid = ValidateStoredEntrySize(parsedEntry, limits, payload, info.dataOffset, recordOffset); valid.HasError())
                return Result<RawEntry>::Failure(std::move(valid).ErrorValue());
            if (auto valid = ValidateEntryExpansion(parsedEntry, limits, recordOffset); valid.HasError())
                return Result<RawEntry>::Failure(std::move(valid).ErrorValue());
            auto validatedRange = ValidateEntryRange(parsedEntry, payload, info.dataOffset, expectedRelativeOffset, recordOffset);
            if (validatedRange.HasError())
                return Result<RawEntry>::Failure(std::move(validatedRange).ErrorValue());
            expectedRelativeOffset = std::move(validatedRange).Value();
            sawChunk |= parsedEntry.kind == SaveArchiveEntryKind::Chunk;
            if (decodedTotal > limits.maximumDecodedBytes - parsedEntry.decodedByteLength)
                return Result<RawEntry>::Failure(
                    ReaderError(SaveErrors::ArchiveDecompressionLimitExceeded, recordOffset, "container/decodedBytes"));
            decodedTotal += parsedEntry.decodedByteLength;
            return Result<RawEntry>::Success(std::move(parsedEntry));
        }

        [[nodiscard]] Result<std::vector<RawEntry>> ReadContainer(const std::span<const std::byte> payload,
                                                                  const SaveArchiveReaderLimits &limits) {
            auto info = ReadContainerInfo(payload, limits);
            if (info.HasError())
                return Result<std::vector<RawEntry>>::Failure(std::move(info).ErrorValue());
            std::vector<RawEntry> entries;
            entries.reserve(info.Value().entryCount);
            std::uint64_t decodedTotal = 0;
            std::size_t expectedRelativeOffset = 0;
            bool sawChunk = false;
            for (std::size_t index = 0; index < info.Value().entryCount; ++index) {
                auto entry = ReadContainerEntry(payload, limits, info.Value(), index, expectedRelativeOffset, sawChunk, decodedTotal);
                if (entry.HasError())
                    return Result<std::vector<RawEntry>>::Failure(std::move(entry).ErrorValue());
                entries.push_back(std::move(entry).Value());
            }
            if (expectedRelativeOffset != payload.size() - info.Value().dataOffset || entries.size() < 3 ||
                entries[0].kind != SaveArchiveEntryKind::Header || entries[1].kind != SaveArchiveEntryKind::Manifest)
                return Result<std::vector<RawEntry>>::Failure(
                    ReaderError(SaveErrors::ArchiveDirectoryInvalid, info.Value().dataOffset, "container/data"));
            return Result<std::vector<RawEntry>>::Success(std::move(entries));
        }

    }  // namespace

    namespace SaveArchiveReaderDetail {
        struct Reader final {
            [[nodiscard]] static Result<ValidatedSaveArchive> ReadArchive(const std::span<const std::byte> archive,
                                                                          std::shared_ptr<const std::vector<std::byte>> ownedArchive,
                                                                          const SaveArchiveReaderLimits &limits) {
                if (!ValidLimits(limits))
                    return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ArchiveFramingLimitExceeded));
                try {
                    SaveArchiveSignatureInfo signature;
                    auto envelope = ReadEnvelope(archive, limits, signature);
                    if (envelope.HasError())
                        return Result<ValidatedSaveArchive>::Failure(std::move(envelope).ErrorValue());
                    auto [preamble, integrity] = std::move(envelope).Value();
                    if (auto verified = VerifySaveArchiveIntegrity(integrity, archive); verified.HasError())
                        return Result<ValidatedSaveArchive>::Failure(std::move(verified).ErrorValue());

                    const auto payload =
                        archive.subspan(SaveArchivePreambleByteLength, static_cast<std::size_t>(preamble.payloadByteLength));
                    auto rawEntries = ReadContainer(payload, limits);
                    if (rawEntries.HasError())
                        return Result<ValidatedSaveArchive>::Failure(std::move(rawEntries).ErrorValue());
                    const auto &entries = rawEntries.Value();
                    auto metadata = DecodeMetadata(payload, entries, limits.metadata);
                    if (metadata.HasError())
                        return Result<ValidatedSaveArchive>::Failure(std::move(metadata).ErrorValue());
                    auto metadataValue = std::move(metadata).Value();
                    auto validatedDirectory = BuildDirectory(payload, entries, metadataValue.manifest, limits);
                    if (validatedDirectory.HasError())
                        return Result<ValidatedSaveArchive>::Failure(std::move(validatedDirectory).ErrorValue());
                    auto validated = std::move(validatedDirectory).Value();
                    if (auto chunks = VerifyDecodedChunks(payload, validated, limits); chunks.HasError())
                        return Result<ValidatedSaveArchive>::Failure(std::move(chunks).ErrorValue());
                    return Result<ValidatedSaveArchive>::Success(
                        ValidatedSaveArchive{archive, std::move(ownedArchive), std::move(preamble), std::move(integrity), signature,
                                             std::move(metadataValue.header), std::move(metadataValue.manifest), std::move(validated)});
                } catch (const std::bad_alloc &) {
                    return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ArchiveAllocationFailed));
                }
            }
        };
    }  // namespace SaveArchiveReaderDetail

    ValidatedSaveArchive::ValidatedSaveArchive(  // NOSONAR(cpp:S107) -- construction owns all validated archive components as one
                                                 // invariant.
        std::span<const std::byte> archive, std::shared_ptr<const std::vector<std::byte>> ownedArchive, SaveArchivePreamble preamble,
        SaveArchiveIntegrityManifest integrity, SaveArchiveSignatureInfo signature, SaveArchiveHeader header, SaveGameManifest manifest,
        ValidatedSaveChunkDirectory directory) noexcept
        : archive_(archive), ownedArchive_(std::move(ownedArchive)), preamble_(std::move(preamble)), integrity_(std::move(integrity)),
          signature_(std::move(signature)), header_(std::move(header)), manifest_(std::move(manifest)), directory_(std::move(directory)),
          payload_(archive_.subspan(SaveArchivePreambleByteLength, static_cast<std::size_t>(preamble_.payloadByteLength))) {}

    const SaveArchivePreamble &ValidatedSaveArchive::Preamble() const noexcept {
        return preamble_;
    }

    const SaveArchiveIntegrityManifest &ValidatedSaveArchive::Integrity() const noexcept {
        return integrity_;
    }

    const SaveArchiveSignatureInfo &ValidatedSaveArchive::Signature() const noexcept {
        return signature_;
    }

    const SaveArchiveHeader &ValidatedSaveArchive::Header() const noexcept {
        return header_;
    }

    const SaveGameManifest &ValidatedSaveArchive::Manifest() const noexcept {
        return manifest_;
    }

    const ValidatedSaveChunkDirectory &ValidatedSaveArchive::Directory() const noexcept {
        return directory_;
    }

    std::span<const std::byte> ValidatedSaveArchive::Payload() const noexcept {
        return payload_;
    }

    Result<std::optional<std::span<const std::byte>>> ValidatedSaveArchive::SelectChunk(const SaveRecordId record) const {
        return SelectSaveChunkPayload(payload_, directory_, record);
    }

    SaveArchiveReader::SaveArchiveReader(SaveArchiveReaderLimits limits) : limits_(std::move(limits)) {}

    Result<ValidatedSaveArchive> SaveArchiveReader::Read(const std::span<const std::byte> archive) const {
        return SaveArchiveReaderDetail::Reader::ReadArchive(archive, {}, limits_);
    }

    Result<ValidatedSaveArchive> SaveArchiveReader::Read(std::shared_ptr<const std::vector<std::byte>> archive) const {
        if (!archive)
            return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ArchiveEnvelopeInvalid));
        const std::span<const std::byte> archiveSpan{*archive};
        return SaveArchiveReaderDetail::Reader::ReadArchive(archiveSpan, std::move(archive), limits_);
    }
}  // namespace Horo::Runtime
