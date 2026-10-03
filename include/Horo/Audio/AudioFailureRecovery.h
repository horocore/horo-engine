#pragma once

/**
 * @file AudioFailureRecovery.h
 * @brief Control-side classification of typed audio failures and safe recovery boundaries.
 */

#include "Horo/Foundation/ErrorCode.h"

#include <cstdint>

namespace Horo::Audio {
    /** @brief Subsystem that owns the failed operation; no native backend identity is exposed. */
    enum class AudioFailureArea : std::uint8_t {
        Asset,
        Codec,
        Stream,
        Queue,
        Voice,
        Graph,
        Device,
        Provider,
        Middleware,
        Memory,
        Runtime,
        Unknown,
    };

    /** @brief Terminal or recovery state of the affected operation, not an implicit runtime transition. */
    enum class AudioFailureState : std::uint8_t {
        Rejected,
        Cancelled,
        Recovering,
        Failed,
    };

    /** @brief Next control/host action; none of these actions is executed by classification. */
    enum class AudioRecoveryAction : std::uint8_t {
        None,
        CorrectInput,
        RetryAtSafePoint,
        ReleaseCapacity,
        ResolveIdentity,
        RebuildCandidate,
        RefillStream,
        ReopenDevice,
        ReplaceRuntime,
        RequestHostPolicy,
    };

    /** @brief Required handling of currently published callback-visible state. */
    enum class AudioActiveStatePolicy : std::uint8_t {
        Preserve,
        QuiesceDevice,
        RetainUntilDetached,
    };

    /** @brief Ownership boundary at which control observed the failure; callback code never calls this API. */
    enum class AudioFailureOrigin : std::uint8_t {
        RequestOrCandidate,
        ActiveEpoch,
        CallbackFault,
    };

    /** @brief Immutable classification used by the audio control owner and host policy boundary. */
    struct AudioFailureDecision final {
        AudioFailureArea area{AudioFailureArea::Unknown};
        AudioFailureState state{AudioFailureState::Failed};
        AudioRecoveryAction action{AudioRecoveryAction::RequestHostPolicy};
        AudioActiveStatePolicy activeState{AudioActiveStatePolicy::RetainUntilDetached};
        bool recognized{}; /**< False for foreign, undeclared, malformed or future codes. */

        [[nodiscard]] constexpr bool operator==(const AudioFailureDecision &) const noexcept = default;
    };

    /**
     * @brief Classifies one already-owned error without allocation or mutation.
     * @param error Typed operation failure observed on audio control.
     * @param origin Whether a request/candidate, the active epoch, or a drained callback fault failed.
     * @return Exact declared mapping, or fail-closed host-policy escalation for unknown codes.
     * @note A CallbackFault origin always requires fatal retained-epoch handling, even when the enclosed code is otherwise retryable.
     * Control must still perform the ADR-062 ordered barrier, detachment proof, reconciliation and host notification.
     */
    [[nodiscard]] AudioFailureDecision ClassifyAudioFailure(const Error &error, AudioFailureOrigin origin) noexcept;
}  // namespace Horo::Audio
