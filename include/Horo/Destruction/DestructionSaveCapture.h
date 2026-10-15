#pragma once

/** @file DestructionSaveCapture.h
 * @brief Owner-safe canonical destruction capture and actual Save participant integration.
 */
#include "Horo/Destruction/DestructionCanonicalState.h"
#include "Horo/Destruction/DestructionDamageRuntime.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Save/SaveCanonicalCodec.h"
#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"

#include <optional>
#include <thread>

namespace Horo::Destruction {
    /** @brief Explicit same-epoch Physics participation evidence; contains identities only, never body state.
     * @details The Physics canonical participant owns the named checkpoint and exact stable active chunk list.
     * The host captures this receipt from that participant at the common safe point, never from body observations.
     */
    struct DestructionPhysicsCaptureReceipt final {
        Runtime::SaveParticipantId participant;
        Runtime::SaveRecordId checkpoint;
        std::uint64_t sceneIncarnation{};
        Runtime::CanonicalCaptureEpoch epoch;
        DestructionHandle target;
        std::vector<DestructionChunkId> activeChunks;
    };

    /** @brief Complete source fence captured at one serialized owner phase. */
    struct DestructionSaveCaptureFence final {
        std::uint64_t sceneIncarnation{};
        std::uint64_t sceneRevision{};
        Runtime::CanonicalCaptureEpoch epoch;
        DestructionCapabilityRevision capabilityRevision;
        DestructionAuthorityEpoch authority;
        CancellationToken cancellation;
    };

    /** @brief Synchronous owner-phase borrow; every reference is used only until Capture returns. */
    struct DestructionSaveOwnerView final {
        const DestructionDamageRuntime &runtime;
        const DestructionDamageContext &context;
        const DestructionCanonicalArtifactView &artifact;
    };

    /** @brief Immutable owned semantic capture issued only after exact live-owner validation. */
    class CapturedDestructionState final {
    public:
        /** @brief Capture exact post-commit semantics from the actual damage owner without mutating it.
         * @param owner Complete synchronous borrow of actual runtime, current authority and exact artifact.
         * @param state Complete owned chunk/seed/support semantics from the same DFR owner phase.
         * @param fence Common Scene/Save epoch and cancellation admission.
         * @param physics Required exact Physics participant receipt when any active chunks exist.
         * @param limits Bounded semantic capture profile.
         * @return Owned immutable capture or original typed lifecycle/stale/content/capacity failure.
         * @details Candidate health/revision/phase/content/capabilities must equal the actual runtime owner. The
         * host supplies chunk semantics from its serialized DFR root, never from presentation events or Physics transforms.
         */
        [[nodiscard]] static Result<CapturedDestructionState> Capture(const DestructionSaveOwnerView &owner,
                                                                      CanonicalDestructionSnapshot state,
                                                                      const DestructionSaveCaptureFence &fence,
                                                                      const std::optional<DestructionPhysicsCaptureReceipt> &physics,
                                                                      const DestructionCanonicalLimits &limits = {});
        /** @brief Return the complete detached semantic root. @return Immutable owned value. */
        [[nodiscard]] const CanonicalDestructionSnapshot &State() const noexcept;
        /** @brief Return captured common source provenance. @return Scene/Save epoch values; grants no mutation authority. */
        [[nodiscard]] const DestructionSaveCaptureFence &Fence() const noexcept;
        /** @brief Return the required paired Physics checkpoint, if active motion exists. @return Immutable identity receipt. */
        [[nodiscard]] const std::optional<DestructionPhysicsCaptureReceipt> &Physics() const noexcept;

    private:
        /** @brief Retain a fully validated owned capture and its exact pairing evidence. */
        CapturedDestructionState(CanonicalDestructionSnapshot state, DestructionSaveCaptureFence fence,
                                 std::optional<DestructionPhysicsCaptureReceipt> physics);
        CanonicalDestructionSnapshot state_;
        DestructionSaveCaptureFence fence_;
        std::optional<DestructionPhysicsCaptureReceipt> physics_;
    };

    /** @brief Encode version1 Save-owned canonical semantic fields, independent from Network framing.
     * @param captured Sealed actual-owner capture. @param limits Finite codec ceilings.
     * @return Owned canonical bytes or typed bounded/cancellation failure; no live owner is retained.
     */
    [[nodiscard]] Result<Runtime::CanonicalEncodedValue> EncodeDestructionSaveState(const CapturedDestructionState &captured,
                                                                                    const Runtime::CanonicalCodecLimits &limits = {});

