#include "Horo/Runtime/Save/SaveStoragePolicy.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace {
        [[nodiscard]] bool IsKnown(const SaveStorageAccess value) noexcept {
            return value >= SaveStorageAccess::ReadWrite && value < SaveStorageAccess::Count;
        }

        [[nodiscard]] bool IsKnown(const SaveStorageFailureCategory value) noexcept {
            return value >= SaveStorageFailureCategory::DiskFull && value < SaveStorageFailureCategory::Count;
        }

        [[nodiscard]] bool IsKnown(const SaveOperationCommitOutcome value) noexcept {
            return value >= SaveOperationCommitOutcome::NotCommitted && value <= SaveOperationCommitOutcome::Unknown;
        }

        [[nodiscard]] bool IsStrictlyOrdered(std::span<const SaveStorageReclaimableGeneration> values) noexcept {
            for (std::size_t index = 1; index < values.size(); ++index) {
                if (!(values[index - 1] < values[index]))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool AddWithoutOverflow(const std::uint64_t left, const std::uint64_t right, std::uint64_t &sum) noexcept {
            if (right > std::numeric_limits<std::uint64_t>::max() - left)
                return false;
            sum = left + right;
            return true;
        }

        [[nodiscard]] const ErrorCodeDescriptor &FailureDescriptor(const SaveStorageFailureCategory category) noexcept {
            using enum SaveStorageFailureCategory;
            switch (category) {
                case DiskFull:
                    return SaveErrors::StorageDiskFull;
                case QuotaExceeded:
                    return SaveErrors::StorageQuotaExceeded;
                case PermissionDenied:
                    return SaveErrors::StoragePermissionDenied;
                case ReadOnly:
                    return SaveErrors::StorageReadOnly;
                case VolumeUnavailable:
                    return SaveErrors::StorageVolumeUnavailable;
                case TransientIo:
                    return SaveErrors::StorageTransientIo;
                case PermanentIo:
                case Count:
                    return SaveErrors::StoragePermanentIo;
            }
            return SaveErrors::StoragePermanentIo;
        }

        [[nodiscard]] SaveStorageRecoveryAction NonTransientRecoveryAction(const SaveStorageFailureCategory category) noexcept {
            using enum SaveStorageRecoveryAction;
            switch (category) {
                case SaveStorageFailureCategory::DiskFull:
                case SaveStorageFailureCategory::QuotaExceeded:
                    return FreeSpace;
                case SaveStorageFailureCategory::PermissionDenied:
                    return FixPermissions;
                case SaveStorageFailureCategory::ReadOnly:
                    return SelectWritableStorage;
                case SaveStorageFailureCategory::VolumeUnavailable:
                    return ReconnectVolume;
                case SaveStorageFailureCategory::TransientIo:
                case SaveStorageFailureCategory::PermanentIo:
                case SaveStorageFailureCategory::Count:
                    return Abort;
            }
            return Abort;
        }

        [[nodiscard]] SaveStorageRecoveryAction RecoveryAction(const SaveStorageFailureInput &input) noexcept {
            using enum SaveStorageRecoveryAction;
            if (input.commitOutcome == SaveOperationCommitOutcome::Unknown)
                return ReconcilePublication;
            if (input.category == SaveStorageFailureCategory::TransientIo)
                return input.completedAutomaticRetries < input.maximumAutomaticRetries ? RetryWithBackoff : Abort;
            return NonTransientRecoveryAction(input.category);
        }

        [[nodiscard]] std::uint64_t CapacityAfterReservations(const std::uint64_t available, const std::uint64_t reserved) noexcept {
            return available > reserved ? available - reserved : 0;
        }

        [[nodiscard]] SaveStoragePreflightOutcome SelectPreflightOutcome(const SaveStorageCapacitySnapshot &capacity,
                                                                         const std::uint64_t required, const std::uint64_t physical,
                                                                         const std::uint64_t quota,
                                                                         const std::uint64_t effective) noexcept {
            if (capacity.Access() == SaveStorageAccess::ReadOnly)
                return SaveStoragePreflightOutcome::ReadOnly;
            if (capacity.Access() == SaveStorageAccess::Unavailable)
                return SaveStoragePreflightOutcome::Unavailable;
            if (required <= effective)
                return SaveStoragePreflightOutcome::Admitted;
            if (capacity.QuotaRemainingBytes().has_value() && quota <= physical)
                return SaveStoragePreflightOutcome::QuotaExceeded;
            return SaveStoragePreflightOutcome::InsufficientSpace;
        }
    }  // namespace

    /** @copydoc SaveStorageCapacitySnapshot::Create */
    Result<SaveStorageCapacitySnapshot> SaveStorageCapacitySnapshot::Create(const SaveStorageCapacityInput &input) {
        if (!IsKnown(input.access) || input.reclaimable.size() > MaximumSaveStorageReclaimableGenerations ||
            !IsStrictlyOrdered(input.reclaimable))
            return Result<SaveStorageCapacitySnapshot>::Failure(MakeError(SaveErrors::StoragePolicyInvalid));

        SaveStorageCapacitySnapshot snapshot;
        snapshot.access_ = input.access;
        snapshot.availableBytes_ = input.availableBytes;
        snapshot.quotaRemainingBytes_ = input.quotaRemainingBytes;
        snapshot.reservedBytes_ = input.reservedBytes;
        for (const SaveStorageReclaimableGeneration &generation : input.reclaimable) {
            std::uint64_t reclaimableBytes{};
            if (!generation.slot.IsValid() || !generation.generation.IsValid() || generation.bytes == 0 ||
                !AddWithoutOverflow(snapshot.reclaimableBytes_, generation.bytes, reclaimableBytes))
                return Result<SaveStorageCapacitySnapshot>::Failure(MakeError(SaveErrors::StoragePolicyInvalid));
            snapshot.reclaimableBytes_ = reclaimableBytes;
            snapshot.reclaimable_[snapshot.reclaimableCount_++] = generation;
        }
        return Result<SaveStorageCapacitySnapshot>::Success(std::move(snapshot));
    }

    /** @copydoc SaveStorageCapacitySnapshot::Access */
    SaveStorageAccess SaveStorageCapacitySnapshot::Access() const noexcept {
        return access_;
    }

    /** @copydoc SaveStorageCapacitySnapshot::AvailableBytes */
    std::uint64_t SaveStorageCapacitySnapshot::AvailableBytes() const noexcept {
        return availableBytes_;
    }

    /** @copydoc SaveStorageCapacitySnapshot::QuotaRemainingBytes */
    std::optional<std::uint64_t> SaveStorageCapacitySnapshot::QuotaRemainingBytes() const noexcept {
        return quotaRemainingBytes_;
    }

    /** @copydoc SaveStorageCapacitySnapshot::ReservedBytes */
    std::uint64_t SaveStorageCapacitySnapshot::ReservedBytes() const noexcept {
        return reservedBytes_;
    }

    /** @copydoc SaveStorageCapacitySnapshot::Reclaimable */
    std::span<const SaveStorageReclaimableGeneration> SaveStorageCapacitySnapshot::Reclaimable() const noexcept {
        return {reclaimable_.data(), reclaimableCount_};
    }

    /** @copydoc SaveStorageCapacitySnapshot::ReclaimableBytes */
    std::uint64_t SaveStorageCapacitySnapshot::ReclaimableBytes() const noexcept {
        return reclaimableBytes_;
    }

    /** @copydoc SaveStoragePreflightDecision::IsAdmitted */
    bool SaveStoragePreflightDecision::IsAdmitted() const noexcept {
        return outcome == SaveStoragePreflightOutcome::Admitted;
    }

    /** @copydoc EvaluateSaveStoragePreflight */
    Result<SaveStoragePreflightDecision> EvaluateSaveStoragePreflight(const SaveStorageCapacitySnapshot &capacity,
                                                                      const SaveStoragePreflightRequest request) {
        std::uint64_t required{};
        if (request.additionalPeakBytes == 0 || !AddWithoutOverflow(request.additionalPeakBytes, request.safetyReserveBytes, required))
            return Result<SaveStoragePreflightDecision>::Failure(MakeError(SaveErrors::StoragePolicyInvalid));

        const std::uint64_t physicalAfterReservations = CapacityAfterReservations(capacity.AvailableBytes(), capacity.ReservedBytes());
        const std::uint64_t quotaAfterReservations =
            capacity.QuotaRemainingBytes().has_value()
                ? CapacityAfterReservations(*capacity.QuotaRemainingBytes(), capacity.ReservedBytes())
                : std::numeric_limits<std::uint64_t>::max();
        const std::uint64_t effective = std::min(physicalAfterReservations, quotaAfterReservations);
        SaveStoragePreflightDecision decision{.outcome = SelectPreflightOutcome(capacity, required, physicalAfterReservations,
                                                                                quotaAfterReservations, effective),
                                              .requiredBytes = required,
                                              .effectiveAvailableBytes = effective,
                                              .shortfallBytes = required > effective ? required - effective : 0,
                                              .reclaimableBytes = capacity.ReclaimableBytes()};
        return Result<SaveStoragePreflightDecision>::Success(decision);
    }

    /** @copydoc SaveStorageFailureDecision::Category */
    SaveStorageFailureCategory SaveStorageFailureDecision::Category() const noexcept {
        return category_;
    }

    /** @copydoc SaveStorageFailureDecision::Action */
    SaveStorageRecoveryAction SaveStorageFailureDecision::Action() const noexcept {
        return action_;
    }

    /** @copydoc SaveStorageFailureDecision::CanRetryAutomatically */
    bool SaveStorageFailureDecision::CanRetryAutomatically() const noexcept {
        return action_ == SaveStorageRecoveryAction::RetryWithBackoff;
    }

    /** @copydoc SaveStorageFailureDecision::RemainingAutomaticRetries */
    std::uint8_t SaveStorageFailureDecision::RemainingAutomaticRetries() const noexcept {
        return remainingAutomaticRetries_;
    }

    /** @copydoc SaveStorageFailureDecision::ErrorValue */
    const Error &SaveStorageFailureDecision::ErrorValue() const noexcept {
        return error_;
    }

    /** @copydoc MakeSaveStorageFailureDecision */
    Result<SaveStorageFailureDecision> MakeSaveStorageFailureDecision(SaveStorageFailureInput input) {
        if (!IsKnown(input.category) || !IsKnown(input.commitOutcome) ||
            input.maximumAutomaticRetries > MaximumSaveStorageAutomaticRetries ||
            input.completedAutomaticRetries > input.maximumAutomaticRetries || input.nativeCause.code.Value().empty() ||
            input.nativeCause.domain.Value().empty())
            return Result<SaveStorageFailureDecision>::Failure(MakeError(SaveErrors::StoragePolicyInvalid));
        try {
            SaveStorageFailureDecision decision;
            decision.category_ = input.category;
            decision.action_ = RecoveryAction(input);
            decision.remainingAutomaticRetries_ =
                decision.CanRetryAutomatically()
                    ? static_cast<std::uint8_t>(input.maximumAutomaticRetries - input.completedAutomaticRetries)
                    : 0;
            decision.error_ = WrapError(FailureDescriptor(input.category), std::move(input.nativeCause));
            return Result<SaveStorageFailureDecision>::Success(std::move(decision));
        } catch (const std::bad_alloc &) {
            return Result<SaveStorageFailureDecision>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
        }
    }
}  // namespace Horo::Runtime
