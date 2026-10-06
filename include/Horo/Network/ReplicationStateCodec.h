#pragma once

/** @file ReplicationStateCodec.h
 * @brief Bounded canonical full and baseline-relative state records over committed capture pins.
 */

#include "Horo/Network/ReplicationStateCapture.h"

#include <thread>

namespace Horo::Network {
    namespace ReplicationStateErrors {
        extern const ErrorCodeDescriptor Invalid;       /**< Invalid framing, field order or operation. */
        extern const ErrorCodeDescriptor Stale;         /**< Wrong session, descriptor, object or baseline occurrence. */
        extern const ErrorCodeDescriptor Capacity;      /**< Finite wire, field or retained value bound exceeded. */
        extern const ErrorCodeDescriptor Closed;        /**< Session codec retired. */
        extern const ErrorCodeDescriptor CallbackFault; /**< Foreign serializer exception. */
    }  // namespace ReplicationStateErrors

    /** @brief Explicit finite per-record work, wire and retained container limits. */
    struct ReplicationStateCodecLimits final {
        std::size_t maximumFields{128};
        std::size_t maximumWireBytes{1024 * 1024};
        std::size_t maximumValueBytes{1024 * 1024};
    };

    /** @brief Host-validated acknowledgement of one immutable committed source occurrence.
     * The connection owner must validate the acknowledgement's session and object before supplying
     * this value. Missing, expired or incompatible evidence selects a full record; it never recaptures
     * Gameplay. Per-connection acknowledgement/history storage belongs to the connection owner.
     */
    struct ReplicationAcknowledgedBaseline final {
        ReplicationCapturedStatePin state;
        std::uint64_t publicationRevision{};
        ReplicationRoleRevision roleRevision; /**< Exact acknowledged recipient projection; transfer forces full state. */
        Sha256Digest projectionFingerprint;   /**< Exact acknowledged field visibility and record kind. */
        std::uint64_t descriptorGeneration{}; /**< Exact acknowledged negotiated generation. */
    };

    /** @brief Owner-computed visibility for one schema field with a Custom condition. */
    struct ReplicationStateCustomVisibility final {
        FieldId field;
        ReplicationCustomConditionEvidence evidence;
    };

    /** @brief Immutable complete decoded network representation, with no Scene mutation authority.
     * A value is constructed only after complete record validation. It pins the codec generation and
     * must still pass the receiving owner's session/object safe-point validation before application.
     * Quantized values are replicas; they never replace canonical server or save/hash state.
     */
    class ReplicationDecodedState final {
    public:
        /** @brief Returns exact authoritative object occurrence. @return Object identity. */
        [[nodiscard]] NetworkObjectId Object() const noexcept {
            return object_;
        }

        /** @brief Returns authoritative committed tick. @return Positive tick. */
        [[nodiscard]] std::uint64_t SimulationTick() const noexcept {
            return tick_;
        }

        /** @brief Returns semantic source publication. @return Positive revision scoped by complete identity. */
        [[nodiscard]] std::uint64_t PublicationRevision() const noexcept {
            return revision_;
        }

        /** @brief Returns canonical-order network values. @return Borrow valid during this value's lifetime. */
        [[nodiscard]] std::span<const ReplicationCapturedField> Fields() const noexcept {
            return fields_;
        }

        /** @brief Tests codec retirement. @return Whether this generation remains eligible. */
        [[nodiscard]] bool IsCurrent() const noexcept {
            return admission_ && admission_->load();
        }

    private:
        friend class ReplicationStateCodec;
        ReplicationDecodedState() = default;
        NetworkObjectId object_;
        NetworkObjectMappingEntry mapping_;
        ReplicationSchemaId schema_;
        ReplicationSchemaVersion version_;
        std::uint64_t tick_{}, revision_{};
        std::shared_ptr<const ReplicationSerializerRegistry> serializers_;
        std::shared_ptr<const std::atomic_bool> admission_;
        std::vector<ReplicationCapturedField> fields_;
    };

    /** @brief Session-owner codec for one explicitly negotiated immutable descriptor generation.
     * Compose only after Active-session admission; generation is host-negotiated, never a pointer or
     * process-local address. Calls are serialized on the constructing owner thread. Each call performs
     * bounded field work and bounded allocations using existing typed serializers; hosts charge the
     * record allowance before calling. There is no transport, capture callback, history or component
     * access. Shutdown precedes session/module retirement and invalidates decoded baselines.
     */
    class ReplicationStateCodec final {
        struct ConstructionKey final {
        private:
            friend class ReplicationStateCodec;
            ConstructionKey() = default;
        };

