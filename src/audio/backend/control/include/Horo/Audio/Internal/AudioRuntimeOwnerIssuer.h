#pragma once

/** @file AudioRuntimeOwnerIssuer.h @brief Explicit process-composition authority for independent Audio runtime identities. */

#include "Horo/Audio/AudioIdentity.h"

#include <atomic>

namespace Horo::Audio::Backend {
    /**
     * @brief Host-owned non-recycling identity source shared by all output families in one process composition.
     *
     * Construct exactly one at the process root and retain it through all borrowing control owners' shutdown.
     * Issue may run on independent game/preview control threads, never a render callback. Relaxed atomics
     * protect only numeric uniqueness; issuance publishes no resources, device clocks or lifecycle state.
     * Recreating a frontend, preview host or workspace must not recreate this authority. No descriptor,
     * service locator, thread ID, backend factory or process-global allocator participates in issuance.
     */
    class AudioRuntimeOwnerIssuer final {
    public:
        AudioRuntimeOwnerIssuer() = default;
        AudioRuntimeOwnerIssuer(const AudioRuntimeOwnerIssuer &) = delete;
        AudioRuntimeOwnerIssuer &operator=(const AudioRuntimeOwnerIssuer &) = delete;

        /**
         * @brief Reserve one process-composition identity which remains retired forever after its runtime ends.
         * @return Fresh nonzero identity, or HandleGenerationExhausted without wraparound or reuse.
         */
        [[nodiscard]] Result<AudioRuntimeId> Issue();

    private:
        std::atomic<std::uint64_t> lastIssued_{};
    };
}  // namespace Horo::Audio::Backend
