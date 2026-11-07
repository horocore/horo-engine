#pragma once
#include "Horo/Runtime/Save/SaveRestoreTransaction.h"
#include "PhysicsSceneStateProjection.h"
#include "PhysicsSceneStateRebind.h"

namespace Horo::Physics::Detail {
    class PhysicsSceneHostAccess;
    class PreparedPhysicsSceneCapture;
    class PreparedPhysicsSceneRestore;

    struct SceneStateStoragePolicy final {
        SceneStateProjectionLimits projection;
        std::uint32_t snapshotSlots{};
        std::size_t maximumRetainedBytes{};
    };

    /** @brief Exact actual candidate/lifecycle target. Only existing PhysicsSceneHostAccess can issue it.
     * Weak existing structural ownership prevents stale Scene-ID reuse or world access after retirement.
     * No public raw World, worker operation, fabricated owner identity or independent publication authority is exposed.
     */
    class PhysicsSceneStateTarget final {
        friend class PhysicsSceneHostAccess;
        friend class PreparedPhysicsSceneCapture;
        friend class PreparedPhysicsSceneRestore;

    public:
        /** @brief Copied actual Scene identity only; not readiness or a caller-issued World identity. */
        [[nodiscard]] Runtime::SceneRuntimeId Scene() const noexcept {
            return scene_;
        }

        /** @brief Probes only this exact published owner; busy/unticked/incomplete structural publication defers.
         * @return Actual joined completed-tick readiness; lifetime, lane, provider and coverage failures remain typed errors. */
        [[nodiscard]] Result<bool> CaptureReady() const;

        /** @brief Actual owner-derived finite limits/profile, followed by SAME canonical pure decode. Never worker-callable. */
        [[nodiscard]] Result<std::unique_ptr<StateDetail::RestartInput>> DecodeInput(std::span<const std::byte>,
                                                                                     std::size_t maximumWireBytes,
                                                                                     std::size_t maximumRetainedBytes) const;
        [[nodiscard]] Result<std::shared_ptr<PreparedPhysicsSceneCapture>> PrepareCapture(const Runtime::PersistentEntityIdentityMap &,
                                                                                          SceneStateStoragePolicy,
                                                                                          const Sha256Digest &authenticatedContent,
                                                                                          std::shared_ptr<const void> contentLease) const;
        [[nodiscard]] Result<std::shared_ptr<PreparedPhysicsSceneRestore>> PrepareRestore(
            std::unique_ptr<StateDetail::RestartInput>, const Runtime::PersistentEntityIdentityMap &, SceneStateStoragePolicy,
            const Sha256Digest &authenticatedContent, std::shared_ptr<const void> contentLease, const CancellationToken &) const;

    private:
        PhysicsSceneStateTarget(PhysicsSceneActivationCandidate &, std::shared_ptr<PhysicsStructuralState>, bool published) noexcept;
        [[nodiscard]] Result<std::shared_ptr<PhysicsStructuralState>> Validate() const;
        [[nodiscard]] Result<void> EmptyCharacterOwner() const;
        [[nodiscard]] Result<std::shared_ptr<PhysicsStructuralState>> CoveredOwner(bool published) const;
        void InstallBindings(PhysicsStructuralState &, PhysicsSceneStateRebind &) noexcept;
        [[nodiscard]] Result<void> RefreshEmptyCharacter() const;
        [[nodiscard]] Result<void> PreflightPendingContinuation() const;
        PhysicsSceneActivationCandidate *candidate_{};
        std::weak_ptr<PhysicsStructuralState> state_;
        std::thread::id ownerThread_;
        Runtime::SceneRuntimeId scene_;
        std::uint64_t revision_{};
        bool published_{};
    };

    /** @brief Cold exact configuration; Capture uses only canonical preallocated slots and same-owner current fences. */
    class PreparedPhysicsSceneCapture final {
        friend class PhysicsSceneStateTarget;

    public:
        [[nodiscard]] Result<PhysicsStateSnapshotLease> Capture() const;
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;

    private:
        explicit PreparedPhysicsSceneCapture(PhysicsSceneStateTarget target) : target_(std::move(target)) {}

        [[nodiscard]] Result<void> Initialize(const PhysicsStructuralState &, const Runtime::PersistentEntityIdentityMap &,
                                              SceneStateStoragePolicy, const Sha256Digest &, std::shared_ptr<const void>);
        PhysicsSceneStateTarget target_;
        std::unique_ptr<PhysicsSceneStateProjection> projection_;
        std::weak_ptr<PhysicsWorldSnapshotState> configuration_;
        std::size_t configurationBytes_{};
    };

    /** @brief Retained actual pending Scene final check. StagedRestoreTransaction invokes this at its LAST fallible point.
     * Actual Scene aggregate alone publishes the prepared candidate after canonical BeginCommit; this hook cannot publish a World.
     */
    class PreparedPhysicsSceneRestore final : public Runtime::IStagedRestoreFinalPublicationCheck {
        friend class PhysicsSceneStateTarget;

    public:
        [[nodiscard]] Result<void> ValidateAndArm() override;
        [[nodiscard]] Result<void> CheckPrepared() const;
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;

    private:
        explicit PreparedPhysicsSceneRestore(PhysicsSceneStateTarget target) : target_(std::move(target)) {}

        [[nodiscard]] Result<void> Initialize(PhysicsStructuralState &, const Runtime::PersistentEntityIdentityMap &,
                                              SceneStateStoragePolicy, const Sha256Digest &, std::shared_ptr<const void>);
        [[nodiscard]] Result<std::size_t> PrepareTables(const PhysicsStructuralState &, SceneStateStoragePolicy);
        [[nodiscard]] Result<void> Reconstruct(PhysicsStructuralState &, PhysicsStateCaptureConfiguration);
        PhysicsSceneStateTarget target_;
        std::unique_ptr<StateDetail::RestartInput> input_;
        std::unique_ptr<PhysicsSceneStateRebind> stagingTables_;
        std::unique_ptr<PhysicsSceneStateProjection> projection_;
        std::weak_ptr<PhysicsWorldSnapshotState> configuration_;
        CancellationToken cancellation_;
        std::uint64_t publicationRevision_{};
        std::size_t configurationBytes_{};
    };
}  // namespace Horo::Physics::Detail
