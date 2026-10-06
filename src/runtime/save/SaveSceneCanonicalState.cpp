#include "Horo/Runtime/Save/SaveSceneCanonicalState.h"

#include "Horo/Runtime/Save/SaveArchiveReader.h"
#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"
#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Runtime {
    namespace {
        /** @brief Bounds layout decoding independently of unknown payload byte lengths. */
        CanonicalCodecLimits LayoutLimits() {
            return {.maximumBytes = 1U << 20U,
                    .maximumDecodedBytes = 2U << 20U,
                    .maximumStringBytes = 128,
                    .maximumCollectionElements = MaximumRuntimeSaveCaptureRecords,
                    .maximumFields = 3,
                    .maximumNestingDepth = 2,
                    .maximumReadWorkBytes = 2U << 20U};
        }

        /** @brief Requires closed ordered classifications and the mandatory self-known entry. */
        Result<void> ValidateLayout(std::span<const SaveSceneCanonicalLayoutEntry> entries) {
            if (entries.empty() || entries.size() > MaximumRuntimeSaveCaptureRecords)
                return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
            bool hasSelf = false;
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const auto &entry = entries[index];
                if (!entry.record.IsValid() ||
                    (entry.representation != SaveSceneCanonicalRepresentation::Known &&
                     entry.representation != SaveSceneCanonicalRepresentation::Opaque) ||
                    (index != 0 && !(entries[index - 1].record < entry.record)))
                    return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
                if (entry.record == SaveSceneCanonicalLayoutRecord()) {
                    if (entry.representation != SaveSceneCanonicalRepresentation::Known)
                        return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
                    hasSelf = true;
                }
            }
            return hasSelf ? Result<void>::Success() : Result<void>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
        }

        /** @brief Reads exactly one stable UUID and closed representation tag. */
        Result<SaveSceneCanonicalLayoutEntry> DecodeLayoutEntry(const CanonicalDecodedValue &encoded) {
            auto opened = encoded.OpenReader();
            if (opened.HasError())
                return Result<SaveSceneCanonicalLayoutEntry>::Failure(opened.ErrorValue());
            auto reader = std::move(opened).Value();
            auto identity = reader.ReadBytes(16);
            if (identity.HasError())
                return Result<SaveSceneCanonicalLayoutEntry>::Failure(identity.ErrorValue());
            if (identity.Value().size() != 16)
                return Result<SaveSceneCanonicalLayoutEntry>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
            auto representation = reader.ReadUInt8();
            if (representation.HasError())
                return Result<SaveSceneCanonicalLayoutEntry>::Failure(representation.ErrorValue());
            if (auto complete = reader.RequireFinished(); complete.HasError())
                return Result<SaveSceneCanonicalLayoutEntry>::Failure(complete.ErrorValue());
            std::array<std::uint8_t, 16> id{};
            std::ranges::transform(identity.Value(), id.begin(), [](std::byte value) {
                return std::to_integer<std::uint8_t>(value);
            });
            auto record = SaveRecordId::FromBytes(id);
            if (record.HasError())
                return Result<SaveSceneCanonicalLayoutEntry>::Failure(record.ErrorValue());
            return Result<SaveSceneCanonicalLayoutEntry>::Success(
                {std::move(record).Value(), static_cast<SaveSceneCanonicalRepresentation>(representation.Value())});
        }

        /** @brief Selects stored logical bytes without decoding an unsupported opaque codec. */
        std::span<const std::byte> PayloadBytes(const SaveSceneCanonicalRecord &record) {
            if (const auto *known = std::get_if<std::span<const std::byte>>(&record.payload))
                return *known;
            return std::get<SaveSceneCanonicalOpaque>(record.payload).storedBytes;
        }

        /** @brief Validates the closed opaque frame; directory evidence is serialized exactly rather than reinterpreted. */
        bool ValidPayload(const SaveSceneCanonicalRecord &record) {
            if (record.payload.valueless_by_exception())
                return false;
            if (const auto *opaque = std::get_if<SaveSceneCanonicalOpaque>(&record.payload))
                return opaque->storedByteLength == opaque->storedBytes.size() && opaque->alignment != 0 &&
                       (opaque->alignment & (opaque->alignment - 1)) == 0;
            return true;
        }

        /** @brief Bounds logical tuple count/bytes and rejects cross-owner duplicate record identities before serialization. */
        Result<void> ValidateParticipants(std::span<const SaveSceneCanonicalParticipant> participants, std::uint64_t maximumBytes) {
            if (participants.size() > MaximumSaveParticipantCount || maximumBytes == 0 ||
                maximumBytes > std::numeric_limits<std::size_t>::max())
                return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            std::vector<SaveRecordId> records;
            std::uint64_t bytes = 0;
            for (std::size_t index = 0; index < participants.size(); ++index) {
                const auto &participant = participants[index];
                if (!participant.participant.IsValid() || !participant.schema.IsValid() || participant.records.empty() ||
                    (index != 0 && !(participants[index - 1].participant < participant.participant)) ||
                    participant.records.size() > MaximumRuntimeSaveCaptureRecords - records.size())
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                for (std::size_t recordIndex = 0; recordIndex < participant.records.size(); ++recordIndex) {
                    const auto &record = participant.records[recordIndex];
                    if (!record.record.IsValid() || !ValidPayload(record) || PayloadBytes(record).size() > maximumBytes - bytes ||
                        (recordIndex != 0 && !(participant.records[recordIndex - 1].record < record.record)))
                        return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                    bytes += PayloadBytes(record).size();
                    records.push_back(record.record);
                }
            }
            std::ranges::sort(records);
            if (std::ranges::adjacent_find(records) != records.end())
                return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            return Result<void>::Success();
        }

        /** @brief Owns selected known bytes while opaque records borrow the reader's immutable source. */
        struct ArchiveCanonicalRecords final {
            std::vector<std::vector<std::byte>> decoded;
            std::uint64_t remainingBytes;

            Result<SaveSceneCanonicalRecord> Select(const ValidatedSaveArchive &archive, const SaveChunkDirectoryEntry &entry,
                                                    SaveSceneCanonicalRepresentation representation) {
                const auto length =
                    representation == SaveSceneCanonicalRepresentation::Known ? entry.decodedByteLength : entry.storedByteLength;
                if (length > remainingBytes)
                    return Result<SaveSceneCanonicalRecord>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
                remainingBytes -= length;
                if (representation == SaveSceneCanonicalRepresentation::Opaque) {
                    return Result<SaveSceneCanonicalRecord>::Success(
                        {entry.record,
                         SaveSceneCanonicalOpaque{entry.codec, entry.storedByteLength, entry.decodedByteLength, entry.alignment,
                                                  entry.decodedHash,
                                                  archive.Payload().subspan(static_cast<std::size_t>(entry.offset),
                                                                            static_cast<std::size_t>(entry.storedByteLength))}});
                }
                auto selected = archive.SelectChunk(entry.record);
                if (selected.HasError())
                    return Result<SaveSceneCanonicalRecord>::Failure(selected.ErrorValue());
                if (!selected.Value())
                    return Result<SaveSceneCanonicalRecord>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
                decoded.push_back(std::move(*selected.Value()));
                return Result<SaveSceneCanonicalRecord>::Success({entry.record, std::span<const std::byte>{decoded.back()}});
            }
        };

        /** @brief Matches complete persisted classifications to the admitted directory and mandatory reserved owner. */
        Result<void> ValidateArchiveLayout(const ValidatedSaveArchive &archive, std::span<const SaveSceneCanonicalLayoutEntry> layout) {
            const auto directory = archive.Directory().Entries();
            if (layout.size() != directory.size())
                return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            for (std::size_t index = 0; index < layout.size(); ++index)
                if (layout[index].record != directory[index].record)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            const auto &participants = archive.Manifest().participants;
            const auto owner =
                std::ranges::find(participants, SaveSceneCanonicalLayoutParticipant(), &SaveManifestParticipant::participant);
            if (owner == participants.end() || !owner->required || owner->schemaVersion.Value() != 1 || owner->chunks.size() != 1 ||
                owner->chunks.front() != SaveSceneCanonicalLayoutRecord())
                return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            return Result<void>::Success();
        }

        /** @brief Serializes one schema-stable record without interpreting its participant-owned canonical bytes. */
        Result<CanonicalEncodedValue> EncodeRecord(const SaveSceneCanonicalRecord &record, const CanonicalCodecLimits &limits) {
            CanonicalValueWriter writer{limits};
            (void)writer.WriteBytes(std::as_bytes(std::span{record.record.Bytes()}));
            if (const auto *known = std::get_if<std::span<const std::byte>>(&record.payload)) {
                (void)writer.WriteUInt8(static_cast<std::uint8_t>(SaveSceneCanonicalRepresentation::Known));
                (void)writer.WriteBytes(*known);
            } else {
                const auto &opaque = std::get<SaveSceneCanonicalOpaque>(record.payload);
                (void)writer.WriteUInt8(static_cast<std::uint8_t>(SaveSceneCanonicalRepresentation::Opaque));
                (void)writer.WriteUInt16(static_cast<std::uint16_t>(opaque.codec));
                (void)writer.WriteUInt64(opaque.storedByteLength);
                (void)writer.WriteUInt64(opaque.decodedByteLength);
                (void)writer.WriteUInt32(opaque.alignment);
                (void)writer.WriteBytes(std::as_bytes(std::span{opaque.decodedHash.bytes}));
                (void)writer.WriteBytes(opaque.storedBytes);
            }
            return std::move(writer).Finalize();
        }

        /** @brief Serializes one present owner/schema and its stable ordered records. */
        Result<CanonicalEncodedValue> EncodeParticipant(const SaveSceneCanonicalParticipant &participant,
                                                        const CanonicalCodecLimits &limits) {
            std::vector<CanonicalEncodedValue> records;
            records.reserve(participant.records.size());
            for (const auto &record : participant.records) {
                auto encoded = EncodeRecord(record, limits);
                if (encoded.HasError())
                    return Result<CanonicalEncodedValue>::Failure(encoded.ErrorValue());
                records.push_back(std::move(encoded).Value());
            }
            CanonicalValueWriter writer{limits};
            (void)writer.WriteUtf8(participant.participant.Value());
            (void)writer.WriteUInt32(participant.schema.Value());
            (void)writer.WriteSequence(records);
            return std::move(writer).Finalize();
        }
    }  // namespace

    /** @copydoc SaveSceneCanonicalLayoutParticipant */
    SaveParticipantId SaveSceneCanonicalLayoutParticipant() {
        return SaveParticipantId::Parse("horo.save.scene.canonical.v2").Value();
    }

    /** @copydoc SaveSceneCanonicalLayoutRecord */
    SaveRecordId SaveSceneCanonicalLayoutRecord() {
        return SaveRecordId::Parse("2d576609-77ae-4fb2-bf4a-9e7b63cbc002").Value();
    }

    /** @copydoc EncodeSaveSceneCanonicalLayout */
    Result<CanonicalEncodedValue> EncodeSaveSceneCanonicalLayout(std::span<const SaveSceneCanonicalLayoutEntry> entries) {
        try {
            if (auto valid = ValidateLayout(entries); valid.HasError())
                return Result<CanonicalEncodedValue>::Failure(valid.ErrorValue());
            std::vector<CanonicalEncodedValue> encoded;
            encoded.reserve(entries.size());
            for (const auto &entry : entries) {
                CanonicalValueWriter element{LayoutLimits()};
                (void)element.WriteBytes(std::as_bytes(std::span{entry.record.Bytes()}));
                (void)element.WriteUInt8(static_cast<std::uint8_t>(entry.representation));
                auto value = std::move(element).Finalize();
                if (value.HasError())
                    return Result<CanonicalEncodedValue>::Failure(value.ErrorValue());
                encoded.push_back(std::move(value).Value());
            }
            CanonicalValueWriter writer{LayoutLimits()};
            (void)writer.WriteUInt32(1);
            (void)writer.WriteSequence(encoded);
            return std::move(writer).Finalize();
        } catch (const std::bad_alloc &) {
            return Result<CanonicalEncodedValue>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

    /** @copydoc DecodeSaveSceneCanonicalLayout */
    Result<std::vector<SaveSceneCanonicalLayoutEntry>> DecodeSaveSceneCanonicalLayout(std::span<const std::byte> bytes) {
        using Entries = std::vector<SaveSceneCanonicalLayoutEntry>;
        try {
            auto opened = CanonicalValueReader::Create(bytes, LayoutLimits());
            if (opened.HasError())
                return Result<Entries>::Failure(opened.ErrorValue());
            auto reader = std::move(opened).Value();
            auto version = reader.ReadUInt32();
            if (version.HasError())
                return Result<Entries>::Failure(version.ErrorValue());
            if (version.Value() != 1)
                return Result<Entries>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
            auto encoded = reader.ReadSequence();
            if (encoded.HasError())
                return Result<Entries>::Failure(encoded.ErrorValue());
            if (auto complete = reader.RequireFinished(); complete.HasError())
                return Result<Entries>::Failure(complete.ErrorValue());
            Entries entries;
            entries.reserve(encoded.Value().size());
            for (const auto &value : encoded.Value()) {
                auto entry = DecodeLayoutEntry(value);
                if (entry.HasError())
                    return Result<Entries>::Failure(entry.ErrorValue());
                entries.push_back(std::move(entry).Value());
            }
            if (auto valid = ValidateLayout(entries); valid.HasError())
                return Result<Entries>::Failure(valid.ErrorValue());
            return Result<Entries>::Success(std::move(entries));
        } catch (const std::bad_alloc &) {
            return Result<Entries>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

    /** @copydoc ValidateSaveSceneCanonicalArchive */
    Result<std::vector<SaveSceneCanonicalLayoutEntry>> ValidateSaveSceneCanonicalArchive(const ValidatedSaveArchive &archive,
                                                                                         const std::uint64_t maximumBytes) {
        using Entries = std::vector<SaveSceneCanonicalLayoutEntry>;
        if (archive.Manifest().saveSchemaVersion.Value() != SaveSceneCanonicalSchemaVersion || maximumBytes == 0 ||
            maximumBytes > std::numeric_limits<std::size_t>::max())
            return Result<Entries>::Failure(MakeError(SaveErrors::MigrationSourceUnsupported));
        try {
            const auto directory = archive.Directory().Entries();
            const auto self = std::ranges::lower_bound(directory, SaveSceneCanonicalLayoutRecord(), {}, &SaveChunkDirectoryEntry::record);
            if (self == directory.end() || self->record != SaveSceneCanonicalLayoutRecord() || self->decodedByteLength > (1U << 20U))
                return Result<Entries>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            auto selected = archive.SelectChunk(self->record);
            if (selected.HasError())
                return Result<Entries>::Failure(selected.ErrorValue());
            if (!selected.Value())
                return Result<Entries>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
            auto layout = DecodeSaveSceneCanonicalLayout(*selected.Value());
            if (layout.HasError())
                return Result<Entries>::Failure(layout.ErrorValue());
            if (auto valid = ValidateArchiveLayout(archive, layout.Value()); valid.HasError())
                return Result<Entries>::Failure(valid.ErrorValue());
            ArchiveCanonicalRecords storage{{}, maximumBytes};
            storage.decoded.reserve(directory.size());
            std::vector<std::vector<SaveSceneCanonicalRecord>> records;
            records.reserve(archive.Manifest().participants.size());
            std::vector<SaveSceneCanonicalParticipant> participants;
            participants.reserve(archive.Manifest().participants.size());
            for (const auto &owner : archive.Manifest().participants) {
                records.emplace_back();
                records.back().reserve(owner.chunks.size());
                for (const auto record : owner.chunks) {
                    const auto entry = std::ranges::lower_bound(directory, record, {}, &SaveChunkDirectoryEntry::record);
                    const auto classification =
                        std::ranges::lower_bound(layout.Value(), record, {}, &SaveSceneCanonicalLayoutEntry::record);
                    if (entry == directory.end() || classification == layout.Value().end() || entry->record != record ||
                        classification->record != record || entry->owner != owner.participant ||
                        (owner.required && classification->representation != SaveSceneCanonicalRepresentation::Known))
                        return Result<Entries>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                    auto value = storage.Select(archive, *entry, classification->representation);
                    if (value.HasError())
                        return Result<Entries>::Failure(value.ErrorValue());
                    records.back().push_back(std::move(value).Value());
                }
                participants.push_back({owner.participant, owner.schemaVersion, records.back()});
            }
            auto encoded = EncodeSaveSceneCanonicalState(archive.Header(), participants, maximumBytes);
            if (encoded.HasError())
                return Result<Entries>::Failure(encoded.ErrorValue());
            if (ComputeCanonicalStateHash(encoded.Value().Bytes()) != archive.Manifest().canonicalState)
                return Result<Entries>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            return layout;
        } catch (const std::bad_alloc &) {
            return Result<Entries>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

    /** @copydoc EncodeSaveSceneCanonicalState */
    Result<CanonicalEncodedValue> EncodeSaveSceneCanonicalState(const SaveArchiveHeader &header,
                                                                std::span<const SaveSceneCanonicalParticipant> participants,
                                                                const std::uint64_t maximumBytes) {
        if (!header.project.IsValid() || !header.world.IsValid() || !header.baseScene.IsValid())
            return Result<CanonicalEncodedValue>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
        try {
            if (auto valid = ValidateParticipants(participants, maximumBytes); valid.HasError())
                return Result<CanonicalEncodedValue>::Failure(valid.ErrorValue());
            const CanonicalCodecLimits limits{.maximumBytes = static_cast<std::size_t>(maximumBytes),
                                              .maximumDecodedBytes = static_cast<std::size_t>(maximumBytes),
                                              .maximumStringBytes = 128,
                                              .maximumCollectionElements = MaximumRuntimeSaveCaptureRecords,
                                              .maximumFields = 16,
                                              .maximumNestingDepth = 4,
                                              .maximumReadWorkBytes = static_cast<std::size_t>(maximumBytes)};
            std::vector<CanonicalEncodedValue> tuples;
            tuples.reserve(participants.size());
            for (const auto &participant : participants) {
                auto encoded = EncodeParticipant(participant, limits);
                if (encoded.HasError())
                    return Result<CanonicalEncodedValue>::Failure(encoded.ErrorValue());
                tuples.push_back(std::move(encoded).Value());
            }
            CanonicalValueWriter writer{limits};
            (void)writer.WriteBytes(std::as_bytes(std::span{header.project.Bytes()}));
            (void)writer.WriteBytes(std::as_bytes(std::span{header.world.Bytes()}));
            (void)writer.WriteBytes(std::as_bytes(std::span{header.baseScene.Bytes()}));
            (void)writer.WriteUInt32(SaveSceneCanonicalSchemaVersion);
            (void)writer.WriteSequence(std::span<const CanonicalEncodedValue>{});
            (void)writer.WriteSequence(tuples);
            return std::move(writer).Finalize();
        } catch (const std::bad_alloc &) {
            return Result<CanonicalEncodedValue>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }
}  // namespace Horo::Runtime
