#pragma once

/**
 * @file SequencePlaybackRuntimeErrors.h
 * @brief Stable failures for session-owned cinematic playback admission and safe-point ownership.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Cinematic::SequencePlaybackRuntimeErrors {
    /** @brief The runtime session identity is reserved or malformed. */
    extern const ErrorCodeDescriptor SessionInvalid;
    /** @brief The requested tier or canonical budget is malformed. */
    extern const ErrorCodeDescriptor BudgetInvalid;
    /** @brief The service has closed admission for shutdown. */
    extern const ErrorCodeDescriptor AdmissionClosed;
    /** @brief A player activation reuses an active stable identity. */
    extern const ErrorCodeDescriptor DuplicateHandle;
    /** @brief The prepared plan, blend policy, or authority transaction is malformed. */
    extern const ErrorCodeDescriptor ActivationInvalid;
    /** @brief The aggregate profile cannot admit the requested player resources. */
    extern const ErrorCodeDescriptor CapacityExceeded;
    /** @brief The authority claim set contains incompatible required owners. */
    extern const ErrorCodeDescriptor AuthorityConflict;
    /** @brief A restore policy or snapshot does not satisfy its safe-point contract. */
    extern const ErrorCodeDescriptor RestoreInvalid;
    /** @brief A terminal player must restore its snapshot before release. */
    extern const ErrorCodeDescriptor RestoreRequired;
    /** @brief The requested operation targets an active rather than terminal player. */
    extern const ErrorCodeDescriptor PlayerNotTerminal;
    /** @brief The submitted handle has an invalid representation. */
    extern const ErrorCodeDescriptor HandleInvalid;
    /** @brief The submitted handle belongs to another player identity. */
    extern const ErrorCodeDescriptor HandleUnknown;
    /** @brief The submitted handle names a retired or newer generation. */
    extern const ErrorCodeDescriptor HandleStale;
    /** @brief A control revision or service reservation cannot advance safely. */
    extern const ErrorCodeDescriptor RevisionExhausted;
}  // namespace Horo::Cinematic::SequencePlaybackRuntimeErrors