    public:
        /** @brief Factory-only constructor admitted by an unforgeable private key. @internal
         * @param key Private admission created after validating the complete projection.
         * @param serializers Exact immutable serializer generation.
         * @param recipient Admitted client role and object occurrence.
         * @param generation Positive negotiated descriptor generation.
         * @param limits Validated finite work and storage limits.
         * @param projection Complete canonical field visibility projection.
         * @param fingerprint Prepared projection identity.
         */
        ReplicationStateCodec(ConstructionKey key, std::shared_ptr<const ReplicationSerializerRegistry> serializers,
                              ReplicationRoleBinding recipient, std::uint64_t generation, ReplicationStateCodecLimits limits,
                              std::vector<FieldId> projection, const Sha256Digest &fingerprint);
        /** @brief Validates finite limits and pins the complete serializer generation.
         * @param serializers Exact negotiated schema projection and codecs.
         * @param recipient Exact host-admitted client object role and ownership publication.
         * @param record Spawn or update projection, including InitialOnly visibility.
         * @param descriptorGeneration Positive host-negotiated descriptor generation.
         * @param custom Complete exact custom-field visibility; missing evidence fails closed.
         * @param limits Finite per-record work and storage bounds.
         * @return Sole codec owner or typed invalid/capacity error.
         */
        [[nodiscard]] static Result<std::unique_ptr<ReplicationStateCodec>> Create(
            std::shared_ptr<const ReplicationSerializerRegistry> serializers, const ReplicationRoleBinding &recipient,
            ReplicationRecordKind record, std::uint64_t descriptorGeneration, std::span<const ReplicationStateCustomVisibility> custom = {},
            const ReplicationStateCodecLimits &limits = {});
        /** @brief Revokes decoded baseline eligibility before releasing the registry generation. */
        ~ReplicationStateCodec();
        ReplicationStateCodec(const ReplicationStateCodec &) = delete;
        ReplicationStateCodec &operator=(const ReplicationStateCodec &) = delete;
        /** @brief Encodes committed source once without accessing its canonical owner.
         * @param source Current authority capture pin from the exact composed session/schema generation.
         * @param baseline Host-validated acknowledgement; unusable evidence selects full state.
         * @param cancellation Checked before and after every serializer callback.
         * @return Canonical complete bytes or typed error; source and baseline are unchanged.
         */
        [[nodiscard]] Result<std::vector<std::byte>> Encode(ReplicationCapturedStatePin source,
                                                            const ReplicationAcknowledgedBaseline &baseline = {},
                                                            const CancellationToken &cancellation = {});
        /** @brief Validates complete framing before decoding any field, then privately reconstructs state.
         * @param wire Bounded complete record, borrowed only until return.
         * @param expected Host-resolved exact object and negotiated schema provenance.
         * @param baseline Previously decoded exact baseline; required for delta, ignored for full state.
         * @param cancellation Checked before and after every serializer callback.
         * @return Complete immutable network state or typed failure without publishing any partial values.
         */
        [[nodiscard]] Result<ReplicationDecodedState> Decode(std::span<const std::byte> wire, const NetworkObjectMappingEntry &expected,
                                                             const ReplicationDecodedState *baseline = nullptr,
                                                             const CancellationToken &cancellation = {});
        /** @brief Closes admission permanently on the owner thread; repeated calls are safe. */
        void Shutdown() const noexcept;

        /** @brief Returns the immutable field projection bound into records and acknowledgements.
         * @return Canonical visibility/record-kind digest, never mutation authority.
         */
        [[nodiscard]] const Sha256Digest &ProjectionFingerprint() const noexcept {
            return fingerprint_;
        }

        /** @brief Returns the negotiated generation for validated acknowledgements. @return Positive descriptor generation. */
        [[nodiscard]] std::uint64_t DescriptorGeneration() const noexcept {
            return generation_;
        }

    private:
        /** @brief Checks owner affinity, permanent session closure and caller cancellation. */
        [[nodiscard]] Result<void> Admit(const CancellationToken &cancellation) const;
        /** @brief Fences captured authority and exact composed immutable schema/session/object occurrence. */
        [[nodiscard]] bool CurrentSource(const ReplicationCapturedStatePin &source) const noexcept;
        /** @brief Tests acknowledged same-generation source pin eligibility as a delta root. */
        [[nodiscard]] bool UsableBaseline(const ReplicationCapturedStatePin &source,
                                          const ReplicationAcknowledgedBaseline &baseline) const noexcept;
        /** @brief Appends exact versioned framing for a full record or acknowledged delta. */
        void AppendHeader(std::vector<std::byte> &wire, const ReplicationCapturedStatePin &source,
                          const ReplicationAcknowledgedBaseline *baseline) const;
        /** @brief Owns validated field bytes and contains faults until a complete immutable candidate is ready. */
        [[nodiscard]] Result<ReplicationDecodedState> CompleteState(ReplicationDecodedState state, std::span<const std::byte> fields,
                                                                    std::uint64_t count, const ReplicationDecodedState *baseline,
                                                                    const CancellationToken &cancellation);
        /** @brief Revalidates cancellation, closure and captured pins after a foreign codec callback. */
        [[nodiscard]] Result<void> ContinueEncoding(const ReplicationCapturedStatePin &source,
                                                    const ReplicationAcknowledgedBaseline *baseline,
                                                    const CancellationToken &cancellation) const;
        /** @brief Compares and encodes one projected field without consulting its canonical owner. */
        [[nodiscard]] Result<std::optional<ReplicationEncodedValue>> EncodeField(FieldId field, const ReplicationCapturedStatePin &source,
                                                                                 const ReplicationAcknowledgedBaseline *baseline,
                                                                                 const CancellationToken &cancellation) const;
        /** @brief Decodes and re-encodes one bounded field before admitting its typed candidate. */
        [[nodiscard]] Result<ReplicationRuntimeValue> DecodeField(FieldId field, const ReplicationEncodedValue &encoded,
                                                                  const CancellationToken &cancellation) const;
        /** @brief Privately reconstructs complete ordered fields over an optional exact validated baseline. */
        [[nodiscard]] Result<std::vector<ReplicationCapturedField>> Reconstruct(std::span<const std::byte> fields, std::uint64_t count,
                                                                                const ReplicationDecodedState *baseline,
                                                                                const CancellationToken &cancellation) const;
        std::shared_ptr<const ReplicationSerializerRegistry> serializers_;
        ReplicationRoleBinding recipient_;
        std::uint64_t generation_{};
        ReplicationStateCodecLimits limits_;
        std::size_t wireCapacity_{};
        std::shared_ptr<std::atomic_bool> admission_;
        std::thread::id owner_;
        bool operating_{};
        std::vector<FieldId> projection_;
        Sha256Digest fingerprint_;
    };
}  // namespace Horo::Network
