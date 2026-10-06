#include "Horo/Runtime/Save/SaveArchiveReader.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveArchiveReaderInternal.h"
#include "SaveChunkCompressionInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace Horo::Runtime {
    namespace {
        constexpr std::array<std::byte, 8> ContainerMagic{std::byte{'H'}, std::byte{'S'}, std::byte{'C'}, std::byte{'T'},
                                                          std::byte{'N'}, std::byte{'R'}, std::byte{'1'}, std::byte{0}};
        constexpr std::size_t MaximumContainerNestingDepth = 32;
        using SaveArchiveReaderDetail::IsZero;
        using SaveArchiveReaderDetail::RawEntry;
        using SaveArchiveReaderDetail::ReadArray;
        using SaveArchiveReaderDetail::ReaderError;
        using SaveArchiveReaderDetail::ReadLittleEndian;

        [[nodiscard]] bool HasValidArchiveLimits(const SaveArchiveReaderLimits &limits) noexcept {
            return limits.maximumArchiveBytes != 0 && limits.maximumStoredPayloadBytes != 0 &&
                   limits.maximumStoredPayloadBytes <= limits.maximumArchiveBytes && limits.maximumDecodedBytes != 0 &&
                   limits.maximumArchiveBytes <= (4ULL << 30U) && limits.maximumDecodedBytes <= (1ULL << 30U) &&
                   limits.maximumReadWorkBytes != 0 && limits.maximumReadWorkBytes <= (16ULL << 30U);
        }

        [[nodiscard]] bool HasValidStructuralLimits(const SaveArchiveReaderLimits &limits) noexcept {
            return limits.maximumEntries != 0 && limits.maximumNestingDepth != 0 &&
                   limits.maximumNestingDepth <= MaximumContainerNestingDepth && limits.maximumExpansionRatio != 0 &&
                   limits.maximumExpansionRatio <= 64 && limits.maximumEntries <= 65'536;
        }

        [[nodiscard]] bool HasValidNestedLimits(const SaveArchiveReaderLimits &limits) noexcept {
            return limits.metadata.maximumNestingDepth <= limits.maximumNestingDepth && limits.metadata.maximumHeaderBytes <= (1U << 20U) &&
                   limits.metadata.maximumManifestBytes <= (4U << 20U) && limits.metadata.maximumTextBytes <= 4'096 &&
                   limits.metadata.maximumParticipants <= 4'096 && limits.metadata.maximumTotalChunks <= limits.maximumEntries &&
                   limits.chunks.maximumEntries <= limits.maximumEntries &&
                   limits.chunks.maximumPayloadBytes <= limits.maximumStoredPayloadBytes &&
                   limits.chunks.maximumStoredChunkBytes <= limits.maximumStoredPayloadBytes && limits.chunks.maximumExpansionRatio != 0 &&
                   limits.chunks.maximumExpansionRatio <= 64 && limits.chunks.maximumDecodedChunkBytes <= limits.maximumDecodedBytes;
        }

        [[nodiscard]] bool ValidLimits(const SaveArchiveReaderLimits &limits) noexcept {
            return HasValidArchiveLimits(limits) && HasValidStructuralLimits(limits) && HasValidNestedLimits(limits);
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

        [[nodiscard]] Result<void> ValidateContainerHeader(const ContainerHeader &header, const SaveArchiveReaderLimits &limits,
                                                           const std::uint32_t archiveVersion) {
            if (header.version != archiveVersion || header.flags != 0 || header.reserved != 0 ||
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
                                                              const SaveArchiveReaderLimits &limits, const std::uint32_t archiveVersion) {
            auto header = ReadContainerHeader(payload);
            if (header.HasError())
                return Result<ContainerInfo>::Failure(header.ErrorValue());
            if (auto valid = ValidateContainerHeader(header.Value(), limits, archiveVersion); valid.HasError())
                return Result<ContainerInfo>::Failure(valid.ErrorValue());
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
                return Result<RawEntry>::Failure(prefix.ErrorValue());
            std::uint16_t ownerReserved{};
            std::uint32_t reserved{};
            RawEntry entry;
            entry.codec = prefix.Value().codec;
            if (auto valid = ReadRawEntryIdentity(payload, offset, entry, ownerReserved, recordOffset); valid.HasError())
                return Result<RawEntry>::Failure(valid.ErrorValue());
            if (auto valid = ReadRawEntryRange(payload, offset, entry, recordOffset); valid.HasError())
                return Result<RawEntry>::Failure(valid.ErrorValue());
            if (!ReadLittleEndian(payload, offset, reserved) || !ReadArray(payload, offset, entry.decodedHash.bytes))
                return Result<RawEntry>::Failure(ReaderError(SaveErrors::ArchiveEntryInvalid, recordOffset, "entry/integrity"));
            if (prefix.Value().flags != 0 || ownerReserved != 0 || reserved != 0)
                return Result<RawEntry>::Failure(ReaderError(SaveErrors::ArchiveEntryInvalid, recordOffset, "entry/reserved"));
            entry.kind = static_cast<SaveArchiveEntryKind>(prefix.Value().kind);
            return Result<RawEntry>::Success(std::move(entry));
        }

        [[nodiscard]] Result<void> ValidateEntryKindValue(const RawEntry &entry, const std::size_t recordOffset,
                                                          const std::uint32_t archiveVersion) {
            using enum SaveArchiveEntryKind;
            if (entry.ownerLength > entry.owner.size())
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveStringInvalid, recordOffset, "entry/ownerLength"));
            if (entry.kind == Extension)
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveExtensionInvalid, recordOffset, "entry/extension"));
            if (entry.kind != Header && entry.kind != Manifest && entry.kind != Chunk)
                return Result<void>::Failure(ReaderError(SaveErrors::ArchiveEntryInvalid, recordOffset, "entry/kind"));
            if (entry.codec != static_cast<std::uint16_t>(SaveChunkCodec::Raw) && (entry.kind != Chunk || archiveVersion < 2))
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
                entry.storedByteLength > limits.chunks.maximumStoredChunkBytes ||
                (entry.codec == static_cast<std::uint16_t>(SaveChunkCodec::Raw) && entry.storedByteLength != entry.decodedByteLength) ||
                entry.alignment == 0 || !std::has_single_bit(entry.alignment) || entry.alignment > limits.chunks.maximumAlignment)
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

        struct ContainerReadState final {
            std::size_t expectedRelativeOffset{};
            std::uint64_t decodedTotal{};
            bool sawChunk{};
        };

        [[nodiscard]] Result<RawEntry> ReadContainerEntry(const std::span<const std::byte> payload, const SaveArchiveReaderLimits &limits,
                                                          const ContainerInfo &info, const std::size_t index, ContainerReadState &state,
                                                          const std::uint32_t archiveVersion) {
            const std::size_t recordOffset = SaveArchiveContainerHeaderByteLength + index * SaveArchiveContainerEntryByteLength;
            auto entry = ReadRawEntry(payload, recordOffset);
            if (entry.HasError())
                return Result<RawEntry>::Failure(entry.ErrorValue());
            auto parsedEntry = std::move(entry).Value();
            if (auto valid = ValidateEntryKindValue(parsedEntry, recordOffset, archiveVersion); valid.HasError())
                return Result<RawEntry>::Failure(valid.ErrorValue());
            if (auto valid = ValidateEntryIdentity(parsedEntry, recordOffset, state.sawChunk); valid.HasError())
                return Result<RawEntry>::Failure(valid.ErrorValue());
            if (auto valid = ValidateStoredEntrySize(parsedEntry, limits, payload, info.dataOffset, recordOffset); valid.HasError())
                return Result<RawEntry>::Failure(valid.ErrorValue());
            if (auto valid = ValidateEntryExpansion(parsedEntry, limits, recordOffset); valid.HasError())
                return Result<RawEntry>::Failure(valid.ErrorValue());
            auto validatedRange = ValidateEntryRange(parsedEntry, payload, info.dataOffset, state.expectedRelativeOffset, recordOffset);
            if (validatedRange.HasError())
                return Result<RawEntry>::Failure(validatedRange.ErrorValue());
            state.expectedRelativeOffset = std::move(validatedRange).Value();
            state.sawChunk |= parsedEntry.kind == SaveArchiveEntryKind::Chunk;
            if (state.decodedTotal > limits.maximumDecodedBytes - parsedEntry.decodedByteLength)
                return Result<RawEntry>::Failure(
                    ReaderError(SaveErrors::ArchiveDecompressionLimitExceeded, recordOffset, "container/decodedBytes"));
            state.decodedTotal += parsedEntry.decodedByteLength;
            return Result<RawEntry>::Success(std::move(parsedEntry));
        }

        [[nodiscard]] Result<std::vector<RawEntry>> ReadContainer(const std::span<const std::byte> payload,
                                                                  const SaveArchiveReaderLimits &limits,
                                                                  const std::uint32_t archiveVersion) {
            auto info = ReadContainerInfo(payload, limits, archiveVersion);
            if (info.HasError())
                return Result<std::vector<RawEntry>>::Failure(info.ErrorValue());
            std::vector<RawEntry> entries;
            entries.reserve(info.Value().entryCount);
            ContainerReadState state;
            for (std::size_t index = 0; index < info.Value().entryCount; ++index) {
                auto entry = ReadContainerEntry(payload, limits, info.Value(), index, state, archiveVersion);
                if (entry.HasError())
                    return Result<std::vector<RawEntry>>::Failure(entry.ErrorValue());
                entries.push_back(std::move(entry).Value());
            }
            if (state.expectedRelativeOffset != payload.size() - info.Value().dataOffset || entries.size() < 3 ||
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
                        return Result<ValidatedSaveArchive>::Failure(envelope.ErrorValue());
                    auto [preamble, integrity] = std::move(envelope).Value();
                    // The integrity pass, container/metadata pass and directory pass can each
                    // inspect the stored archive once. Reserve that cumulative work up front.
                    if (archive.size() > limits.maximumReadWorkBytes / 3)
                        return Result<ValidatedSaveArchive>::Failure(ReadWorkError(0, "envelope/readWork"));
                    if (auto verified = VerifySaveArchiveIntegrity(integrity, archive); verified.HasError())
                        return Result<ValidatedSaveArchive>::Failure(verified.ErrorValue());

                    const auto payload =
                        archive.subspan(SaveArchivePreambleByteLength, static_cast<std::size_t>(preamble.payloadByteLength));
                    auto rawEntries = ReadContainer(payload, limits, preamble.archiveFormatVersion.Value());
                    if (rawEntries.HasError())
                        return Result<ValidatedSaveArchive>::Failure(rawEntries.ErrorValue());
                    const auto &entries = rawEntries.Value();
                    auto metadata = DecodeMetadata(payload, entries, limits.metadata);
                    if (metadata.HasError())
                        return Result<ValidatedSaveArchive>::Failure(metadata.ErrorValue());
                    auto metadataValue = std::move(metadata).Value();
                    auto validatedDirectory =
                        BuildDirectory(payload, entries, metadataValue.manifest, limits, preamble.archiveFormatVersion.Value());
                    if (validatedDirectory.HasError())
                        return Result<ValidatedSaveArchive>::Failure(validatedDirectory.ErrorValue());
                    auto validated = std::move(validatedDirectory).Value();
                    auto remainingReadWork = std::make_shared<std::atomic<std::uint64_t>>(limits.maximumReadWorkBytes -
                                                                                          static_cast<std::uint64_t>(archive.size()) * 3);
                    return Result<ValidatedSaveArchive>::Success(
                        ValidatedSaveArchive{ValidatedSaveArchive::Contents{.archive = archive,
                                                                            .ownedArchive = std::move(ownedArchive),
                                                                            .preamble = std::move(preamble),
                                                                            .integrity = std::move(integrity),
                                                                            .signature = signature,
                                                                            .header = std::move(metadataValue.header),
                                                                            .manifest = std::move(metadataValue.manifest),
                                                                            .directory = std::move(validated),
                                                                            .remainingReadWork = std::move(remainingReadWork)}});
                } catch (const std::bad_alloc &) {
                    return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::ArchiveAllocationFailed));
                }
            }
        };
    }  // namespace SaveArchiveReaderDetail

    ValidatedSaveArchive::ValidatedSaveArchive(Contents contents) noexcept
        : archive_(contents.archive), ownedArchive_(std::move(contents.ownedArchive)), preamble_(std::move(contents.preamble)),
          integrity_(std::move(contents.integrity)), signature_(std::move(contents.signature)), header_(std::move(contents.header)),
          manifest_(std::move(contents.manifest)), directory_(std::move(contents.directory)),
          payload_(archive_.subspan(SaveArchivePreambleByteLength, static_cast<std::size_t>(preamble_.payloadByteLength))),
          remainingReadWork_(std::move(contents.remainingReadWork)) {}

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

    Result<std::optional<std::vector<std::byte>>> ValidatedSaveArchive::SelectChunk(const SaveRecordId record) const {
        const auto entries = directory_.Entries();
        if (const auto found = std::ranges::lower_bound(entries, record, {}, &SaveChunkDirectoryEntry::record);
            found != entries.end() && found->record == record) {
            if (!SaveChunkCompressionDetail::Supports(found->codec))
                return Result<std::optional<std::vector<std::byte>>>::Failure(MakeError(SaveErrors::ArchiveCodecUnsupported));
            std::uint64_t remaining = remainingReadWork_->load(std::memory_order_relaxed);
            while (true) {
                const std::uint64_t work = found->storedByteLength + (found->codec == SaveChunkCodec::Raw ? 0 : found->decodedByteLength);
                if (work < found->storedByteLength || work > remaining)
                    return Result<std::optional<std::vector<std::byte>>>::Failure(
                        SaveArchiveReaderDetail::ReadWorkError(found->offset, "chunk/readWork"));
                if (remainingReadWork_->compare_exchange_weak(remaining, remaining - work, std::memory_order_relaxed))
                    break;
            }
        }
        return SelectSaveChunkPayload(payload_, directory_, record);
    }

    namespace {
        /** @brief Verifies one opaque optional owner's chunks and records its explicit disposition. */
        [[nodiscard]] Result<void> CollectUnknownParticipant(const ValidatedSaveArchive &archive,
                                                             const SaveManifestParticipant &participant,
                                                             const SaveCompatibilityPolicy &policy,
                                                             const std::uint64_t maximumPreservedBytes, std::uint64_t &retainedBytes,
                                                             SaveUnknownDataReport &report) {
            if (std::ranges::binary_search(policy.droppableUnknownParticipants, participant.participant)) {
                for (const SaveRecordId &record : participant.chunks) {
                    auto verified = archive.SelectChunk(record);
                    if (verified.HasError())
                        return Result<void>::Failure(verified.ErrorValue());
                    if (!verified.Value())
                        return Result<void>::Failure(MakeError(SaveErrors::ArchiveDirectoryInvalid));
                }
                report.dropped.push_back(participant.participant);
                return Result<void>::Success();
            }
            for (const SaveRecordId &record : participant.chunks) {
                const auto entries = archive.Directory().Entries();
                const auto found = std::ranges::lower_bound(entries, record, {}, &SaveChunkDirectoryEntry::record);
                if (found == entries.end() || found->record != record || found->owner != participant.participant)
                    return Result<void>::Failure(MakeError(SaveErrors::ArchiveDirectoryInvalid));
                if (found->storedByteLength > maximumPreservedBytes - retainedBytes)
                    return Result<void>::Failure(
                        MakeError(SaveErrors::ArchiveMetadataLimitExceeded, "Unknown optional data exceeds the preservation budget."));
                auto verified = archive.SelectChunk(record);
                if (verified.HasError())
                    return Result<void>::Failure(verified.ErrorValue());
                if (!verified.Value())
                    return Result<void>::Failure(MakeError(SaveErrors::ArchiveDirectoryInvalid));
                const auto stored =
                    archive.Payload().subspan(static_cast<std::size_t>(found->offset), static_cast<std::size_t>(found->storedByteLength));
                report.preserved.push_back({.entry = *found, .storedBytes = {stored.begin(), stored.end()}});
                retainedBytes += found->storedByteLength;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidatedSaveArchive::InspectUnknownData */
    Result<SaveUnknownDataReport> ValidatedSaveArchive::InspectUnknownData(const SaveCompatibilityPolicy &policy,
                                                                           const std::uint64_t maximumPreservedBytes) const {
        if (const SaveCompatibilityDecision decision =
                EvaluateSaveCompatibility(preamble_.archiveFormatVersion, header_, manifest_, policy);
            decision.disposition == SaveCompatibilityDisposition::Rejected) {
            using enum SaveCompatibilityReason;
            std::string reason = "Save cannot be restored: ";
            if (decision.reason == UnknownRequiredParticipant && decision.participant)
                reason += std::format("required module or content participant '{}' is unavailable.", decision.participant->Value());
            else if (decision.reason == UnsupportedParticipantSchema && decision.participant)
                reason += std::format("participant '{}' needs a supported module version or a migration.", decision.participant->Value());
            else if (decision.reason == MissingRequiredParticipant && decision.participant)
                reason += std::format("required participant '{}' is absent from the archive.", decision.participant->Value());
            else if (decision.reason == UnsupportedFeature)
                reason += std::format("unsupported required feature flags {}.", header_.featureFlags & ~policy.supportedFeatureFlagsMask);
            else
                reason += std::format("compatibility preflight rejected the archive (reason {}).", static_cast<unsigned>(decision.reason));
            return Result<SaveUnknownDataReport>::Failure(MakeError(SaveErrors::MigrationSourceUnsupported, std::move(reason)));
        }

        SaveUnknownDataReport report;
        std::uint64_t retainedBytes = 0;
        try {
            for (const SaveManifestParticipant &participant : manifest_.participants) {
                const auto installed =
                    std::ranges::lower_bound(policy.participants, participant.participant, {}, &SaveParticipantCompatibility::participant);
                if (const bool supported =
                        installed != policy.participants.end() && installed->participant == participant.participant &&
                        (installed->versions.direct.Contains(participant.schemaVersion) ||
                         (installed->versions.migrationSource && installed->versions.migrationSource->Contains(participant.schemaVersion)));
                    supported)
                    continue;
                if (participant.required)
                    return Result<SaveUnknownDataReport>::Failure(
                        MakeError(SaveErrors::MigrationSourceUnsupported,
                                  "Required module or content participant '" + participant.participant.Value() + "' is unavailable."));
                if (auto collected = CollectUnknownParticipant(*this, participant, policy, maximumPreservedBytes, retainedBytes, report);
                    collected.HasError())
                    return Result<SaveUnknownDataReport>::Failure(collected.ErrorValue());
            }
            std::ranges::sort(report.preserved, {}, [](const PreservedSaveChunk &chunk) {
                return chunk.entry.record;
            });
            return Result<SaveUnknownDataReport>::Success(std::move(report));
        } catch (const std::bad_alloc &) {
            return Result<SaveUnknownDataReport>::Failure(MakeError(SaveErrors::ArchiveAllocationFailed));
        }
    }

    /** @copydoc VerifyUnknownDataRoundTrip */
    Result<void> VerifyUnknownDataRoundTrip(const ValidatedSaveArchive &source, const ValidatedSaveArchive &destination,
                                            const SaveCompatibilityPolicy &policy, const std::uint64_t maximumPreservedBytes) {
        auto expected = source.InspectUnknownData(policy, maximumPreservedBytes);
        if (expected.HasError())
            return Result<void>::Failure(expected.ErrorValue());
        auto actual = destination.InspectUnknownData(policy, maximumPreservedBytes);
        if (actual.HasError())
            return Result<void>::Failure(actual.ErrorValue());
        const auto &actualChunks = actual.Value().preserved;
        for (const PreservedSaveChunk &chunk : expected.Value().preserved) {
            const auto found = std::ranges::lower_bound(actualChunks, chunk.entry.record, {}, [](const PreservedSaveChunk &value) {
                return value.entry.record;
            });
            if (found == actualChunks.end() || found->entry.record != chunk.entry.record || found->entry.owner != chunk.entry.owner ||
                found->entry.codec != chunk.entry.codec || found->entry.storedByteLength != chunk.entry.storedByteLength ||
                found->entry.decodedByteLength != chunk.entry.decodedByteLength || found->entry.alignment != chunk.entry.alignment ||
                found->entry.decodedHash != chunk.entry.decodedHash || found->storedBytes != chunk.storedBytes)
                return Result<void>::Failure(MakeError(SaveErrors::MigrationCandidateInvalid,
                                                       "Save copy or migration lost preservable optional data: participant '" +
                                                           chunk.entry.owner.Value() + "', record " + chunk.entry.record.ToString() + "."));
        }
        return Result<void>::Success();
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
