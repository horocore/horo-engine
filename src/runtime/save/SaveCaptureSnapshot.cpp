#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <tuple>
#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Evaluates a fixed set of independent invariants without embedding control flow in the caller. */
        template <std::size_t Count> [[nodiscard]] constexpr bool AllTrue(const std::array<bool, Count> &conditions) noexcept {
            return std::ranges::all_of(conditions, std::identity{});
        }

        /** @brief Core-owned immutable payload used for bounded eager copies. */
        class CopiedCanonicalPayload final : public IImmutableCanonicalPayload {
        public:
            explicit CopiedCanonicalPayload(const std::span<const std::byte> bytes) : bytes_(bytes.begin(), bytes.end()) {}

            [[nodiscard]] std::uint64_t ByteLength() const noexcept override {
                return static_cast<std::uint64_t>(bytes_.size());
            }

            [[nodiscard]] std::size_t SegmentCount() const noexcept override {
                return 1;
            }

            [[nodiscard]] std::span<const std::byte> Segment(const std::size_t index) const noexcept override {
                return index == 0 ? std::span<const std::byte>{bytes_} : std::span<const std::byte>{};
            }

        private:
            std::vector<std::byte> bytes_;
        };

        /** @brief Reports whether operation capture bounds are finite and within qualified hard ceilings. */
        [[nodiscard]] bool HasValidCaptureLimits(const RuntimeSaveCaptureLimits &limits) noexcept {
            return AllTrue(std::array{limits.maximumParticipants != 0, limits.maximumParticipants <= MaximumSaveParticipantCount,
                                      limits.maximumRecords != 0, limits.maximumRecords <= MaximumRuntimeSaveCaptureRecords,
                                      limits.maximumSegments != 0, limits.maximumSegments <= MaximumRuntimeSaveCaptureSegments,
                                      limits.maximumPayloadBytes != 0, limits.maximumPayloadBytes <= MaximumRuntimeSaveCapturePayloadBytes,
                                      limits.maximumCopiedRecordBytes != 0,
                                      limits.maximumCopiedRecordBytes <= MaximumRuntimeSaveCapturePayloadBytes});
        }

        /** @brief Reports whether stable capture evidence identifies one exact safe-point observation. */
        [[nodiscard]] bool HasValidCaptureProvenance(const RuntimeSaveCaptureProvenance &provenance) noexcept {
            return AllTrue(std::array{provenance.capturedState.IsValid(), provenance.epoch.IsValid(), provenance.sceneIncarnation != 0,
                                      provenance.sceneRevision != 0, provenance.registryGeneration != 0});
        }

        /** @brief Establishes deterministic whole-snapshot canonical record order. */
        [[nodiscard]] bool RecordLess(const OwnedCanonicalSnapshot &left, const OwnedCanonicalSnapshot &right) noexcept {
            return std::tie(left.Record().participant, left.Record().record) < std::tie(right.Record().participant, right.Record().record);
        }

        /** @brief Validates immutable lease shape and checked segment accounting. */
        [[nodiscard]] Result<std::pair<std::uint64_t, std::size_t>> InspectPayload(
            const std::shared_ptr<const IImmutableCanonicalPayload> &payload, const std::size_t maximumSegments,
            const std::uint64_t maximumBytes) {
            if (payload == nullptr)
                return Result<std::pair<std::uint64_t, std::size_t>>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
            const std::size_t segments = payload->SegmentCount();
            const std::uint64_t declaredBytes = payload->ByteLength();
            if (segments == 0 || segments > maximumSegments || declaredBytes > maximumBytes)
                return Result<std::pair<std::uint64_t, std::size_t>>::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));

            std::uint64_t inspectedBytes = 0;
            for (std::size_t index = 0; index < segments; ++index) {
                const std::span<const std::byte> segment = payload->Segment(index);
                if ((segment.data() == nullptr && !segment.empty()) || segment.size() > declaredBytes - inspectedBytes)
                    return Result<std::pair<std::uint64_t, std::size_t>>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
                inspectedBytes += static_cast<std::uint64_t>(segment.size());
            }
            if (inspectedBytes != declaredBytes)
                return Result<std::pair<std::uint64_t, std::size_t>>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
            return Result<std::pair<std::uint64_t, std::size_t>>::Success({declaredBytes, segments});
        }

        /** @brief Scopes adapter writes to the exact participant and records ignored sink failures. */
        class ParticipantCaptureSink final : public ICanonicalCaptureSink {
        public:
            ParticipantCaptureSink(RuntimeSaveCaptureBuilder &builder, const CanonicalStateParticipantDescriptor &descriptor) noexcept
                : builder_(builder), descriptor_(descriptor) {}

            [[nodiscard]] Result<void> WriteCopied(const SaveRecordId record, const std::span<const std::byte> bytes) override {
                Result<void> result = builder_.AddRecord(MakeRecord(record), bytes);
                rejected_ = rejected_ || result.HasError();
                return result;
            }

            [[nodiscard]] Result<void> WriteImmutable(const SaveRecordId record,
                                                      std::shared_ptr<const IImmutableCanonicalPayload> payload) override {
                Result<void> result = builder_.AddImmutableRecord(MakeRecord(record), std::move(payload));
                rejected_ = rejected_ || result.HasError();
                return result;
            }

            [[nodiscard]] bool RejectedWrite() const noexcept {
                return rejected_;
            }

        private:
            [[nodiscard]] CanonicalCaptureRecord MakeRecord(SaveRecordId record) const {
                return {.participant = descriptor_.participant, .schemaVersion = descriptor_.schemaVersion, .record = std::move(record)};
            }

            RuntimeSaveCaptureBuilder &builder_;
            const CanonicalStateParticipantDescriptor &descriptor_;
            bool rejected_{};
        };
    }  // namespace

    OwnedCanonicalSnapshot::OwnedCanonicalSnapshot(CanonicalCaptureRecord record,
                                                   std::shared_ptr<const ICanonicalStateAdapter> adapterLease,
                                                   std::shared_ptr<const IImmutableCanonicalPayload> payload,
                                                   const std::uint64_t byteLength, const std::size_t segmentCount) noexcept
        : record_(std::move(record)), adapterLease_(std::move(adapterLease)), payload_(std::move(payload)), byteLength_(byteLength),
          segmentCount_(segmentCount) {}

    /** @copydoc OwnedCanonicalSnapshot::Record */
    const CanonicalCaptureRecord &OwnedCanonicalSnapshot::Record() const noexcept {
        return record_;
    }

    /** @copydoc OwnedCanonicalSnapshot::SegmentCount */
    std::size_t OwnedCanonicalSnapshot::SegmentCount() const noexcept {
        return segmentCount_;
    }

    /** @copydoc OwnedCanonicalSnapshot::Segment */
    std::span<const std::byte> OwnedCanonicalSnapshot::Segment(const std::size_t index) const noexcept {
        return payload_ == nullptr || index >= segmentCount_ ? std::span<const std::byte>{} : payload_->Segment(index);
    }

    /** @copydoc OwnedCanonicalSnapshot::ByteLength */
    std::uint64_t OwnedCanonicalSnapshot::ByteLength() const noexcept {
        return byteLength_;
    }

    RuntimeSaveSnapshot::RuntimeSaveSnapshot(RuntimeSaveCaptureProvenance provenance, const std::uint64_t payloadByteLength,
                                             SaveParticipantRegistrySnapshot participants,
                                             std::shared_ptr<const std::vector<CanonicalCaptureParticipantProjection>> projection,
                                             std::shared_ptr<const std::vector<OwnedCanonicalSnapshot>> records) noexcept
        : provenance_(std::move(provenance)), payloadByteLength_(payloadByteLength), participants_(std::move(participants)),
          projection_(std::move(projection)), records_(std::move(records)) {}

    /** @copydoc RuntimeSaveSnapshot::IsValid */
    bool RuntimeSaveSnapshot::IsValid() const noexcept {
        return AllTrue(std::array{records_ != nullptr, projection_ != nullptr, participants_.IsValid(),
                                  HasValidCaptureProvenance(provenance_), provenance_.registryGeneration == participants_.Generation()});
    }

    /** @copydoc RuntimeSaveSnapshot::Provenance */
    const RuntimeSaveCaptureProvenance &RuntimeSaveSnapshot::Provenance() const noexcept {
        return provenance_;
    }

    /** @copydoc RuntimeSaveSnapshot::Records */
    std::span<const OwnedCanonicalSnapshot> RuntimeSaveSnapshot::Records() const noexcept {
        return records_ == nullptr ? std::span<const OwnedCanonicalSnapshot>{} : std::span<const OwnedCanonicalSnapshot>{*records_};
    }

    /** @copydoc RuntimeSaveSnapshot::Participants */
    std::span<const CanonicalCaptureParticipantProjection> RuntimeSaveSnapshot::Participants() const noexcept {
        return projection_ == nullptr ? std::span<const CanonicalCaptureParticipantProjection>{}
                                      : std::span<const CanonicalCaptureParticipantProjection>{*projection_};
    }

    /** @copydoc RuntimeSaveSnapshot::PayloadByteLength */
    std::uint64_t RuntimeSaveSnapshot::PayloadByteLength() const noexcept {
        return payloadByteLength_;
    }

    RuntimeSaveCaptureBuilder::RuntimeSaveCaptureBuilder(RuntimeSaveCaptureProvenance provenance,
                                                         SaveParticipantRegistrySnapshot participants,
                                                         const RuntimeSaveCaptureLimits &limits, std::vector<ParticipantUsage> usage,
                                                         RecordAdmissions recordAdmissions) noexcept
        : provenance_(std::move(provenance)), participants_(std::move(participants)), limits_(limits), usage_(std::move(usage)),
          recordAdmissions_(std::move(recordAdmissions)) {}

    RuntimeSaveCaptureBuilder::RuntimeSaveCaptureBuilder(RuntimeSaveCaptureBuilder &&other) noexcept {
        MoveFrom(std::move(other));
    }

    RuntimeSaveCaptureBuilder &RuntimeSaveCaptureBuilder::operator=(RuntimeSaveCaptureBuilder &&other) noexcept {
        if (this != &other) {
            MarkSpent();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    /** @copydoc RuntimeSaveCaptureBuilder::Create */
    Result<RuntimeSaveCaptureBuilder> RuntimeSaveCaptureBuilder::Create(RuntimeSaveCaptureProvenance provenance,
                                                                        SaveParticipantRegistrySnapshot participants,
                                                                        const RuntimeSaveCaptureLimits &limits) {
        if (const Result<void> valid = ValidateCreationContext(provenance, participants, limits); valid.HasError())
            return Result<RuntimeSaveCaptureBuilder>::Failure(valid.ErrorValue());

        try {
            auto admission = BuildAdmissionState(participants);
            if (admission.HasError())
                return Result<RuntimeSaveCaptureBuilder>::Failure(admission.ErrorValue());
            std::vector<OwnedCanonicalSnapshot> records;
            records.reserve(limits.maximumRecords);
            AdmissionState state = std::move(admission).Value();
            RuntimeSaveCaptureBuilder builder{std::move(provenance), std::move(participants), limits, std::move(state.usage),
                                              std::move(state.records)};
            builder.records_ = std::move(records);
            return Result<RuntimeSaveCaptureBuilder>::Success(std::move(builder));
        } catch (const std::bad_alloc &) {
            return Result<RuntimeSaveCaptureBuilder>::Failure(MakeError(SaveErrors::CaptureAllocationFailed));
        }
    }

    Result<void> RuntimeSaveCaptureBuilder::ValidateCreationContext(const RuntimeSaveCaptureProvenance &provenance,
                                                                    const SaveParticipantRegistrySnapshot &participants,
                                                                    const RuntimeSaveCaptureLimits &limits) {
        if (!HasValidCaptureProvenance(provenance) || !HasValidCaptureLimits(limits) || !participants.IsValid())
            return Result<void>::Failure(MakeError(SaveErrors::CaptureContextInvalid));
        if (participants.Bindings().size() > limits.maximumParticipants)
            return Result<void>::Failure(MakeError(SaveErrors::CaptureContextInvalid));
        if (provenance.registryGeneration != participants.Generation())
            return Result<void>::Failure(MakeError(SaveErrors::CaptureRegistryStale));
        return Result<void>::Success();
    }

    Result<RuntimeSaveCaptureBuilder::AdmissionState> RuntimeSaveCaptureBuilder::BuildAdmissionState(
        const SaveParticipantRegistrySnapshot &participants) {
        AdmissionState state;
        state.usage.reserve(participants.Bindings().size());
        std::size_t totalOwnedRecords = 0;
        for (const SaveParticipantBinding &binding : participants.Bindings()) {
            if (HasSaveParticipantRole(binding.Descriptor().roles, SaveParticipantRole::Capture)) {
                if (binding.Descriptor().ownedRecords.size() > MaximumRuntimeSaveCaptureRecords - totalOwnedRecords)
                    return Result<AdmissionState>::Failure(MakeError(SaveErrors::CaptureContextInvalid));
                totalOwnedRecords += binding.Descriptor().ownedRecords.size();
            }
            state.usage.push_back({.participant = binding.Descriptor().participant});
        }

        state.records.reserve(totalOwnedRecords);
        std::size_t participantIndex = 0;
        for (const SaveParticipantBinding &binding : participants.Bindings()) {
            if (HasSaveParticipantRole(binding.Descriptor().roles, SaveParticipantRole::Capture)) {
                for (const SaveRecordId &record : binding.Descriptor().ownedRecords)
                    state.records.try_emplace(record, RecordAdmission{.participantIndex = participantIndex});
            }
            ++participantIndex;
        }
        return Result<AdmissionState>::Success(std::move(state));
    }

    /** @copydoc RuntimeSaveCaptureBuilder::CaptureParticipants */
    Result<void> RuntimeSaveCaptureBuilder::CaptureParticipants() {
        if (sealed_)
            return Result<void>::Failure(MakeError(SaveErrors::CaptureAlreadySealed));
        if (captureAttempted_)
            return Result<void>::Failure(MakeError(SaveErrors::CaptureAdapterContractInvalid));

        const std::size_t initialRecordCount = records_.size();
        const std::uint64_t initialPayloadBytes = payloadByteLength_;
        const std::size_t initialSegmentCount = segmentCount_;
        std::vector<ParticipantUsage> usageCheckpoint;

        try {
            usageCheckpoint = usage_;
            for (const SaveParticipantBinding &binding : participants_.CaptureBindings()) {
                const Result<void> captured = CaptureBinding(binding);
                if (captured.HasError()) {
                    Error error = captured.ErrorValue();
                    RollbackCapture(initialRecordCount, initialPayloadBytes, initialSegmentCount, std::move(usageCheckpoint));
                    return Result<void>::Failure(std::move(error));
                }
            }
            captureAttempted_ = true;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            RollbackCapture(initialRecordCount, initialPayloadBytes, initialSegmentCount, std::move(usageCheckpoint));
            return Result<void>::Failure(MakeError(SaveErrors::CaptureAllocationFailed));
        } catch (...) {  // NOSONAR -- adapter boundaries must normalize non-standard exceptions into the typed contract error.
            RollbackCapture(initialRecordCount, initialPayloadBytes, initialSegmentCount, std::move(usageCheckpoint));
            return Result<void>::Failure(MakeError(SaveErrors::CaptureAdapterContractInvalid));
        }
    }

    /** @copydoc RuntimeSaveCaptureBuilder::AddRecord */
    Result<void> RuntimeSaveCaptureBuilder::AddRecord(const CanonicalCaptureRecord &record, const std::span<const std::byte> bytes) {
        if (sealed_)
            return Result<void>::Failure(MakeError(SaveErrors::CaptureAlreadySealed));
        if (bytes.size() > limits_.maximumCopiedRecordBytes)
            return Result<void>::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
        if (const Result<void> admitted = ValidateAdmission(record, static_cast<std::uint64_t>(bytes.size()), 1); admitted.HasError())
            return admitted;
        try {
            CanonicalCaptureRecord ownedRecord = record;
            return AdmitPayload(std::move(ownedRecord), std::make_shared<CopiedCanonicalPayload>(bytes),
                                static_cast<std::uint64_t>(bytes.size()), 1);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(SaveErrors::CaptureAllocationFailed));
        }
    }

    /** @copydoc RuntimeSaveCaptureBuilder::AddImmutableRecord */
    Result<void> RuntimeSaveCaptureBuilder::AddImmutableRecord(const CanonicalCaptureRecord &record,
                                                               std::shared_ptr<const IImmutableCanonicalPayload> payload) {
        if (sealed_)
            return Result<void>::Failure(MakeError(SaveErrors::CaptureAlreadySealed));
        const Result<std::pair<std::uint64_t, std::size_t>> inspected =
            InspectPayload(payload, limits_.maximumSegments - segmentCount_, limits_.maximumPayloadBytes - payloadByteLength_);
        if (inspected.HasError())
            return Result<void>::Failure(inspected.ErrorValue());
        const auto [byteLength, segments] = inspected.Value();
        if (const Result<void> admitted = ValidateAdmission(record, byteLength, segments); admitted.HasError())
            return admitted;
        try {
            CanonicalCaptureRecord ownedRecord = record;
            return AdmitPayload(std::move(ownedRecord), std::move(payload), byteLength, segments);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(SaveErrors::CaptureAllocationFailed));
        }
    }

    /** @copydoc RuntimeSaveCaptureBuilder::Seal */
    Result<RuntimeSaveSnapshot> RuntimeSaveCaptureBuilder::Seal() {
        if (sealed_)
            return Result<RuntimeSaveSnapshot>::Failure(MakeError(SaveErrors::CaptureAlreadySealed));

        try {
            auto projection = BuildParticipantProjection();
            if (projection.HasError())
                return Result<RuntimeSaveSnapshot>::Failure(projection.ErrorValue());

            std::ranges::sort(records_, RecordLess);
            auto immutableProjection =
                std::make_shared<const std::vector<CanonicalCaptureParticipantProjection>>(std::move(projection).Value());
            auto immutableRecords = std::make_shared<const std::vector<OwnedCanonicalSnapshot>>(std::move(records_));
            RuntimeSaveSnapshot snapshot{provenance_, payloadByteLength_, std::move(participants_), std::move(immutableProjection),
                                         std::move(immutableRecords)};
            sealed_ = true;
            recordAdmissions_.clear();
            usage_.clear();
            return Result<RuntimeSaveSnapshot>::Success(std::move(snapshot));
        } catch (const std::bad_alloc &) {
            return Result<RuntimeSaveSnapshot>::Failure(MakeError(SaveErrors::CaptureAllocationFailed));
        }
    }

    void RuntimeSaveCaptureBuilder::MoveFrom(RuntimeSaveCaptureBuilder &&other) noexcept {
        provenance_ = std::move(other.provenance_);
        participants_ = std::move(other.participants_);
        limits_ = other.limits_;
        usage_ = std::move(other.usage_);
        records_ = std::move(other.records_);
        recordAdmissions_ = std::move(other.recordAdmissions_);
        payloadByteLength_ = other.payloadByteLength_;
        segmentCount_ = other.segmentCount_;
        captureAttempted_ = other.captureAttempted_;
        sealed_ = other.sealed_;
        other.MarkSpent();
    }

    void RuntimeSaveCaptureBuilder::MarkSpent() noexcept {
        records_.clear();
        recordAdmissions_.clear();
        usage_.clear();
        participants_ = {};
        payloadByteLength_ = 0;
        segmentCount_ = 0;
        captureAttempted_ = true;
        sealed_ = true;
    }

    void RuntimeSaveCaptureBuilder::RollbackCapture(const std::size_t recordCount, const std::uint64_t payloadBytes,
                                                    const std::size_t segments, std::vector<ParticipantUsage> usage) noexcept {
        while (records_.size() > recordCount) {
            if (const auto admission = recordAdmissions_.find(records_.back().Record().record); admission != recordAdmissions_.end())
                admission->second.captured = false;
            records_.pop_back();
        }
        payloadByteLength_ = payloadBytes;
        segmentCount_ = segments;
        if (!usage.empty())
            usage_ = std::move(usage);
    }

    RuntimeSaveCaptureBuilder::ParticipantUsage *RuntimeSaveCaptureBuilder::FindUsage(const SaveParticipantId &participant) noexcept {
        const auto found = std::ranges::find(usage_, participant, &ParticipantUsage::participant);
        return found == usage_.end() ? nullptr : std::to_address(found);
    }

    const RuntimeSaveCaptureBuilder::ParticipantUsage *RuntimeSaveCaptureBuilder::FindUsage(
        const SaveParticipantId &participant) const noexcept {
        const auto found = std::ranges::find(usage_, participant, &ParticipantUsage::participant);
        return found == usage_.end() ? nullptr : std::to_address(found);
    }

    bool RuntimeSaveCaptureBuilder::HasValidRecord(const CanonicalCaptureRecord &record) const {
        const SaveParticipantBinding *binding = participants_.Find(record.participant);
        const auto admission = recordAdmissions_.find(record.record);
        if (binding == nullptr || admission == recordAdmissions_.end() || admission->second.participantIndex >= usage_.size())
            return false;
        return AllTrue(std::array{record.participant.IsValid(), record.schemaVersion.IsValid(), record.record.IsValid(),
                                  HasSaveParticipantRole(binding->Descriptor().roles, SaveParticipantRole::Capture),
                                  binding->Descriptor().schemaVersion == record.schemaVersion,
                                  usage_[admission->second.participantIndex].participant == record.participant});
    }

    bool RuntimeSaveCaptureBuilder::FitsAdmission(const CanonicalCaptureRecord &record, const std::uint64_t byteLength,
                                                  const std::size_t segments) const noexcept {
        const ParticipantUsage *usage = FindUsage(record.participant);
        const SaveParticipantBinding *binding = participants_.Find(record.participant);
        if (usage == nullptr || binding == nullptr || segments == 0)
            return false;
        return records_.size() < limits_.maximumRecords && segments <= limits_.maximumSegments - segmentCount_ &&
               byteLength <= limits_.maximumPayloadBytes - payloadByteLength_ &&
               usage->recordCount < binding->Descriptor().limits.maximumRecordCount &&
               byteLength <= binding->Descriptor().limits.maximumPayloadBytes - usage->payloadBytes;
    }

    Result<void> RuntimeSaveCaptureBuilder::ValidateAdmission(const CanonicalCaptureRecord &record, const std::uint64_t byteLength,
                                                              const std::size_t segments) const {
        if (!HasValidRecord(record))
            return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
        if (const auto admission = recordAdmissions_.find(record.record); admission->second.captured)
            return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordDuplicate));
        if (!FitsAdmission(record, byteLength, segments))
            return Result<void>::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
        return Result<void>::Success();
    }

    Result<void> RuntimeSaveCaptureBuilder::CaptureBinding(const SaveParticipantBinding &binding) {
        using enum CanonicalCaptureDisposition;
        const CanonicalStateParticipantDescriptor &descriptor = binding.Descriptor();
        ParticipantUsage *usage = FindUsage(descriptor.participant);
        if (usage == nullptr)
            return Result<void>::Failure(MakeError(SaveErrors::CaptureAdapterContractInvalid));

        ParticipantCaptureSink sink{*this, descriptor};
        const Result<CanonicalCaptureDisposition> captured = binding.Adapter()->Capture(MakeContext(binding), sink);
        if (captured.HasError())
            return Result<void>::Failure(captured.ErrorValue());
        if (sink.RejectedWrite() || (captured.Value() != Captured && captured.Value() != Omitted) ||
            (captured.Value() == Omitted && usage->recordCount != 0))
            return Result<void>::Failure(MakeError(SaveErrors::CaptureAdapterContractInvalid));
        usage->resolved = true;
        usage->disposition = captured.Value();
        if ((descriptor.required || captured.Value() == Captured) && usage->recordCount != descriptor.ownedRecords.size())
            return Result<void>::Failure(MakeError(SaveErrors::CaptureIncomplete));
        return Result<void>::Success();
    }

    bool RuntimeSaveCaptureBuilder::HasCompleteParticipantProjection(const CanonicalStateParticipantDescriptor &descriptor,
                                                                     const ParticipantUsage *usage, const bool captured) noexcept {
        return !(descriptor.required || captured) || (usage != nullptr && usage->recordCount == descriptor.ownedRecords.size());
    }

    Result<CanonicalCaptureDisposition> RuntimeSaveCaptureBuilder::ValidateParticipantProjection(
        const SaveParticipantBinding &binding) const {
        using enum CanonicalCaptureDisposition;
        const CanonicalStateParticipantDescriptor &descriptor = binding.Descriptor();
        const ParticipantUsage *usage = FindUsage(descriptor.participant);
        const bool captured = usage != nullptr && usage->recordCount != 0;
        if (!HasCompleteParticipantProjection(descriptor, usage, captured))
            return Result<CanonicalCaptureDisposition>::Failure(MakeError(SaveErrors::CaptureIncomplete));
        if (usage != nullptr && usage->resolved && usage->disposition == Omitted && captured)
            return Result<CanonicalCaptureDisposition>::Failure(MakeError(SaveErrors::CaptureAdapterContractInvalid));
        return Result<CanonicalCaptureDisposition>::Success(captured ? Captured : Omitted);
    }

    void RuntimeSaveCaptureBuilder::AppendParticipantProjection(std::vector<CanonicalCaptureParticipantProjection> &projection,
                                                                const SaveParticipantBinding &binding,
                                                                const CanonicalCaptureDisposition disposition) const {
        const CanonicalStateParticipantDescriptor &descriptor = binding.Descriptor();
        CanonicalCaptureParticipantProjection entry{
            .participant = descriptor.participant,
            .schemaVersion = descriptor.schemaVersion,
            .scope = descriptor.scope,
            .required = descriptor.required,
            .disposition = disposition,
        };
        if (disposition == CanonicalCaptureDisposition::Captured) {
            entry.records = descriptor.ownedRecords;
            std::ranges::sort(entry.records);
        }
        projection.push_back(std::move(entry));
    }

    Result<std::vector<CanonicalCaptureParticipantProjection>> RuntimeSaveCaptureBuilder::BuildParticipantProjection() const {
        std::vector<CanonicalCaptureParticipantProjection> projection;
        projection.reserve(participants_.Bindings().size());
        for (const SaveParticipantBinding &binding : participants_.Bindings()) {
            if (!HasSaveParticipantRole(binding.Descriptor().roles, SaveParticipantRole::Capture))
                continue;
            const Result<CanonicalCaptureDisposition> disposition = ValidateParticipantProjection(binding);
            if (disposition.HasError())
                return Result<std::vector<CanonicalCaptureParticipantProjection>>::Failure(disposition.ErrorValue());
            AppendParticipantProjection(projection, binding, disposition.Value());
        }
        return Result<std::vector<CanonicalCaptureParticipantProjection>>::Success(std::move(projection));
    }

    Result<void> RuntimeSaveCaptureBuilder::AdmitPayload(CanonicalCaptureRecord record,
                                                         std::shared_ptr<const IImmutableCanonicalPayload> payload,
                                                         const std::uint64_t byteLength, const std::size_t segments) {
        const SaveParticipantBinding *binding = participants_.Find(record.participant);
        ParticipantUsage *usage = FindUsage(record.participant);
        if (binding == nullptr || usage == nullptr)
            return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));

        auto admission = recordAdmissions_.find(record.record);
        if (admission == recordAdmissions_.end() || admission->second.captured)
            return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordDuplicate));
        records_.push_back(OwnedCanonicalSnapshot{std::move(record), binding->Adapter(), std::move(payload), byteLength, segments});
        admission->second.captured = true;
        payloadByteLength_ += byteLength;
        segmentCount_ += segments;
        usage->payloadBytes += byteLength;
        usage->segmentCount += segments;
        ++usage->recordCount;
        return Result<void>::Success();
    }

    CanonicalCaptureContext RuntimeSaveCaptureBuilder::MakeContext(const SaveParticipantBinding &binding) const noexcept {
        const ParticipantUsage *usage = FindUsage(binding.Descriptor().participant);
        const std::size_t participantRecords = usage == nullptr ? 0 : usage->recordCount;
        const std::uint64_t participantBytes = usage == nullptr ? 0 : usage->payloadBytes;
        return {
            .provenance = provenance_,
            .participant = binding.Descriptor().participant,
            .schemaVersion = binding.Descriptor().schemaVersion,
            .scope = binding.Descriptor().scope,
            .admission =
                {
                    .operationRecords = limits_.maximumRecords - records_.size(),
                    .operationSegments = limits_.maximumSegments - segmentCount_,
                    .operationPayloadBytes = limits_.maximumPayloadBytes - payloadByteLength_,
                    .participantRecords = static_cast<std::uint32_t>(binding.Descriptor().limits.maximumRecordCount - participantRecords),
                    .participantPayloadBytes = binding.Descriptor().limits.maximumPayloadBytes - participantBytes,
                    .maximumCopiedRecordBytes =
                        std::min({limits_.maximumCopiedRecordBytes, limits_.maximumPayloadBytes - payloadByteLength_,
                                  binding.Descriptor().limits.maximumPayloadBytes - participantBytes}),
                },
        };
    }
}  // namespace Horo::Runtime
