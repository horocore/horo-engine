#pragma once
#include "Horo/Network/NetworkDebugger.h"

/** @file ReplicationStateCapture.h
 * @brief Prepared authoritative capture of committed owner state into immutable bounded snapshots.
 */

#include "Horo/Network/ReplicationSerializer.h"
#include "Horo/Network/ReplicationWorldLifecycle.h"

#include <atomic>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::Network {
    namespace ReplicationCaptureErrors {
        extern const ErrorCodeDescriptor Invalid;       /**< Malformed target, field, value or capture order. */
        extern const ErrorCodeDescriptor CallbackFault; /**< Owner or codec callback failed unexpectedly. */
        extern const ErrorCodeDescriptor Capacity;      /**< Prepared storage or work capacity exhausted. */
        extern const ErrorCodeDescriptor Stale;         /**< World, mapping, descriptor or owner generation changed. */
        extern const ErrorCodeDescriptor Uncommitted;   /**< Owner state is not committed for the requested tick. */
        extern const ErrorCodeDescriptor Closed;        /**< Capture admission is permanently closed. */
    }  // namespace ReplicationCaptureErrors

    namespace Detail {
        struct ReplicationPreparedCaptureField;
    }

    /** @brief One copied canonical field; it never retains owner storage or a native address. */
    struct ReplicationCapturedField final {
        FieldId field;
        ReplicationRuntimeValue value;
    };

    /** @brief Immutable pool-backed source snapshot shared by all downstream client projections.
     * Fields retain typed canonical source values, not encoded buffers. A pin prevents slot reuse.
     * Mapping/world revocation or coordinator shutdown makes IsCurrent false immediately; copied
     * historical data is never eligible as a baseline for a replacement logical identity.
     */
    class ReplicationCapturedState final {
    public:
        /** @brief Tests logical generation validity from any thread. @return Whether this pin remains eligible. */
        [[nodiscard]] bool IsCurrent() const noexcept;
        /** @brief Returns exact source occurrence. @return Owned mapping provenance. */
        [[nodiscard]] const NetworkObjectMappingEntry &Object() const noexcept;
        /** @brief Returns pinned capture-world identity for downstream generation fencing.
         * @return Immutable capture read; consult IsCurrent before its descriptor.
         */
        [[nodiscard]] const ReplicationWorldCaptureRead &World() const noexcept;
        /** @brief Returns committed capture tick. @return Positive owner simulation tick. */
        [[nodiscard]] std::uint64_t SimulationTick() const noexcept;
        /** @brief Returns owner source revision. @return Positive committed source revision. */
        [[nodiscard]] std::uint64_t SourceRevision() const noexcept;
        /** @brief Returns owner transaction revision. @return Exact complete owner publication revision. */
        [[nodiscard]] std::uint64_t CommitRevision() const noexcept;
        /** @brief Returns semantic capture publication count. @return Positive non-wrapping revision for this target. */
        [[nodiscard]] std::uint64_t PublicationRevision() const noexcept;
        /** @brief Returns pinned complete descriptor generation. @return Immutable schema pin. */
        [[nodiscard]] const ReplicationDescriptorSnapshotPtr &Descriptors() const noexcept;
        /** @brief Views identity-sorted copied values. @return Borrow valid only while this snapshot pin lives. */
        [[nodiscard]] std::span<const ReplicationCapturedField> Fields() const noexcept;

    private:
        friend class ReplicationStateCapture;
        friend class ReplicationCaptureWriter;
        NetworkObjectMappingEntry object_;
        ReplicationWorldCaptureRead world_;
        ReplicationDescriptorSnapshotPtr descriptors_;
        std::shared_ptr<const std::atomic_bool> admission_;
        std::vector<ReplicationCapturedField> fields_;
        std::uint64_t simulationTick_{};
        std::uint64_t sourceRevision_{};
        std::uint64_t commitRevision_{};
        std::uint64_t publicationRevision_{};
    };

    /** @brief Immutable snapshot pin; copying it performs no allocation. */
    using ReplicationCapturedStatePin = std::shared_ptr<const ReplicationCapturedState>;

    /** @brief Non-retainable bounded writer over a private prepared candidate.
     * Every declared field must be written exactly once. Container writes copy into reserved
     * storage; undeclared fields, wrong types and exceeded bounds reject the complete candidate.
     */
    class ReplicationCaptureWriter final {
    public:
        ReplicationCaptureWriter(const ReplicationCaptureWriter &) = delete;
        ReplicationCaptureWriter &operator=(const ReplicationCaptureWriter &) = delete;
        /** @brief Copies one declared Boolean field. @param field Stable field. @param value Canonical source. @return Typed result. */
        [[nodiscard]] Result<void> Write(FieldId field, bool value);
        /** @brief Copies one declared signed integer. @param field Stable field. @param value Canonical source. @return Typed result. */
        [[nodiscard]] Result<void> Write(FieldId field, std::int64_t value);
        /** @brief Copies one declared unsigned integer. @param field Stable field. @param value Canonical source. @return Typed result. */
        [[nodiscard]] Result<void> Write(FieldId field, std::uint64_t value);
        /** @brief Copies one declared finite scalar. @param field Stable field. @param value Canonical source. @return Typed result. */
        [[nodiscard]] Result<void> Write(FieldId field, double value);
        /** @brief Copies bounded text. @param field Stable field. @param value Borrowed until return. @return Typed result. */
        [[nodiscard]] Result<void> Write(FieldId field, std::string_view value);
        /** @brief Copies bounded bytes. @param field Stable field. @param value Borrowed until return. @return Typed result. */
        [[nodiscard]] Result<void> Write(FieldId field, std::span<const std::byte> value);

    private:
        friend class ReplicationStateCapture;
        ReplicationCaptureWriter(ReplicationCapturedState &state, std::span<Detail::ReplicationPreparedCaptureField> fields) noexcept;
        [[nodiscard]] Result<std::size_t> Admit(FieldId field, ReplicationValueKind kind, std::size_t elements);
        [[nodiscard]] Result<void> Complete() const;
        ReplicationCapturedState &state_;
        std::span<Detail::ReplicationPreparedCaptureField> fields_;
        bool failed_{};
    };

    /** @brief Exact transaction evidence issued by the canonical owner while its read scope is open. */
    struct ReplicationCommittedRead final {
        std::uint64_t simulationTick{};
        std::uint64_t sourceRevision{};
        std::uint64_t commitRevision{};
    };

    /** @brief Owner adapter pinned by explicit host composition; never a second component registry.
     * The adapter owns/pins its module and canonical state lifetime. BeginRead rejects preparing or
     * partially committed state and holds the read-only owner barrier until EndRead. Capture must
     * perform bounded work, allocate nothing, retain neither writer nor owner views, and invoke no
     * transport or consumer. IsCurrent fences reentrant replacement before publication. EndRead
     * runs exactly once after every successful BeginRead, including failure and cancellation.
     */
    class ICommittedReplicationSource {
    public:
        virtual ~ICommittedReplicationSource() = default;
        /** @brief Opens an owner-safe read. @param object Exact object/entity. @param tick Committed tick. @return Read or typed failure.
         */
        [[nodiscard]] virtual Result<ReplicationCommittedRead> BeginRead(const NetworkObjectMappingEntry &object,
                                                                         std::uint64_t tick) const = 0;
        /** @brief Writes declared canonical values. @param read Open scope. @param writer Prepared output. @return Complete result. */
        [[nodiscard]] virtual Result<void> Capture(const ReplicationCommittedRead &read, ReplicationCaptureWriter &writer) const = 0;
        /** @brief Tests the still-open owner publication. @param read Open scope. @return Exact generation remains committed. */
        [[nodiscard]] virtual bool IsCurrent(const ReplicationCommittedRead &read) const noexcept = 0;
        /** @brief Closes the owner read barrier. @param read Previously successful BeginRead evidence. */
        virtual void EndRead(const ReplicationCommittedRead &read) const noexcept = 0;
    };

    /** @brief Explicit composition binding between one mapping occurrence and its declaring owner. */
    struct ReplicationCaptureTarget final {
        NetworkObjectMappingEntry object;
        std::shared_ptr<const ICommittedReplicationSource> source;
    };

    /** @brief Finite preparation and per-tick work limits; all pool backing storage is reserved before reads open. */
    struct ReplicationCaptureLimits final {
        std::size_t maximumTargets{256};
        std::size_t maximumFieldsPerTarget{128};
        std::size_t snapshotSlotsPerTarget{3};
        std::size_t maximumPreparedBytes{16 * 1024 * 1024};
        std::size_t maximumTargetsPerTick{16};
        std::size_t maximumFieldsPerTick{1024};
        std::size_t maximumBytesPerTick{1024 * 1024};
    };

    /** @brief Owned bounded result of one rotating reconciliation pass. Failures publish no partial object. */
    struct ReplicationCaptureReport final {
        std::uint64_t simulationTick{};
        std::size_t considered{};
        std::size_t published{};
        std::size_t unchanged{};
        std::size_t failed{};
        std::size_t deferred{};
        std::optional<Error> firstError;
    };

    /** @brief Owner-thread coordinator for authoritative committed canonical copies.
     * The host installs a complete target/descriptor generation outside simulation. CaptureAtCommit
     * reserves stable rotating reconciliation before bounded hint service under object/field/byte budgets; lost hints are reconciled and
     * duplicate hints carry no values. Shutdown precedes module/world destruction. Replace the whole
     * coordinator for descriptor or target-set replacement; Shutdown revokes all old snapshot pins.
     */
    class ReplicationStateCapture final {
        struct Impl;

        struct ConstructionKey final {
        private:
            friend class ReplicationStateCapture;
            ConstructionKey() = default;
        };

    public:
        /** @brief Factory-only constructor admitted by an unforgeable private key. @internal
         * @param key Private admission created only after Prepare validates and reserves the complete generation.
         * @param impl Transactionally prepared sole implementation owner.
         */
        ReplicationStateCapture(ConstructionKey key, std::unique_ptr<Impl> impl) noexcept;
        /** @brief Prepares complete bounded pools and owner pins transactionally.
         * @param world Active generation read acquired during composition.
         * @param serializers Exact immutable descriptor/codec generation.
         * @param targets Complete explicit owner bindings.
         * @param limits Finite storage and reconciliation bounds.
         * @param debugger Optional owner-thread collector retained by the host through capture destruction.
         * @return Prepared coordinator or a typed error without publication.
         */
        [[nodiscard]] static Result<std::unique_ptr<ReplicationStateCapture>> Prepare(
            const ReplicationWorldCaptureRead &world, std::shared_ptr<const ReplicationSerializerRegistry> serializers,
            std::span<const ReplicationCaptureTarget> targets, const ReplicationCaptureLimits &limits = {},
            NetworkDebugger *debugger = nullptr);
        ~ReplicationStateCapture();
        ReplicationStateCapture(const ReplicationStateCapture &) = delete;
        ReplicationStateCapture &operator=(const ReplicationStateCapture &) = delete;
        /** @brief Coalesces one value-free hint. @param object Exact occurrence. @return Accepted or typed identity/lifecycle error. */
        [[nodiscard]] Result<void> MarkDirty(NetworkObjectId object);
        /** @brief Captures a bounded pass after a complete canonical owner commit.
         * @param world Current allocation-free lifecycle read from NetworkFlush.
         * @param committedTick Positive strictly increasing committed simulation tick.
         * @param cancellation Caller cancellation; checked before/after every owner callback.
         * @return Bounded report or typed lifecycle/order error. Existing current pins survive owner failure.
         * Callback exceptions become typed candidate failures; allocation failure reports Capacity.
         */
        [[nodiscard]] Result<ReplicationCaptureReport> CaptureAtCommit(const ReplicationWorldCaptureRead &world,
                                                                       std::uint64_t committedTick,
                                                                       const CancellationToken &cancellation = {});
        /** @brief Pins the latest eligible source. @param object Exact occurrence. @return Current immutable pin or typed unavailable. */
        [[nodiscard]] Result<ReplicationCapturedStatePin> Latest(NetworkObjectId object) const;
        /** @brief Tests whether shutdown and every external immutable pin/read have drained.
         * @return True when owner-thread destruction can reclaim pools and module pins at a quiescent boundary.
         * @pre Host calls this only on the owner thread outside normal capture work.
         */
        [[nodiscard]] bool CanReclaim() const noexcept;
        /** @brief Permanently closes admission, revokes pins and retains prepared pools and owner/module references until quiescent
         * destruction; safe repeatedly on owner thread. */
        void Shutdown() noexcept;

    private:
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Network
