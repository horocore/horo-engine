#include "Horo/Runtime/Save/SaveArchiveContainerWriter.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "RuntimeSceneErrors.h"
#include "SaveContentInternal.h"
#include "SaveContentRequirementsInternal.h"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>

namespace Horo::Runtime {
    namespace {
        /** @brief Reacquires actual Scene ownership and checks the same private source/install authority on the owner thread. */
        Result<RuntimeSceneView> CurrentWorld(const std::shared_ptr<SaveContentDetail::WorldState> &state) {
            if (!state || !state->content || state->content->installedOwner->ownerThread != std::this_thread::get_id())
                return Result<RuntimeSceneView>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            const auto &content = *state->content;
            if (content.cancellation.IsCancellationRequested())
                return Result<RuntimeSceneView>::Failure(MakeError(SaveErrors::OperationCancelled));
            if (content.installedOwner->closed || content.installedOwner->current != content.installed)
                return Result<RuntimeSceneView>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            for (const auto &module : content.installed->modules)
                if (!module.CanAdmit())
                    return Result<RuntimeSceneView>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            const auto view = state->service->ActiveScene();
            if (!view || view->RuntimeId() != state->boundScene || !view->IsCurrent())
                return Result<RuntimeSceneView>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            return Result<RuntimeSceneView>::Success(*view);
        }

        /** @brief Retains logical asset declarations and derives only actual installed native capture requirements. */
        Result<CanonicalEncodedValue> CapturedRequirements(const SaveContentDetail::WorldState &world,
                                                           const SaveParticipantRegistrySnapshot &participants) {
            try {
                auto requirements = world.content->requirements;
                if (requirements.empty() && world.content->legacy) {
                    requirements.push_back(
                        {SaveContentRequirementsParticipant(), SaveContentNecessity::Required,
                         SaveAssetContentRequirement{Assets::AssetId::FromBytes(world.descriptor.baseScene.Bytes()),
                                                     world.descriptor.expectedAssetType, world.descriptor.contentDigest}});
                }
                // Logical asset identities and expected source digests remain stable. The typed project policy
                // resolves admitted physical substitutions again on load; capture is not an asset-ID migration.
                for (const auto &module : world.content->installed->modules) {
                    const auto &native = module.Descriptor();
                    const auto *binding = participants.Find(native.participant.participant);
                    if (!binding || !HasSaveParticipantRole(binding->Descriptor().roles, SaveParticipantRole::Capture))
                        continue;
                    if (!module.CanAdmit() || binding->Adapter().get() != module.AcquireAdapter().get() ||
                        binding->Descriptor() != native.participant)
                        return Result<CanonicalEncodedValue>::Failure(MakeError(SaveErrors::RestoreActivationStale));
                    const SaveModuleContentRequirement declaration{native.moduleId, native.moduleVersion};
                    const auto existing = std::ranges::find_if(requirements, [&](const SaveContentRequirement &entry) {
                        return entry.owner == native.participant.participant &&
                               std::holds_alternative<SaveModuleContentRequirement>(entry.content);
                    });
                    if (existing != requirements.end()) {
                        if (std::get<SaveModuleContentRequirement>(existing->content) != declaration)
                            return Result<CanonicalEncodedValue>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                        existing->necessity = native.participant.required ? SaveContentNecessity::Required : SaveContentNecessity::Optional;
                    } else {
                        requirements.push_back(
                            {native.participant.participant,
                             native.participant.required ? SaveContentNecessity::Required : SaveContentNecessity::Optional, declaration});
                    }
                }
                if (auto sorted = SaveContentDetail::SortRequirements(requirements); sorted.HasError())
                    return Result<CanonicalEncodedValue>::Failure(sorted.ErrorValue());
                return EncodeSaveContentRequirements(requirements);
            } catch (const std::bad_alloc &) {
                return Result<CanonicalEncodedValue>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
            }
        }

        /** @brief Required owner-bound declaration; generic unadmitted capture cannot bypass degraded-world project policy. */
        class RequirementsAdapter final : public ICanonicalStateAdapter {
        public:
            explicit RequirementsAdapter(std::shared_ptr<SaveContentDetail::WorldState> world) : world_(std::move(world)) {}