    /** @brief Detached Save record preserving semantic values and explicit paired participant provenance. */
    struct DecodedDestructionSaveState final {
        CanonicalDestructionSnapshot state;
        DestructionSaveCaptureFence fence; /**< Decoded value provenance; cancellation is empty and grants no authority. */
        std::optional<DestructionPhysicsCaptureReceipt> physics;
    };

    /** @brief Decode detached version1 semantics against exact artifact evidence without publishing a runtime root.
     * @param source Bounded Save reader. @param artifact Exact stable chunk table.
     * @param limits Finite semantic profile. @return Validated semantic state or original typed failure.
     * @details Restore/reconciliation and aggregate publication remain the separate DFR-006.3 owner responsibility.
     */
    [[nodiscard]] Result<DecodedDestructionSaveState> DecodeDestructionSaveState(Runtime::CanonicalValueReader source,
                                                                                 const DestructionCanonicalArtifactView &artifact,
                                                                                 const DestructionCanonicalLimits &limits = {});

    /** @brief Explicit owner lease invoked only at the serialized Save safe point; no ambient discovery.
     * @details Implementations own or safely borrow their DFR/Physics participants, reject replacement/shutdown,
     * and return only CapturedDestructionState issued from the live damage owner at the supplied common epoch.
     */
    class IDestructionSaveSource {
    public:
        virtual ~IDestructionSaveSource() = default;
        /** @brief Capture one complete current DFR record. @param context Common Save safe point and remaining budgets.
         * @return Sealed owned source or original typed owner failure.
         */
        [[nodiscard]] virtual Result<CapturedDestructionState> Capture(const Runtime::CanonicalCaptureContext &context) const = 0;
    };

    /** @brief Inert participant identity, owned record and finite capture limits. */
    struct DestructionSaveBinding final {
        Runtime::SaveParticipantId participant;
        Runtime::SaveRecordId record;
        std::uint64_t sceneIncarnation{};
        DestructionHandle target;
        FractureArtifactContentIdentity content;
        DestructionCapabilityRevision capabilityRevision;
        DestructionAuthorityEpoch authority;
        DestructionCanonicalLimits limits;
        std::optional<Runtime::SaveParticipantId> physicsParticipant;
    };

    /** @brief Actual Save capture adapter; workers receive canonical bytes, never this owner's runtime references. */
    class DestructionSaveAdapter final : public Runtime::ICanonicalStateAdapter {
        class ConstructionKey final {
            friend class DestructionSaveAdapter;
            ConstructionKey() = default;

        public:
            ConstructionKey(const ConstructionKey &) = default;
        };

    public:
        /** @brief Construct only with factory-held authority; registration and capture remain explicit.
         * @param key Non-forgeable factory passkey. @param binding Exact source opening and finite budgets.
         * @param source Explicit owner lease.
         */
        DestructionSaveAdapter(ConstructionKey key, DestructionSaveBinding binding, std::shared_ptr<const IDestructionSaveSource> source);
        /** @brief Bind explicit owner leases without registration or invoking capture.
         * @param binding Exact capture source epoch, record identity and bounds.
         * @param source Owner lease retained until adapter/capture retirement.
         * @return Adapter or typed invalid/budget failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<DestructionSaveAdapter>> Create(const DestructionSaveBinding &binding,
                                                                                    std::shared_ptr<const IDestructionSaveSource> source);
        /** @brief Build inert Save metadata without registration or callbacks. @return Capture-only descriptor. */
        [[nodiscard]] Runtime::CanonicalStateParticipantDescriptor Descriptor() const;
        /** @copydoc Runtime::ICanonicalStateAdapter::Capture */
        [[nodiscard]] Result<Runtime::CanonicalCaptureDisposition> Capture(const Runtime::CanonicalCaptureContext &context,
                                                                           Runtime::ICanonicalCaptureSink &sink) const override;
        /** @brief Idempotently close capture admission without touching a runtime owner or emitted bytes. */
        void Shutdown() noexcept;

    private:
        DestructionSaveBinding binding_;
        std::shared_ptr<const IDestructionSaveSource> source_;
        CancellationSource shutdown_;
        std::thread::id ownerThread_;
    };
}  // namespace Horo::Destruction
