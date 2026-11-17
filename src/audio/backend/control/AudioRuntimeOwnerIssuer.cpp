#include "Horo/Audio/Internal/AudioRuntimeOwnerIssuer.h"

#include <limits>

namespace Horo::Audio::Backend {
    /** @copydoc AudioRuntimeOwnerIssuer::Issue */
    Result<AudioRuntimeId> AudioRuntimeOwnerIssuer::Issue() {
        auto previous = lastIssued_.load(std::memory_order_relaxed);
        for (;;) {
            if (previous == std::numeric_limits<std::uint64_t>::max())
                return Result<AudioRuntimeId>::Failure(MakeError(AudioErrors::HandleGenerationExhausted));
            if (lastIssued_.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed))
                return AudioRuntimeId::Create(previous + 1);
        }
    }
}  // namespace Horo::Audio::Backend
