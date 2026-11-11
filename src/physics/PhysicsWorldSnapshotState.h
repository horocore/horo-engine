#pragma once

#include "PhysicsStateSnapshotInternal.h"
#include "PhysicsStateSnapshotSlot.h"
#include "PhysicsWorldInternal.h"

namespace Horo::Physics::Detail {
    /** @brief Same-world owner state. Closing severs every borrowed world pointer
     * before retained public candidates can outlive it. */
    struct PhysicsWorldSnapshotState final : std::enable_shared_from_this<PhysicsWorldSnapshotState> {
        ~PhysicsWorldSnapshotState() {
            if (!closed.load(std::memory_order_acquire))
                Close();
        }

        PhysicsWorld::Impl *world{};
        const std::thread::id ownerThread{std::this_thread::get_id()};
        std::atomic<bool> closed{};
        std::unique_ptr<CanonicalPhysicsStateWorkspace> workspace;
        std::shared_ptr<PhysicsStateResourceRecord> resources;
        std::vector<std::shared_ptr<PhysicsStateSnapshotSlot>> slots;
        std::unique_ptr<PhysicsEventProjection> stagedEvents;
        std::vector<PhysicsEventProjectionPairState> eventPairScratch;
        std::vector<PhysicsEventRecord> eventScratch;
        std::vector<std::uint8_t> eventOrdinalScratch;
        std::size_t retainedPayloadBytes{};
        PhysicsStateCompatibility compatibility;
        bool pendingRestore{};
        bool candidateRegistered{}; /**< Counter charge starts only once this owner is
                                       admitted, not during fallible cold staging. */
        std::shared_ptr<PhysicsStateRestoreCandidate::Impl> restoreControl;
        PhysicsStateSnapshotLease pendingSnapshot;
        CancellationToken restoreCancellation;
        CanonicalWorldHandle pendingNative;
        std::uint64_t pendingPublicationRevision{};
        std::uint64_t nextRestoreSerial{1};
        std::uint64_t pendingRestoreSerial{};
        std::uint64_t validatedRestoreSerial{};
        std::uint64_t validatedPublicationRevision{};

        /** @brief Transactional cold same-world admission; registers no global/native ownership until every reservation fits. */
        [[nodiscard]] static Result<std::shared_ptr<PhysicsWorldSnapshotState>> Prepare(
            PhysicsWorld::Impl &world, const PhysicsStateCaptureConfiguration &configuration);
        [[nodiscard]] Result<void> PrepareConfiguration(const PhysicsStateCaptureConfiguration &configuration);
        [[nodiscard]] Result<void> PreparePublicationStorage(const PhysicsStateCaptureConfiguration &configuration);
        [[nodiscard]] Result<void> PrepareCompatibility(const PhysicsStateCaptureConfiguration &configuration);
        [[nodiscard]] Result<void> RegisterConfiguration();
        [[nodiscard]] std::size_t FixedRetainedPayloadBytes() const noexcept;
        /** @brief Prepares one unpublished independent slot; all allocation failures
         * preserve every existing capture. */
        [[nodiscard]] static Result<std::shared_ptr<PhysicsStateSnapshotSlot>> PrepareSlot(
            const PhysicsStateLimits &limits, std::uint32_t maximumPairs, std::uint32_t maximumEvents, std::size_t wireBytes,
            std::size_t maximumPayloadBytes, const std::shared_ptr<PhysicsStateResourceRecord> &resources);

        /** @brief At most the cold-admitted slot count; no shared_ptr promotion or
         * count guesses confer write authority. */
        [[nodiscard]] Result<std::shared_ptr<PhysicsStateSnapshotSlot>> SealSlot() {
            for (const auto &slot : slots)
                if (slot->TryWrite())
                    return Result<std::shared_ptr<PhysicsStateSnapshotSlot>>::Success(slot);
            return Result<std::shared_ptr<PhysicsStateSnapshotSlot>>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }

        /** @brief Atomically publishes only a fully written immutable value,
         * transferring its first reader into the opaque lease. */
        [[nodiscard]] PhysicsStateSnapshotLease Publish(const std::shared_ptr<PhysicsStateSnapshotSlot> &slot) noexcept {
            slot->PublishWrite();
            return PhysicsStateSnapshotLease{slot};
        }

        /** @brief Owner drains an exact foreign abandon only after the handle's final
         * control write is visible. */
        void CollectAbandonedRestore() noexcept;
        /** @brief Checks the actual owner/joined admission without creating a new phase authority. */
        [[nodiscard]] Result<void> CheckJoinedOwner() const;
        [[nodiscard]] Result<void> CheckCurrentWorld() const;
        [[nodiscard]] Result<PhysicsStateSnapshotLease> Capture();
        [[nodiscard]] Result<PhysicsStateSnapshotLease> Decode(std::span<const std::byte> bytes);
        [[nodiscard]] Result<PhysicsStateRestoreCandidate> PrepareRestore(PhysicsStateSnapshotLease snapshot,
                                                                          const CancellationToken &cancellation);
        [[nodiscard]] Result<void> ValidateSnapshot(const PhysicsStateSnapshot &snapshot);
        [[nodiscard]] Result<void> CheckRestoreAdmission(const PhysicsStateSnapshotLease &snapshot,
                                                         const CancellationToken &cancellation) const;
        [[nodiscard]] PhysicsStateRestoreCandidate RegisterRestore(PhysicsStateSnapshotLease snapshot,
                                                                   const CancellationToken &cancellation,
                                                                   CanonicalWorldHandle native) noexcept;
        [[nodiscard]] Result<void> CaptureSlot(PhysicsStateSnapshot::Impl &value);
        [[nodiscard]] Result<void> CheckPendingSourcePublication(std::uint64_t serial) const;
        [[nodiscard]] Result<void> CheckPendingNativeTopology() const;
        /** @brief Revalidates the exact pending attempt at the real owner publication
         * fence. */
        [[nodiscard]] Result<void> ValidatePendingRestore(std::uint64_t serial);
        /** @brief Performs only the already-armed same-fence owner swap; no
         * allocation or callbacks. */
        void PublishPendingRestore(std::uint64_t serial) noexcept;

        /** @brief Native and staged projection ownership is released only by the
         * actual world owner, never by a worker lease release. */
        void Close() noexcept;
    };

    /** @brief Exclusive private writer rollback; failure cannot leave a Writing
     * slot permanently occupied. */
    class PhysicsSnapshotWriteGuard final {
    public:
        explicit PhysicsSnapshotWriteGuard(PhysicsStateSnapshotSlot &slot) noexcept : slot_(&slot) {}

        ~PhysicsSnapshotWriteGuard() {
            if (slot_)
                slot_->AbortWrite();
        }

        PhysicsSnapshotWriteGuard(const PhysicsSnapshotWriteGuard &) = delete;
        PhysicsSnapshotWriteGuard &operator=(const PhysicsSnapshotWriteGuard &) = delete;

        void Release() noexcept {
            slot_ = nullptr;
        }

    private:
        PhysicsStateSnapshotSlot *slot_;
    };
}  // namespace Horo::Physics::Detail
