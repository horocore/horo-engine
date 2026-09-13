#pragma once

/**
 * @file SaveStoragePolicy.h
 * @brief Backend-neutral save storage capacity, failure, retry, and recovery policy.
 */

#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveOperation.h"
#include "Horo/Runtime/Save/SaveSlotMetadata.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Horo::Runtime {
    inline constexpr std::size_t MaximumSaveStorageReclaimableGenerations = 32;
    inline constexpr std::uint8_t MaximumSaveStorageAutomaticRetries = 8;

    /** @brief Trusted provider access observed for one namespace capacity snapshot. */
    enum class SaveStorageAccess : std::uint8_t {
        ReadWrite,
        ReadOnly,
        Unavailable,
        Count
    };

    /** @brief Closed portable storage failure vocabulary; native details remain an owned cause. */
    enum class SaveStorageFailureCategory : std::uint8_t {
        DiskFull,
        QuotaExceeded,
        PermissionDenied,
        ReadOnly,
        VolumeUnavailable,
        TransientIo,
        PermanentIo,
        Count
    };

    /** @brief Explicit next action; only RetryWithBackoff authorizes automatic retry. */
    enum class SaveStorageRecoveryAction : std::uint8_t {
        RetryWithBackoff,
        FreeSpace,
        FixPermissions,
        SelectWritableStorage,
        ReconnectVolume,
        ReconcilePublication,
        Abort,
        Count
    };

    /** @brief Preflight result derived from a non-authoritative point-in-time estimate. */
    enum class SaveStoragePreflightOutcome : std::uint8_t {
        Admitted,
        InsufficientSpace,
        QuotaExceeded,
        ReadOnly,
        Unavailable,
        Count
    };

    /** @brief One old generation that policy may present as reclaimable but never deletes. */
    struct SaveStorageReclaimableGeneration final {
        SaveGameSlotId slot;
        SlotGenerationId generation;
        std::uint64_t bytes{};

        [[nodiscard]] constexpr auto operator<=>(const SaveStorageReclaimableGeneration &) const noexcept = default;
    };

    /** @brief Trusted input used to construct one bounded immutable capacity snapshot. */
    struct SaveStorageCapacityInput final {
        SaveStorageAccess access{SaveStorageAccess::Unavailable};
        std::uint64_t availableBytes{};                   /**< Physical bytes currently reported available. */
        std::optional<std::uint64_t> quotaRemainingBytes; /**< Provider quota remaining, or unknown. */
        std::uint64_t reservedBytes{};                    /**< Bytes already reserved by accepted save work. */
        std::span<const SaveStorageReclaimableGeneration> reclaimable;
    };

    /** @brief Bounded immutable namespace capacity evidence used only for preflight estimates. */
    class SaveStorageCapacitySnapshot final {
    public:
        /**
         * @brief Validates and owns one capacity observation.
         * @param input Trusted provider observation and stable-ordered reclaimable generations.
         * @return Immutable snapshot or SaveErrors::StoragePolicyInvalid.
         */
        [[nodiscard]] static Result<SaveStorageCapacitySnapshot> Create(const SaveStorageCapacityInput &input);

        /** @brief Returns observed namespace access. @return Closed access value. */
        [[nodiscard]] SaveStorageAccess Access() const noexcept;
        /** @brief Returns physical available bytes before save reservations. @return Byte count. */
        [[nodiscard]] std::uint64_t AvailableBytes() const noexcept;
        /** @brief Returns provider quota remaining when known. @return Optional byte count. */
        [[nodiscard]] std::optional<std::uint64_t> QuotaRemainingBytes() const noexcept;
        /** @brief Returns bytes already reserved by accepted work. @return Byte count. */
        [[nodiscard]] std::uint64_t ReservedBytes() const noexcept;
        /** @brief Returns stable-ordered informational recovery candidates. @return Immutable bounded view. */
        [[nodiscard]] std::span<const SaveStorageReclaimableGeneration> Reclaimable() const noexcept;
        /** @brief Returns the overflow-checked reclaimable byte total. @return Informational byte count. */
        [[nodiscard]] std::uint64_t ReclaimableBytes() const noexcept;

    private:
        SaveStorageAccess access_{SaveStorageAccess::Unavailable};
        std::uint64_t availableBytes_{};
        std::optional<std::uint64_t> quotaRemainingBytes_;
        std::uint64_t reservedBytes_{};
        std::array<SaveStorageReclaimableGeneration, MaximumSaveStorageReclaimableGenerations> reclaimable_{};
        std::size_t reclaimableCount_{};
        std::uint64_t reclaimableBytes_{};
    };

    /** @brief Peak additional-space estimate for one mutation; it never guarantees later writes. */
    struct SaveStoragePreflightRequest final {
        std::uint64_t additionalPeakBytes{}; /**< Temporary, final, and required recovery-copy headroom not already reserved. */
        std::uint64_t safetyReserveBytes{};  /**< Product reserve that must remain free after admission. */
    };

    /** @brief Immutable arithmetic evidence for a capacity preflight decision. */
    struct SaveStoragePreflightDecision final {
        SaveStoragePreflightOutcome outcome{SaveStoragePreflightOutcome::Unavailable};
        std::uint64_t requiredBytes{};
        std::uint64_t effectiveAvailableBytes{};
        std::uint64_t shortfallBytes{};
        std::uint64_t reclaimableBytes{}; /**< Informational only; never counted as free or deleted automatically. */

        /** @brief Reports whether preflight currently admits work. @return True only for Admitted. */
        [[nodiscard]] bool IsAdmitted() const noexcept;
    };

    /**
     * @brief Evaluates a conservative capacity estimate without treating it as a write guarantee.
     * @param capacity Immutable provider observation.
     * @param request Required additional peak bytes and product safety reserve.
     * @return Decision or SaveErrors::StoragePolicyInvalid on zero/overflowing requirements.
     */
    [[nodiscard]] Result<SaveStoragePreflightDecision> EvaluateSaveStoragePreflight(const SaveStorageCapacitySnapshot &capacity,
                                                                                    SaveStoragePreflightRequest request);

    /** @brief Input for portable failure normalization and bounded retry policy. */
    struct SaveStorageFailureInput final {
        SaveStorageFailureCategory category{SaveStorageFailureCategory::PermanentIo};
        SaveOperationCommitOutcome commitOutcome{SaveOperationCommitOutcome::NotCommitted};
        std::uint8_t completedAutomaticRetries{};
        std::uint8_t maximumAutomaticRetries{};
        Error nativeCause; /**< Original typed platform/provider failure retained as an immutable cause. */
    };

    /** @brief Portable storage failure plus explicit finite recovery behavior. */
    class SaveStorageFailureDecision final {
    public:
        /** @brief Returns normalized category. @return Closed category value. */
        [[nodiscard]] SaveStorageFailureCategory Category() const noexcept;
        /** @brief Returns required owner/user action. @return Closed recovery action. */
        [[nodiscard]] SaveStorageRecoveryAction Action() const noexcept;
        /** @brief Reports whether one more automatic attempt is authorized. @return True only for bounded transient retry. */
        [[nodiscard]] bool CanRetryAutomatically() const noexcept;
        /** @brief Returns automatic retries still authorized, including the current decision. @return Finite remaining count. */
        [[nodiscard]] std::uint8_t RemainingAutomaticRetries() const noexcept;
        /** @brief Returns stable portable error with the original typed cause retained. @return Immutable error. */
        [[nodiscard]] const Error &ErrorValue() const noexcept;

    private:
        friend Result<SaveStorageFailureDecision> MakeSaveStorageFailureDecision(SaveStorageFailureInput input);
        SaveStorageFailureCategory category_{SaveStorageFailureCategory::PermanentIo};
        SaveStorageRecoveryAction action_{SaveStorageRecoveryAction::Abort};
        std::uint8_t remainingAutomaticRetries_{};
        Error error_;
    };

    /**
     * @brief Normalizes native/provider failure without losing its typed cause or permitting unbounded retry.
     * @param input Category, publication knowledge, finite retry state, and original cause.
     * @return Portable decision or SaveErrors::StoragePolicyInvalid.
     */
    [[nodiscard]] Result<SaveStorageFailureDecision> MakeSaveStorageFailureDecision(SaveStorageFailureInput input);
}  // namespace Horo::Runtime
