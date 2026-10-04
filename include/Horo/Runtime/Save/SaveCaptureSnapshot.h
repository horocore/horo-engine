#pragma once

/**
 * @file SaveCaptureSnapshot.h
 * @brief Bounded adapter capture and immutable runtime-save snapshots.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveParticipantRegistry.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace Horo::Runtime {
    /** @brief Qualified hard ceiling for records retained by one detached capture. */
    inline constexpr std::size_t MaximumRuntimeSaveCaptureRecords = 16'384;
    /** @brief Qualified hard ceiling for immutable segments retained by one capture. */
    inline constexpr std::size_t MaximumRuntimeSaveCaptureSegments = 65'536;
    /** @brief Qualified hard ceiling for canonical bytes retained by one detached capture. */
    inline constexpr std::uint64_t MaximumRuntimeSaveCapturePayloadBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    /** @brief Default ceiling for one eager owner-thread copy; larger records use immutable leases. */
    inline constexpr std::uint64_t DefaultRuntimeSaveCopiedRecordBytes = 1024ULL * 1024ULL;

    /** @brief Non-zero logical tick shared by every participant in one coherent capture. */
    struct CanonicalCaptureEpoch final {
        std::uint64_t value{};

        /** @brief Reports whether this is an issued capture epoch. @return True for a non-zero value. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const CanonicalCaptureEpoch &) const noexcept = default;
    };

    /** @brief Stable source and registry evidence retained by a detached runtime-save snapshot. */
    struct RuntimeSaveCaptureProvenance final {
        CapturedStateId capturedState;      /**< Stable identity of this detached captured state. */
        CanonicalCaptureEpoch epoch;        /**< Exact logical tick shared by all records. */
        std::uint64_t sceneIncarnation{};   /**< Non-zero source scene lifetime generation. */
        std::uint64_t sceneRevision{};      /**< Non-zero committed source scene revision. */
        std::uint64_t registryGeneration{}; /**< Exact participant registry snapshot generation. */
        [[nodiscard]] constexpr auto operator<=>(const RuntimeSaveCaptureProvenance &) const noexcept = default;
    };

    /** @brief Operation-wide allocation policy and bounds visible before adapters capture state. */
    struct RuntimeSaveCaptureLimits final {
        std::size_t maximumParticipants{MaximumSaveParticipantCount};                /**< Maximum pinned participant bindings. */
        std::size_t maximumRecords{MaximumRuntimeSaveCaptureRecords};                /**< Maximum owned canonical records. */
        std::size_t maximumSegments{MaximumRuntimeSaveCaptureSegments};              /**< Maximum immutable payload segments. */
        std::uint64_t maximumPayloadBytes{MaximumRuntimeSaveCapturePayloadBytes};    /**< Maximum aggregate canonical bytes. */
        std::uint64_t maximumCopiedRecordBytes{DefaultRuntimeSaveCopiedRecordBytes}; /**< Maximum eager copy per record. */
        [[nodiscard]] constexpr auto operator<=>(const RuntimeSaveCaptureLimits &) const noexcept = default;
    };

    /** @brief Remaining exact admission visible before an adapter materializes state. */
    struct CanonicalCaptureAdmission final {
        std::size_t operationRecords{};           /**< Remaining aggregate records. */
        std::size_t operationSegments{};          /**< Remaining aggregate immutable segments. */
        std::uint64_t operationPayloadBytes{};    /**< Remaining aggregate payload bytes. */
        std::uint32_t participantRecords{};       /**< Remaining participant records. */
        std::uint64_t participantPayloadBytes{};  /**< Remaining participant payload bytes. */
        std::uint64_t maximumCopiedRecordBytes{}; /**< Per-record eager-copy ceiling. */
        [[nodiscard]] constexpr auto operator<=>(const CanonicalCaptureAdmission &) const noexcept = default;
    };

    /** @brief Exact value-only context supplied to a participant at the owner safe point. */
    struct CanonicalCaptureContext final {
        RuntimeSaveCaptureProvenance provenance; /**< Aggregate source and epoch evidence. */
        SaveParticipantId participant;           /**< Exact registered semantic owner. */
        ParticipantSchemaVersion schemaVersion;  /**< Exact registered payload schema. */
        SaveParticipantScope scope;              /**< Registered authority scope. */
        CanonicalCaptureAdmission admission;     /**< Checked remaining host allocation policy. */
        [[nodiscard]] auto operator<=>(const CanonicalCaptureContext &) const noexcept = default;
    };

    /** @brief Adapter-declared result for one participant capture attempt. */
    enum class CanonicalCaptureDisposition : std::uint8_t {
        Captured,
        Omitted
    };

    /** @brief Immutable concurrently readable canonical payload/root lease. */
    class IImmutableCanonicalPayload {
    public:
        virtual ~IImmutableCanonicalPayload() = default;
        /** @brief Returns the exact sum of all segments. @return Stable payload byte length. */
        [[nodiscard]] virtual std::uint64_t ByteLength() const noexcept = 0;
        /** @brief Returns the bounded immutable segment count. @return Stable segment count. */
        [[nodiscard]] virtual std::size_t SegmentCount() const noexcept = 0;
        /** @brief Returns one immutable concurrently readable segment. @param index Zero-based segment index.
         * @return Stable bytes, or an empty span when index is invalid.
         */
        [[nodiscard]] virtual std::span<const std::byte> Segment(std::size_t index) const noexcept = 0;
    };

    /** @brief Call-scoped host-owned sink used by one adapter under its exact capture context. */
    class ICanonicalCaptureSink {
    public:
        virtual ~ICanonicalCaptureSink() = default;
        /** @brief Copies a small borrowed record into host-owned immutable storage.
         * @param record Registered record identity owned by the active adapter.
         * @param bytes Borrowed safe-point bytes, never retained.
         * @return Success or a typed record, budget, duplicate, or allocation error.
         */
        [[nodiscard]] virtual Result<void> WriteCopied(SaveRecordId record, std::span<const std::byte> bytes) = 0;
        /** @brief Transfers a bounded immutable/COW payload lease without eager whole-state copying.
         * @param record Registered record identity owned by the active adapter.
         * @param payload Immutable payload/root lease whose segments remain stable.
         * @return Success or a typed record, budget, duplicate, or allocation error.
         */
        [[nodiscard]] virtual Result<void> WriteImmutable(SaveRecordId record,
                                                          std::shared_ptr<const IImmutableCanonicalPayload> payload) = 0;
    };

    /** @brief Stable semantic provenance supplied with one capture payload. */
    struct CanonicalCaptureRecord final {
        SaveParticipantId participant;          /**< Registered semantic owner. */
        ParticipantSchemaVersion schemaVersion; /**< Exact schema used by the canonical bytes. */
        SaveRecordId record;                    /**< Stable record identity owned by the participant. */
        [[nodiscard]] auto operator<=>(const CanonicalCaptureRecord &) const noexcept = default;
    };

    /** @brief Immutable manifest-facing participant projection, including explicit optional omission. */
    struct CanonicalCaptureParticipantProjection final {
        SaveParticipantId participant;             /**< Stable registered participant identity. */
        ParticipantSchemaVersion schemaVersion;    /**< Exact captured participant schema. */
        SaveParticipantScope scope;                /**< Registered persistence scope. */
        bool required{};                           /**< Descriptor-required policy. */
        CanonicalCaptureDisposition disposition{}; /**< Captured or descriptor-approved omitted state. */
        std::vector<SaveRecordId> records;         /**< Stable-sorted records; empty when omitted. */
        [[nodiscard]] auto operator<=>(const CanonicalCaptureParticipantProjection &) const noexcept = default;
    };

    /** @brief One immutable record detached from mutable runtime state. */
    class OwnedCanonicalSnapshot final {
    public:
        OwnedCanonicalSnapshot(const OwnedCanonicalSnapshot &) = delete;
        OwnedCanonicalSnapshot &operator=(const OwnedCanonicalSnapshot &) = delete;
        OwnedCanonicalSnapshot(OwnedCanonicalSnapshot &&) noexcept = default;
        OwnedCanonicalSnapshot &operator=(OwnedCanonicalSnapshot &&) noexcept = default;

        /** @brief Returns stable semantic record provenance. @return Borrowed immutable provenance. */
        [[nodiscard]] const CanonicalCaptureRecord &Record() const noexcept;
        /** @brief Returns the number of immutable payload segments. @return Bounded segment count. */
        [[nodiscard]] std::size_t SegmentCount() const noexcept;
        /** @brief Returns one immutable payload segment. @param index Zero-based segment index.
         * @return Stable bytes, or an empty span when index is invalid.
         */
        [[nodiscard]] std::span<const std::byte> Segment(std::size_t index) const noexcept;
        /** @brief Returns exact canonical payload bytes. @return Checked byte count. */
        [[nodiscard]] std::uint64_t ByteLength() const noexcept;

    private:
        friend class RuntimeSaveCaptureBuilder;
        OwnedCanonicalSnapshot(CanonicalCaptureRecord record, std::shared_ptr<const ICanonicalStateAdapter> adapterLease,
                               std::shared_ptr<const IImmutableCanonicalPayload> payload, std::uint64_t byteLength,
                               std::size_t segmentCount) noexcept;

        CanonicalCaptureRecord record_;
        std::shared_ptr<const ICanonicalStateAdapter> adapterLease_;
        std::shared_ptr<const IImmutableCanonicalPayload> payload_;
        std::uint64_t byteLength_{};
        std::size_t segmentCount_{};
    };

    /** @brief Immutable detached capture suitable for background encoding and storage. */
    class RuntimeSaveSnapshot final {
    public:
        RuntimeSaveSnapshot() = default;
        /** @brief Reports whether this is a sealed detached capture. @return True for an issued snapshot. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns immutable source and registry evidence. @return Borrowed stable provenance. */
        [[nodiscard]] const RuntimeSaveCaptureProvenance &Provenance() const noexcept;
        /** @brief Returns records in canonical participant then record identity order. @return Immutable owned view. */
        [[nodiscard]] std::span<const OwnedCanonicalSnapshot> Records() const noexcept;
        /** @brief Returns immutable manifest-facing participant projection. @return Stable ordered projection. */
        [[nodiscard]] std::span<const CanonicalCaptureParticipantProjection> Participants() const noexcept;
        /** @brief Returns exact aggregate canonical payload bytes. @return Checked byte total. */
        [[nodiscard]] std::uint64_t PayloadByteLength() const noexcept;

    private:
        friend class RuntimeSaveCaptureBuilder;
        RuntimeSaveSnapshot(RuntimeSaveCaptureProvenance provenance, std::uint64_t payloadByteLength,
                            SaveParticipantRegistrySnapshot participants,
                            std::shared_ptr<const std::vector<CanonicalCaptureParticipantProjection>> projection,
                            std::shared_ptr<const std::vector<OwnedCanonicalSnapshot>> records) noexcept;

        RuntimeSaveCaptureProvenance provenance_;
        std::uint64_t payloadByteLength_{};
        SaveParticipantRegistrySnapshot participants_;
        std::shared_ptr<const std::vector<CanonicalCaptureParticipantProjection>> projection_;
        std::shared_ptr<const std::vector<OwnedCanonicalSnapshot>> records_;
    };

    /** @brief Owner-safe-point coordinator and bounded host payload sink. */
    class RuntimeSaveCaptureBuilder final {
    public:
        RuntimeSaveCaptureBuilder(const RuntimeSaveCaptureBuilder &) = delete;
        RuntimeSaveCaptureBuilder &operator=(const RuntimeSaveCaptureBuilder &) = delete;
        RuntimeSaveCaptureBuilder(RuntimeSaveCaptureBuilder &&other) noexcept;
        RuntimeSaveCaptureBuilder &operator=(RuntimeSaveCaptureBuilder &&other) noexcept;

        /** @brief Validates capture evidence, allocation policy, and pinned registry generation.
         * @param provenance Stable source and registry evidence captured at the owner safe point.
         * @param participants Immutable registry snapshot whose adapter leases outlive payload leases.
         * @param limits Qualified operation-wide allocation policy and bounds.
         * @return Empty coordinator or a typed context, stale-generation, or allocation error.
         */
        [[nodiscard]] static Result<RuntimeSaveCaptureBuilder> Create(RuntimeSaveCaptureProvenance provenance,
                                                                      SaveParticipantRegistrySnapshot participants,
                                                                      const RuntimeSaveCaptureLimits &limits = {});

        /** @brief Invokes every capture adapter with exact context and a bounded host sink.
         * @return Success or the first typed adapter, sink, contract, budget, or allocation failure.
         */
        [[nodiscard]] Result<void> CaptureParticipants();
        /** @brief Copies one borrowed canonical payload after validation and admission.
         * @param record Stable participant, schema, and record identity.
         * @param bytes Borrowed owner-safe-point bytes; never retained.
         * @return Success or a typed sealed, invalid-record, duplicate, budget, or allocation error.
         */
        [[nodiscard]] Result<void> AddRecord(const CanonicalCaptureRecord &record, std::span<const std::byte> bytes);
        /** @brief Admits one immutable/COW payload lease without eager whole-state copying.
         * @param record Stable participant, schema, and record identity.
         * @param payload Immutable segmented payload/root lease.
         * @return Success or a typed sealed, invalid-record, duplicate, budget, or allocation error.
         */
        [[nodiscard]] Result<void> AddImmutableRecord(const CanonicalCaptureRecord &record,
                                                      std::shared_ptr<const IImmutableCanonicalPayload> payload);
        /** @brief Publishes one coherently ordered immutable capture and spends this builder.
         * @return Detached snapshot, or a typed incomplete, sealed, or allocation error.
         */
        [[nodiscard]] Result<RuntimeSaveSnapshot> Seal();

    private:
        struct ParticipantUsage final {
            SaveParticipantId participant;
            std::uint64_t payloadBytes{};
            std::size_t recordCount{};
            std::size_t segmentCount{};
            bool resolved{};
            CanonicalCaptureDisposition disposition{CanonicalCaptureDisposition::Omitted};
        };

        struct RecordAdmission final {
            std::size_t participantIndex{};
            bool captured{};
        };

        using RecordAdmissions = std::unordered_map<SaveRecordId, RecordAdmission, PersistentSaveIdentityHash<SaveRecordIdentityTag>>;

        struct AdmissionState final {
            std::vector<ParticipantUsage> usage;
            RecordAdmissions records;
        };

        RuntimeSaveCaptureBuilder(RuntimeSaveCaptureProvenance provenance, SaveParticipantRegistrySnapshot participants,
                                  const RuntimeSaveCaptureLimits &limits, std::vector<ParticipantUsage> usage,
                                  RecordAdmissions recordAdmissions) noexcept;
        void MoveFrom(RuntimeSaveCaptureBuilder &&other) noexcept;
        void MarkSpent() noexcept;
        void RollbackCapture(std::size_t recordCount, std::uint64_t payloadBytes, std::size_t segmentCount,
                             std::vector<ParticipantUsage> usage) noexcept;
        [[nodiscard]] ParticipantUsage *FindUsage(const SaveParticipantId &participant) noexcept;
        [[nodiscard]] const ParticipantUsage *FindUsage(const SaveParticipantId &participant) const noexcept;
        [[nodiscard]] bool HasValidRecord(const CanonicalCaptureRecord &record) const;
        [[nodiscard]] bool FitsAdmission(const CanonicalCaptureRecord &record, std::uint64_t byteLength,
                                         std::size_t segmentCount) const noexcept;
        [[nodiscard]] Result<void> ValidateAdmission(const CanonicalCaptureRecord &record, std::uint64_t byteLength,
                                                     std::size_t segmentCount) const;
        /** @brief Captures the participant set with rollback on failure. */
        [[nodiscard]] Result<void> CaptureAllParticipants();
        [[nodiscard]] Result<void> CaptureBinding(const SaveParticipantBinding &binding);
        [[nodiscard]] static Result<void> ValidateCreationContext(const RuntimeSaveCaptureProvenance &provenance,
                                                                  const SaveParticipantRegistrySnapshot &participants,
                                                                  const RuntimeSaveCaptureLimits &limits);
        [[nodiscard]] static Result<AdmissionState> BuildAdmissionState(const SaveParticipantRegistrySnapshot &participants);
        [[nodiscard]] static bool HasCompleteParticipantProjection(const CanonicalStateParticipantDescriptor &descriptor,
                                                                   const ParticipantUsage *usage, bool captured) noexcept;
        [[nodiscard]] Result<CanonicalCaptureDisposition> ValidateParticipantProjection(const SaveParticipantBinding &binding) const;
        void AppendParticipantProjection(std::vector<CanonicalCaptureParticipantProjection> &projection,
                                         const SaveParticipantBinding &binding, CanonicalCaptureDisposition disposition) const;
        [[nodiscard]] Result<std::vector<CanonicalCaptureParticipantProjection>> BuildParticipantProjection() const;
        [[nodiscard]] Result<void> AdmitPayload(CanonicalCaptureRecord record, std::shared_ptr<const IImmutableCanonicalPayload> payload,
                                                std::uint64_t byteLength, std::size_t segmentCount);
        [[nodiscard]] CanonicalCaptureContext MakeContext(const SaveParticipantBinding &binding) const noexcept;

        RuntimeSaveCaptureProvenance provenance_;
        SaveParticipantRegistrySnapshot participants_;
        RuntimeSaveCaptureLimits limits_;
        std::vector<ParticipantUsage> usage_;
        std::vector<OwnedCanonicalSnapshot> records_;
        RecordAdmissions recordAdmissions_;
        std::uint64_t payloadByteLength_{};
        std::size_t segmentCount_{};
        bool captureAttempted_{};
        bool sealed_{false};
    };
}  // namespace Horo::Runtime