            Result<CanonicalCaptureDisposition> Capture(const CanonicalCaptureContext &context,
                                                        ICanonicalCaptureSink &sink) const override {
                if (!world_->captureAdmission || !world_->captureRequirements || context.provenance != *world_->captureAdmission ||
                    context.participant != SaveContentRequirementsParticipant() || context.schemaVersion.Value() != 1)
                    return Result<CanonicalCaptureDisposition>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
                if (auto current = CurrentWorld(world_); current.HasError())
                    return Result<CanonicalCaptureDisposition>::Failure(current.ErrorValue());
                auto written = sink.WriteCopied(SaveContentRequirementsRecord(), world_->captureRequirements->Bytes());
                if (written.HasError())
                    return Result<CanonicalCaptureDisposition>::Failure(written.ErrorValue());
                return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
            }

        private:
            std::shared_ptr<SaveContentDetail::WorldState> world_;
        };

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
                        manifest.participants.push_back(
                            {participant.participant, participant.schemaVersion, participant.required, participant.records});
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
                    chunks.push_back(
                        {{identity.record, identity.participant, 0, bytes.size(), bytes.size(), 1, SaveChunkCodec::Raw, digest},
                         std::move(bytes)});
                    layout.push_back({identity.record, SaveSceneCanonicalRepresentation::Known});
                }
                return Result<void>::Success();
            }

            /** @brief Carries exact source frames and their authenticated release-independent representation. */
            Result<void> AddRetained(const SaveContentDetail::AcceptedCaptureSeal &source) {
                for (const auto &chunk : source.opaque->preserved) {
                    const auto tag =
                        std::ranges::lower_bound(*source.canonicalLayout, chunk.entry.record, {}, &SaveSceneCanonicalLayoutEntry::record);
                    if (!source.canonicalLayout->empty() && (tag == source.canonicalLayout->end() || tag->record != chunk.entry.record))
                        return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                    const auto representation =
                        source.canonicalLayout->empty() ? SaveSceneCanonicalRepresentation::Opaque : tag->representation;
                    const auto retainedBytes = representation == SaveSceneCanonicalRepresentation::Known
                                                   ? std::max(chunk.entry.storedByteLength, chunk.entry.decodedByteLength)
                                                   : chunk.entry.storedByteLength;
                    if (retainedBytes > remainingBytes)
                        return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecLimitExceeded));
                    const auto selectionWork =
                        representation == SaveSceneCanonicalRepresentation::Known
                            ? chunk.entry.storedByteLength + (chunk.entry.codec == SaveChunkCodec::Raw ? 0 : chunk.entry.decodedByteLength)
                            : 0;
                    if (selectionWork < chunk.entry.storedByteLength && representation == SaveSceneCanonicalRepresentation::Known)
                        return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
                    if (auto work = AdmitWork(retainedBytes, selectionWork); work.HasError())
                        return work;
                    remainingBytes -= retainedBytes;
                    layout.push_back({chunk.entry.record, representation});
                    chunks.push_back(chunk);
                    const auto owner = std::ranges::find(manifest.participants, chunk.entry.owner, &SaveManifestParticipant::participant);
                    if (owner != manifest.participants.end()) {
                        if (std::ranges::find(retainedOwners, owner->participant) != retainedOwners.end()) {
                            owner->chunks.push_back(chunk.entry.record);
                            continue;
                        }
                        return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                    }
                    const auto &sourceParticipants = source.sourceReader->Manifest().participants;
                    const auto original = std::ranges::find(sourceParticipants, chunk.entry.owner, &SaveManifestParticipant::participant);
                    if (original == sourceParticipants.end() || original->required)
                        return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                    retainedOwners.push_back(original->participant);
                    manifest.participants.push_back({original->participant, original->schemaVersion, false, {chunk.entry.record}});
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
                layout.push_back({SaveSceneCanonicalLayoutRecord(), SaveSceneCanonicalRepresentation::Known});
                std::ranges::sort(layout, {}, &SaveSceneCanonicalLayoutEntry::record);
                auto encoded = EncodeSaveSceneCanonicalLayout(layout);
                if (encoded.HasError())
                    return Result<void>::Failure(encoded.ErrorValue());
                if (encoded.Value().Bytes().size() != layoutBytes)
                    return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
                std::vector<std::byte> bytes(encoded.Value().Bytes().begin(), encoded.Value().Bytes().end());
                const auto digest = ComputeSha256(bytes);
                chunks.push_back({{SaveSceneCanonicalLayoutRecord(), SaveSceneCanonicalLayoutParticipant(), 0, bytes.size(), bytes.size(),
                                   1, SaveChunkCodec::Raw, digest},
                                  std::move(bytes)});
                manifest.participants.push_back({SaveSceneCanonicalLayoutParticipant(),
                                                 ParticipantSchemaVersion::Create(1).Value(),
                                                 true,
                                                 {SaveSceneCanonicalLayoutRecord()}});
                std::ranges::sort(manifest.participants, {}, &SaveManifestParticipant::participant);
                for (auto &owner : manifest.participants)
                    std::ranges::sort(owner.chunks);
                std::ranges::sort(chunks, {}, [](const PreservedSaveChunk &chunk) {
                    return chunk.entry.record;
                });
                return Result<void>::Success();
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
                        if (tag->representation == SaveSceneCanonicalRepresentation::Opaque) {
                            const auto &entry = chunk->entry;
                            records.back().push_back(
                                {record, SaveSceneCanonicalOpaque{entry.codec, entry.storedByteLength, entry.decodedByteLength,
                                                                  entry.alignment, entry.decodedHash, chunk->storedBytes}});
                        } else if (chunk->entry.codec == SaveChunkCodec::Raw) {
                            records.back().push_back({record, std::span<const std::byte>{chunk->storedBytes}});
                        } else {
                            auto selected = source.sourceReader->SelectChunk(record);
                            if (selected.HasError())
                                return Result<CanonicalStateHash>::Failure(selected.ErrorValue());
                            if (!selected.Value())
                                return Result<CanonicalStateHash>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
                            decoded.push_back(std::move(*selected.Value()));
                            records.back().push_back({record, std::span<const std::byte>{decoded.back()}});
                        }
                    }
                    participants.push_back({owner.participant, owner.schemaVersion, records.back()});
                }
                auto encoded = EncodeSaveSceneCanonicalState(header, participants, encodingLimit);
                if (encoded.HasError())
                    return Result<CanonicalStateHash>::Failure(encoded.ErrorValue());
                return Result<CanonicalStateHash>::Success(ComputeCanonicalStateHash(encoded.Value().Bytes()));
            }
        };

        /** @brief Closes owner callback admission on every return, including foreign callback failures handled by the barrier. */
        struct CaptureAdmission final {
            SaveContentDetail::WorldState &world;

            ~CaptureAdmission() {
                world.captureAdmission.reset();
                world.captureRequirements.reset();
            }
        };

        /** @brief Rejects retention/capture ownership overlap and binds the actual registered declaration adapter. */
        Result<void> ValidateCaptureRegistry(const SaveContentDetail::WorldState &world,
                                             const SaveParticipantRegistrySnapshot &participants) {
            for (const auto &binding : participants.Bindings())
                if (binding.Descriptor().participant == SaveSceneCanonicalLayoutParticipant())
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                else if (binding.Descriptor().scope == SaveParticipantScope::PersistentWorld)
                    return Result<void>::Failure(MakeError(SceneErrors::SaveBootstrapDatasetUnsupported));
            const auto declaration = world.declarationAdapter.lock();
            const auto *binding = participants.Find(SaveContentRequirementsParticipant());
            if (!declaration || !binding || binding->Adapter().get() != declaration.get())
                return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            for (const auto &entry : participants.CaptureBindings()) {
                const auto owner = entry.Descriptor().participant;
                if (std::ranges::binary_search(world.content->quarantinedOwners, owner) ||
                    std::ranges::any_of(world.content->opaque.preserved, [&](const PreservedSaveChunk &chunk) {
                    return chunk.entry.owner == owner;
                }))
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            }
            const auto admittedOwner = [&](const SaveParticipantId &owner) {
                if (std::ranges::any_of(world.content->opaque.preserved, [&](const PreservedSaveChunk &chunk) {
                    return chunk.entry.owner == owner;
                }))
                    return true;
                const auto *candidate = participants.Find(owner);
                return candidate && HasSaveParticipantRole(candidate->Descriptor().roles, SaveParticipantRole::Capture);
            };
            for (const auto &requirement : world.content->requirements)
                if (!admittedOwner(requirement.owner))
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
            for (const auto &owner : world.content->sourceReader.Manifest().participants)
                if (owner.required && owner.participant != SaveSceneCanonicalLayoutParticipant() && !admittedOwner(owner.participant))
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
            return Result<void>::Success();
        }

        /** @brief Prevents a skipped declaration owner from producing a manifest that cannot be reconciled again. */
        Result<void> ValidateCapturedOwners(const SaveContentDetail::WorldState &world, const RuntimeSaveSnapshot &capture) {
            const auto retainedOrCaptured = [&](const SaveParticipantId &owner) {
                if (std::ranges::any_of(world.content->opaque.preserved, [&](const PreservedSaveChunk &chunk) {
                    return chunk.entry.owner == owner;
                }))
                    return true;
                return std::ranges::any_of(capture.Participants(), [&](const auto &participant) {
                    return participant.participant == owner && participant.disposition == CanonicalCaptureDisposition::Captured;
                });
            };
            for (const auto &requirement : world.content->requirements)
                if (!retainedOrCaptured(requirement.owner))
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
            for (const auto &owner : world.content->sourceReader.Manifest().participants)
                if (owner.required && owner.participant != SaveSceneCanonicalLayoutParticipant() && !retainedOrCaptured(owner.participant))
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
            for (const auto &module : world.content->installed->modules) {
                const auto owner = module.Descriptor().participant.participant;
                if (std::ranges::any_of(capture.Participants(), [&](const auto &participant) {
                    return participant.participant == owner && participant.disposition != CanonicalCaptureDisposition::Captured;
                }))
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantIncomplete));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc SaveContentSnapshot::SaveContentSnapshot */
    SaveContentSnapshot::SaveContentSnapshot(RuntimeSaveSnapshot capture,
                                             std::shared_ptr<const SaveContentDetail::AcceptedCaptureSeal> seal) noexcept
        : capture_(std::move(capture)), seal_(std::move(seal)) {}

    /** @copydoc SaveContentSnapshot::Diagnostics */
    std::span<const SaveContentDiagnostic> SaveContentSnapshot::Diagnostics() const noexcept {
        return seal_ ? std::span<const SaveContentDiagnostic>{*seal_->diagnostics} : std::span<const SaveContentDiagnostic>{};
    }

    /** @copydoc SaveContentWorld::Diagnostics */
    std::span<const SaveContentDiagnostic> SaveContentWorld::Diagnostics() const noexcept {
        return state_ ? std::span<const SaveContentDiagnostic>{state_->content->diagnostics} : std::span<const SaveContentDiagnostic>{};
    }

    /** @copydoc SaveContentSnapshot::Capture */
    const RuntimeSaveSnapshot &SaveContentSnapshot::Capture() const noexcept {
        return capture_;
    }

    /** @copydoc SaveContentSnapshot::ReSave */
    Result<FinalizedSaveArchive> SaveContentSnapshot::ReSave(SaveArchiveHeader header, const ArchiveFormatVersion version,
                                                             const SaveArchiveReaderLimits &limits) const {
        if (!seal_ || !capture_.IsValid() || capture_.Provenance() != seal_->provenance || header.project != seal_->project ||
            header.world != seal_->world || header.baseScene != seal_->baseScene)
            return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
        const auto &source = *seal_;
        try {
            ReSaveAssembly assembly{{SaveSchemaVersion::Create(SaveSceneCanonicalSchemaVersion).Value(), {}, {}},
                                    {},
                                    {},
                                    {},
                                    std::min<std::uint64_t>(64ULL << 20U, limits.maximumDecodedBytes),
                                    limits.maximumReadWorkBytes,
                                    std::min<std::uint64_t>(64ULL << 20U, limits.maximumReadWorkBytes / ReSaveAssembly::WorkPasses)};
            if (auto metadata = assembly.AdmitMetadata(header, capture_, source, limits); metadata.HasError())
                return Result<FinalizedSaveArchive>::Failure(metadata.ErrorValue());
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
            auto preservation = *source.preservationPolicy;
            preservation.saveSchemaVersions.direct = {SaveSchemaVersion::Create(1).Value(), SaveSchemaVersion::Create(2).Value()};
            // The privately admitted snapshot qualifies only its exact captured owners/schemas. Source-only
            // opaque owners cannot enter this set: capture/retention overlap was rejected before callbacks.
            for (const auto &captured : capture_.Participants()) {
                if (captured.disposition != CanonicalCaptureDisposition::Captured)
                    continue;
                const auto known =
                    std::ranges::find(preservation.participants, captured.participant, &SaveParticipantCompatibility::participant);
                if (known == preservation.participants.end())
                    preservation.participants.push_back(
                        {captured.participant, {{captured.schemaVersion, captured.schemaVersion}, {}}, false, {}});
                else if (!known->versions.direct.Contains(captured.schemaVersion))
                    return Result<FinalizedSaveArchive>::Failure(
                        MakeError(SaveErrors::MigrationSourceUnsupported, "Captured participant '" + captured.participant.Value() +
                                                                              "' needs an explicit supported schema policy."));
            }
            const auto schema = ParticipantSchemaVersion::Create(1).Value();
            std::erase_if(preservation.participants, [](const SaveParticipantCompatibility &owner) {
                return owner.participant == SaveSceneCanonicalLayoutParticipant();
            });
            preservation.participants.push_back({SaveSceneCanonicalLayoutParticipant(), {{schema, schema}, {}}, false, {}});
            std::ranges::sort(preservation.participants, {}, &SaveParticipantCompatibility::participant);
            auto preserved = [&]() {
                if (source.sourceReader->Manifest().saveSchemaVersion.Value() != SaveSceneCanonicalSchemaVersion)
                    return VerifyUnknownDataRoundTrip(*source.sourceReader, readback.Value(), preservation, source.maximumPreservedBytes);
                auto sourceProof = ValidatedSaveSceneCanonicalPreservation::Create(*source.sourceReader);
                if (sourceProof.HasError())
                    return Result<void>::Failure(sourceProof.ErrorValue());
                return sourceProof.Value().VerifyRoundTrip(destinationProof.Value(), preservation, source.maximumPreservedBytes);
            }();
            if (preserved.HasError())
                return Result<FinalizedSaveArchive>::Failure(preserved.ErrorValue());
            return finalized;
        } catch (const std::bad_alloc &) {
            return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

    /** @copydoc SaveContentWorld::RegisterRequirements */
    Result<SaveParticipantRegistration> SaveContentWorld::RegisterRequirements(CanonicalStateParticipantRegistry &registry) const {
        if (auto current = CurrentWorld(state_); current.HasError())
            return Result<SaveParticipantRegistration>::Failure(current.ErrorValue());
        if (!state_->declarationAdapter.expired())
            return Result<SaveParticipantRegistration>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
        try {
            auto adapter = std::make_shared<RequirementsAdapter>(state_);
            const CanonicalStateParticipantDescriptor descriptor{.participant = SaveContentRequirementsParticipant(),
                                                                 .schemaVersion = ParticipantSchemaVersion::Create(1).Value(),
                                                                 .scope = SaveParticipantScope::RuntimeScene,
                                                                 .roles = SaveParticipantRole::Capture,
                                                                 .required = true,
                                                                 .limits = {1U << 20U, 1, 4},
                                                                 .dependencies = {},
                                                                 .ownedRecords = {SaveContentRequirementsRecord()}};
            auto registered = registry.Register(descriptor, adapter);
            if (registered.HasValue())
                state_->declarationAdapter = std::move(adapter);
            return registered;
        } catch (const std::bad_alloc &) {
            return Result<SaveParticipantRegistration>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

    /** @copydoc SaveContentWorld::CaptureAtSafePoint */
    Result<SaveContentCaptureOutcome> SaveContentWorld::CaptureAtSafePoint(SaveCaptureBarrier &barrier, const RuntimePhase phase,
                                                                           const SaveRuntimeGeneration generation,
                                                                           CapturedStateId capturedState, const CanonicalCaptureEpoch epoch,
                                                                           SaveParticipantRegistrySnapshot participants,
                                                                           const SaveDegradedWorldPolicy policy,
                                                                           const RuntimeSaveCaptureLimits &limits) const {
        static_assert(std::is_nothrow_move_constructible_v<Result<SaveContentCaptureOutcome>>);
        // Error storage is prepared before admission mutates any world/barrier state. Unwinding only moves owned storage.
        auto allocationFailure = Result<SaveContentCaptureOutcome>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        try {
            auto view = CurrentWorld(state_);
            if (view.HasError())
                return Result<SaveContentCaptureOutcome>::Failure(view.ErrorValue());
            if (state_->captureAdmission ||
                (policy != SaveDegradedWorldPolicy::Reject && policy != SaveDegradedWorldPolicy::PreserveOpaque) ||
                (state_->content->degraded && policy != SaveDegradedWorldPolicy::PreserveOpaque))
                return Result<SaveContentCaptureOutcome>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
            if (generation.scene != view.Value().RuntimeId().value || generation.registry != participants.Generation())
                return Result<SaveContentCaptureOutcome>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            if (auto registry = ValidateCaptureRegistry(*state_, participants); registry.HasError())
                return Result<SaveContentCaptureOutcome>::Failure(registry.ErrorValue());
            const RuntimeSaveCaptureProvenance provenance{capturedState, epoch, generation.scene, view.Value().StructuralRevision(),
                                                          generation.registry};
            auto requirements = CapturedRequirements(*state_, participants);
            if (requirements.HasError())
                return Result<SaveContentCaptureOutcome>::Failure(requirements.ErrorValue());
            // Allocate the immutable admission seal before invoking any capture callback or consuming barrier readiness.
            auto publication = state_->publication->Snapshot();
            if (publication.HasError())
                return Result<SaveContentCaptureOutcome>::Failure(publication.ErrorValue());
            if (publication.Value().status != ScenePublicationStatus::Published || publication.Value().scene != state_->boundScene ||
                publication.Value().datasets != SceneCanonicalDatasetProjection::Absent)
                return Result<SaveContentCaptureOutcome>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            std::shared_ptr<const SaveContentDetail::AcceptedCaptureSeal> seal;
            seal = std::make_shared<const SaveContentDetail::AcceptedCaptureSeal>(state_->content, provenance, state_->descriptor.world,
                                                                                  state_->descriptor.baseScene);
            state_->captureRequirements = std::move(requirements).Value();
            state_->captureAdmission = provenance;
            const CaptureAdmission admission{*state_};
            auto captured = barrier.CaptureAtSafePoint(phase, generation, provenance, std::move(participants), limits);
            if (captured.HasError())
                return Result<SaveContentCaptureOutcome>::Failure(captured.ErrorValue());
            auto outcome = std::move(captured).Value();
            SaveContentCaptureOutcome result{outcome.barrier, {}, std::move(outcome.error)};
            if (outcome.capture) {
                if (auto owners = ValidateCapturedOwners(*state_, *outcome.capture); owners.HasError())
                    result.error = owners.ErrorValue();
                else
                    result.capture = SaveContentSnapshot{std::move(*outcome.capture), std::move(seal)};
            }
            return Result<SaveContentCaptureOutcome>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return std::move(allocationFailure);
        }
    }
}  // namespace Horo::Runtime
