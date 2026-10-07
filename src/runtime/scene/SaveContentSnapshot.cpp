#include "Horo/Runtime/Save/SaveArchiveContainerWriter.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveContentInternal.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Runtime {
    namespace {
        /** @brief Owns the exact capture/retention merge and its stable logical representation through final readback. */
        struct ReSaveAssembly final {
            SaveGameManifest manifest;
            std::vector<PreservedSaveChunk> chunks;
            std::vector<SaveSceneCanonicalLayoutEntry> layout;
            std::vector<SaveParticipantId> retainedOwners;
            std::uint64_t remainingBytes;
            std::uint64_t remainingWork;
            std::uint64_t encodingLimit;

            static constexpr std::uint64_t WorkPasses = 40;
            static constexpr std::uint64_t RecordMetadataWork = 128U << 10U;

            /** @brief Conservatively reserves framing/JSON/reader work before materializing payloads or metadata.
             * @details The allowance covers two full reader admissions, logical readback, preservation comparison and
             *          metadata encoding; tight budgets may reject before the downstream exact reader would reject.
             */
            Result<void> AdmitMetadata(const SaveArchiveHeader &header, const RuntimeSaveSnapshot &capture,
                                       const SaveContentDetail::AcceptedCaptureSeal &source, const SaveArchiveReaderLimits &limits) {
                if (capture.Participants().size() > MaximumSaveParticipantCount ||
                    capture.Records().size() > MaximumRuntimeSaveCaptureRecords ||
                    source.sourceReader->Manifest().participants.size() > MaximumSaveParticipantCount ||
                    source.opaque->preserved.size() > MaximumRuntimeSaveCaptureRecords)
                    return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecLimitExceeded));
                if (auto valid = ValidateSaveArchiveHeader(header, limits.metadata); valid.HasError())
                    return valid;
                const auto textBytes = header.engineVersion.size();
                if (header.projectBuildId.size() > std::numeric_limits<std::uint64_t>::max() - textBytes ||
                    textBytes + header.projectBuildId.size() > (std::numeric_limits<std::uint64_t>::max() - 8192) / 6)
                    return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecLimitExceeded));
                // Six bytes is the maximum JSON escaping expansion of one input UTF-8 byte.
                const auto headerBytes =
                    std::max<std::uint64_t>(limits.metadata.maximumHeaderBytes, 8192 + 6 * (textBytes + header.projectBuildId.size()));
                const auto owners = capture.Participants().size() + source.sourceReader->Manifest().participants.size() + 1;
                const auto records = capture.Records().size() + source.opaque->preserved.size() + 1;
                // Valid participant identifiers are bounded by MaximumSaveParticipantIdBytes; canonical UUID references by 36 bytes.
                const auto manifestBytes =
                    std::max<std::uint64_t>(limits.metadata.maximumManifestBytes, 1024 + 2048 * owners + 64 * records);
                const auto directoryBytes = 188 * records + 84;
                if (headerBytes > std::numeric_limits<std::uint64_t>::max() - manifestBytes ||
                    headerBytes + manifestBytes > std::numeric_limits<std::uint64_t>::max() - directoryBytes)
                    return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecLimitExceeded));
                const auto metadata = headerBytes + manifestBytes + directoryBytes;
                if (metadata > remainingWork / WorkPasses)
                    return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecLimitExceeded));
                remainingWork -= metadata * WorkPasses;
                return Result<void>::Success();
            }

            /** @brief Reserves 40 payload passes and bounded owner lookup/sort/tuple work before any materialization. */
            Result<void> AdmitWork(const std::uint64_t logicalBytes, const std::uint64_t selectionWork = 0) {
                constexpr std::uint64_t metadataWork = RecordMetadataWork;
                if (remainingWork < metadataWork || selectionWork > remainingWork - metadataWork ||
                    logicalBytes > (remainingWork - metadataWork - selectionWork) / WorkPasses)
                    return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecLimitExceeded));
                remainingWork -= metadataWork + selectionWork + logicalBytes * WorkPasses;
                return Result<void>::Success();
            }

            /** @brief Bounds allocation before copying an immutable record's admitted segments. */
            Result<void> AddCapture(const RuntimeSaveSnapshot &capture) {
                for (const auto &participant : capture.Participants()) {
                    if (participant.scope == SaveParticipantScope::PersistentWorld ||
                        participant.participant == SaveSceneCanonicalLayoutParticipant())
                        return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                    if (participant.disposition == CanonicalCaptureDisposition::Captured)
                        manifest.participants.emplace_back(participant.participant, participant.schemaVersion, participant.required,
                                                           participant.records);
                }
                for (const auto &record : capture.Records()) {
                    if (record.ByteLength() > remainingBytes)
                        return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecLimitExceeded));
                    if (auto work = AdmitWork(record.ByteLength()); work.HasError())
                        return work;
                    remainingBytes -= record.ByteLength();
                    std::vector<std::byte> bytes;
                    bytes.reserve(static_cast<std::size_t>(record.ByteLength()));
                    for (std::size_t segment = 0; segment < record.SegmentCount(); ++segment) {
                        const auto part = record.Segment(segment);
                        if (part.size() > record.ByteLength() - bytes.size())
                            return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
                        bytes.insert(bytes.end(), part.begin(), part.end());
                    }
                    if (bytes.size() != record.ByteLength())
                        return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
                    const auto &identity = record.Record();
                    const auto digest = ComputeSha256(bytes);
                    chunks.emplace_back(SaveChunkDirectoryEntry{identity.record, identity.participant, 0, bytes.size(), bytes.size(), 1,
                                                                SaveChunkCodec::Raw, digest},
                                        std::move(bytes));
                    layout.emplace_back(identity.record, SaveSceneCanonicalRepresentation::Known);
                }
                return Result<void>::Success();
            }

            /** @brief Resolves only authenticated classifications; legacy retention stays opaque. */
            Result<SaveSceneCanonicalRepresentation> RetainedRepresentation(const SaveContentDetail::AcceptedCaptureSeal &source,
                                                                            const SaveRecordId &record) const {
                if (source.canonicalLayout->empty())
                    return Result<SaveSceneCanonicalRepresentation>::Success(SaveSceneCanonicalRepresentation::Opaque);
                const auto tag = std::ranges::lower_bound(*source.canonicalLayout, record, {}, &SaveSceneCanonicalLayoutEntry::record);
                if (tag == source.canonicalLayout->end() || tag->record != record)
                    return Result<SaveSceneCanonicalRepresentation>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                return Result<SaveSceneCanonicalRepresentation>::Success(tag->representation);
            }

            /** @brief Retains only original optional owners and rejects capture/retention overlap. */
            Result<void> AddRetainedOwner(const SaveContentDetail::AcceptedCaptureSeal &source, const SaveChunkDirectoryEntry &entry) {
                if (const auto owner = std::ranges::find(manifest.participants, entry.owner, &SaveManifestParticipant::participant);
                    owner != manifest.participants.end()) {
                    if (std::ranges::find(retainedOwners, owner->participant) == retainedOwners.end())
                        return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                    owner->chunks.push_back(entry.record);
                    return Result<void>::Success();
                }
                const auto &sourceParticipants = source.sourceReader->Manifest().participants;
                const auto original = std::ranges::find(sourceParticipants, entry.owner, &SaveManifestParticipant::participant);
                if (original == sourceParticipants.end() || original->required)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                retainedOwners.push_back(original->participant);
                manifest.participants.emplace_back(original->participant, original->schemaVersion, false,
                                                   std::vector<SaveRecordId>{entry.record});
                return Result<void>::Success();
            }

            /** @brief Carries exact source frames and their authenticated release-independent representation. */
            Result<void> AddRetained(const SaveContentDetail::AcceptedCaptureSeal &source) {
                for (const auto &chunk : source.opaque->preserved) {
                    auto tag = RetainedRepresentation(source, chunk.entry.record);
                    if (tag.HasError())
                        return Result<void>::Failure(tag.ErrorValue());
                    const auto representation = tag.Value();
                    const auto retainedBytes = representation == SaveSceneCanonicalRepresentation::Known
                                                   ? std::max(chunk.entry.storedByteLength, chunk.entry.decodedByteLength)
                                                   : chunk.entry.storedByteLength;
                    if (retainedBytes > remainingBytes)
                        return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecLimitExceeded));
                    std::uint64_t selectionWork{};
                    if (representation == SaveSceneCanonicalRepresentation::Known) {
                        selectionWork = chunk.entry.storedByteLength;
                        if (chunk.entry.codec != SaveChunkCodec::Raw)
                            selectionWork += chunk.entry.decodedByteLength;
                    }
                    if (selectionWork < chunk.entry.storedByteLength && representation == SaveSceneCanonicalRepresentation::Known)
                        return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
                    if (auto work = AdmitWork(retainedBytes, selectionWork); work.HasError())
                        return work;
                    remainingBytes -= retainedBytes;
                    layout.emplace_back(chunk.entry.record, representation);
                    chunks.push_back(chunk);
                    if (auto owner = AddRetainedOwner(source, chunk.entry); owner.HasError())
                        return owner;
                }
                return Result<void>::Success();
            }

            /** @brief Installs the required derived layout only after duplicate-free capture/retention admission. */
            Result<void> AddLayout() {
                if (layout.size() >= MaximumRuntimeSaveCaptureRecords)
                    return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecLimitExceeded));
                // Fixed codec widths: version/count (8), then length(4)+UUID length/data(20)+tag(1) per element.
                const std::uint64_t layoutBytes = 8 + 25 * (layout.size() + 1);
                if (layoutBytes > remainingBytes)
                    return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecLimitExceeded));
                if (auto work = AdmitWork(layoutBytes); work.HasError())
                    return work;
                remainingBytes -= layoutBytes;
                layout.emplace_back(SaveSceneCanonicalLayoutRecord(), SaveSceneCanonicalRepresentation::Known);
                std::ranges::sort(layout, {}, &SaveSceneCanonicalLayoutEntry::record);
                auto encoded = EncodeSaveSceneCanonicalLayout(layout);
                if (encoded.HasError())
                    return Result<void>::Failure(encoded.ErrorValue());
                if (encoded.Value().Bytes().size() != layoutBytes)
                    return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
                std::vector<std::byte> bytes(encoded.Value().Bytes().begin(), encoded.Value().Bytes().end());
                const auto digest = ComputeSha256(bytes);
                chunks.emplace_back(SaveChunkDirectoryEntry{SaveSceneCanonicalLayoutRecord(), SaveSceneCanonicalLayoutParticipant(), 0,
                                                            bytes.size(), bytes.size(), 1, SaveChunkCodec::Raw, digest},
                                    std::move(bytes));
                manifest.participants.emplace_back(SaveSceneCanonicalLayoutParticipant(), ParticipantSchemaVersion::Create(1).Value(), true,
                                                   std::vector<SaveRecordId>{SaveSceneCanonicalLayoutRecord()});
                std::ranges::sort(manifest.participants, {}, &SaveManifestParticipant::participant);
                for (auto &owner : manifest.participants)
                    std::ranges::sort(owner.chunks);
                std::ranges::sort(chunks, {}, [](const PreservedSaveChunk &chunk) {
                    return chunk.entry.record;
                });
                return Result<void>::Success();
            }

            /** @brief Borrows exact opaque/raw frames and owns only admitted supported decoded records. */
            Result<SaveSceneCanonicalRecord> SelectRecord(const PreservedSaveChunk &chunk,
                                                          const SaveSceneCanonicalRepresentation representation,
                                                          const SaveContentDetail::AcceptedCaptureSeal &source,
                                                          std::vector<std::vector<std::byte>> &decoded) const {
                const auto &entry = chunk.entry;
                if (representation == SaveSceneCanonicalRepresentation::Opaque)
                    return Result<SaveSceneCanonicalRecord>::Success(
                        {entry.record, SaveSceneCanonicalOpaque{entry.codec, entry.storedByteLength, entry.decodedByteLength,
                                                                entry.alignment, entry.decodedHash, chunk.storedBytes}});
                if (entry.codec == SaveChunkCodec::Raw)
                    return Result<SaveSceneCanonicalRecord>::Success({entry.record, std::span<const std::byte>{chunk.storedBytes}});
                auto selected = source.sourceReader->SelectChunk(entry.record);
                if (selected.HasError())
                    return Result<SaveSceneCanonicalRecord>::Failure(std::move(selected).ErrorValue());
                if (!selected.Value())
                    return Result<SaveSceneCanonicalRecord>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
                decoded.push_back(*std::move(selected).Value());
                return Result<SaveSceneCanonicalRecord>::Success({entry.record, std::span<const std::byte>{decoded.back()}});
            }

            /** @brief Reconstructs supported known bytes while opaque tuples borrow exact retained stored frames. */
            Result<CanonicalStateHash> Hash(const SaveArchiveHeader &header, const SaveContentDetail::AcceptedCaptureSeal &source) const {
                std::vector<std::vector<std::byte>> decoded;
                decoded.reserve(chunks.size());
                std::vector<std::vector<SaveSceneCanonicalRecord>> records;
                records.reserve(manifest.participants.size());
                std::vector<SaveSceneCanonicalParticipant> participants;
                participants.reserve(manifest.participants.size());
                for (const auto &owner : manifest.participants) {
                    records.emplace_back();
                    records.back().reserve(owner.chunks.size());
                    for (const auto record : owner.chunks) {
                        const auto chunk = std::ranges::lower_bound(chunks, record, {}, [](const PreservedSaveChunk &value) {
                            return value.entry.record;
                        });
                        const auto tag = std::ranges::lower_bound(layout, record, {}, &SaveSceneCanonicalLayoutEntry::record);
                        if (chunk == chunks.end() || chunk->entry.record != record || tag == layout.end() || tag->record != record)
                            return Result<CanonicalStateHash>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
                        auto selected = SelectRecord(*chunk, tag->representation, source, decoded);
                        if (selected.HasError())
                            return Result<CanonicalStateHash>::Failure(std::move(selected).ErrorValue());
                        records.back().push_back(std::move(selected).Value());
                    }
                    participants.emplace_back(owner.participant, owner.schemaVersion, records.back());
                }
                auto encoded = EncodeSaveSceneCanonicalState(header, participants, encodingLimit);
                if (encoded.HasError())
                    return Result<CanonicalStateHash>::Failure(encoded.ErrorValue());
                return Result<CanonicalStateHash>::Success(ComputeCanonicalStateHash(encoded.Value().Bytes()));
            }
        };

        /** @brief Extends compatibility only for the privately accepted capture's exact current owners and built-in layout. */
        Result<SaveCompatibilityPolicy> CandidatePreservationPolicy(const RuntimeSaveSnapshot &capture,
                                                                    const SaveContentDetail::AcceptedCaptureSeal &source) {
            auto preservation = *source.preservationPolicy;
            preservation.saveSchemaVersions.direct = {SaveSchemaVersion::Create(1).Value(), SaveSchemaVersion::Create(2).Value()};
            // The privately admitted snapshot qualifies only its exact captured owners/schemas. Source-only
            // opaque owners cannot enter this set: capture/retention overlap was rejected before callbacks.
            for (const auto &captured : capture.Participants()) {
                if (captured.disposition != CanonicalCaptureDisposition::Captured)
                    continue;
                const auto known =
                    std::ranges::find(preservation.participants, captured.participant, &SaveParticipantCompatibility::participant);
                if (known == preservation.participants.end())
                    preservation.participants.push_back(
                        {captured.participant, {{captured.schemaVersion, captured.schemaVersion}, {}}, false, {}});
                else if (!known->versions.direct.Contains(captured.schemaVersion))
                    return Result<SaveCompatibilityPolicy>::Failure(
                        MakeError(SaveErrors::MigrationSourceUnsupported, "Captured participant '" + captured.participant.Value() +
                                                                              "' needs an explicit supported schema policy."));
            }
            const auto schema = ParticipantSchemaVersion::Create(1).Value();
            std::erase_if(preservation.participants, [](const SaveParticipantCompatibility &owner) {
                return owner.participant == SaveSceneCanonicalLayoutParticipant();
            });
            preservation.participants.push_back({SaveSceneCanonicalLayoutParticipant(), {{schema, schema}, {}}, false, {}});
            std::ranges::sort(preservation.participants, {}, &SaveParticipantCompatibility::participant);
            return Result<SaveCompatibilityPolicy>::Success(std::move(preservation));
        }

        /** @brief Independently validates source/candidate retained bytes while keeping legacy source inspection strict. */
        Result<void> VerifyCandidateRetention(const RuntimeSaveSnapshot &capture, const SaveContentDetail::AcceptedCaptureSeal &source,
                                              const ValidatedSaveArchive &readback,
                                              const ValidatedSaveSceneCanonicalPreservation &destinationProof) {
            auto policy = CandidatePreservationPolicy(capture, source);
            if (policy.HasError())
                return Result<void>::Failure(policy.ErrorValue());
            if (source.sourceReader->Manifest().saveSchemaVersion.Value() != SaveSceneCanonicalSchemaVersion)
                return VerifyUnknownDataRoundTrip(*source.sourceReader, readback, policy.Value(), source.maximumPreservedBytes);
            auto sourceProof = ValidatedSaveSceneCanonicalPreservation::Create(*source.sourceReader);
            if (sourceProof.HasError())
                return Result<void>::Failure(sourceProof.ErrorValue());
            return sourceProof.Value().VerifyRoundTrip(destinationProof, policy.Value(), source.maximumPreservedBytes);
        }

    }  // namespace

    SaveContentSnapshot::SaveContentSnapshot(RuntimeSaveSnapshot capture,
                                             std::shared_ptr<const SaveContentDetail::AcceptedCaptureSeal> seal) noexcept
        : capture_(std::move(capture)), seal_(std::move(seal)) {}

    /** @copydoc SaveContentSnapshot::Diagnostics */
    std::span<const SaveContentDiagnostic> SaveContentSnapshot::Diagnostics() const noexcept {
        return seal_ ? std::span<const SaveContentDiagnostic>{*seal_->diagnostics} : std::span<const SaveContentDiagnostic>{};
    }

    /** @copydoc SaveContentSnapshot::Capture */
    const RuntimeSaveSnapshot &SaveContentSnapshot::Capture() const noexcept {
        return capture_;
    }

    /** @copydoc SaveContentSnapshot::ReSave */
    Result<FinalizedSaveArchive> SaveContentSnapshot::ReSave(const SaveArchiveHeader &metadata, const ArchiveFormatVersion version,
                                                             const SaveArchiveReaderLimits &limits) const {
        if (!seal_ || !capture_.IsValid() || capture_.Provenance() != seal_->provenance || metadata.project != seal_->project ||
            metadata.world != seal_->world || metadata.baseScene != seal_->baseScene)
            return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
        const auto &source = *seal_;
        try {
            const auto &header = metadata;
            ReSaveAssembly assembly{{SaveSchemaVersion::Create(SaveSceneCanonicalSchemaVersion).Value(), {}, {}},
                                    {},
                                    {},
                                    {},
                                    std::min<std::uint64_t>(64ULL << 20U, limits.maximumDecodedBytes),
                                    limits.maximumReadWorkBytes,
                                    std::min<std::uint64_t>(64ULL << 20U, limits.maximumReadWorkBytes / ReSaveAssembly::WorkPasses)};
            if (auto admittedMetadata = assembly.AdmitMetadata(header, capture_, source, limits); admittedMetadata.HasError())
                return Result<FinalizedSaveArchive>::Failure(admittedMetadata.ErrorValue());
            if (auto captured = assembly.AddCapture(capture_); captured.HasError())
                return Result<FinalizedSaveArchive>::Failure(captured.ErrorValue());
            if (auto retained = assembly.AddRetained(source); retained.HasError())
                return Result<FinalizedSaveArchive>::Failure(retained.ErrorValue());
            if (auto layout = assembly.AddLayout(); layout.HasError())
                return Result<FinalizedSaveArchive>::Failure(layout.ErrorValue());
            auto hash = assembly.Hash(header, source);
            if (hash.HasError())
                return Result<FinalizedSaveArchive>::Failure(hash.ErrorValue());
            assembly.manifest.canonicalState = hash.Value();
            auto finalized = SaveArchiveContainerWriter::Write(header, assembly.manifest, assembly.chunks, version, limits);
            if (finalized.HasError())
                return finalized;
            auto readback = SaveArchiveReader{limits}.Read(finalized.Value().Archive().bytes);
            if (readback.HasError())
                return Result<FinalizedSaveArchive>::Failure(readback.ErrorValue());
            auto destinationProof = ValidatedSaveSceneCanonicalPreservation::Create(readback.Value());
            if (destinationProof.HasError())
                return Result<FinalizedSaveArchive>::Failure(destinationProof.ErrorValue());
            if (auto preserved = VerifyCandidateRetention(capture_, source, readback.Value(), destinationProof.Value());
                preserved.HasError())
                return Result<FinalizedSaveArchive>::Failure(preserved.ErrorValue());
            return finalized;
        } catch (const std::bad_alloc &) {
            return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

}  // namespace Horo::Runtime
