#pragma once

/** @file NetworkWorldOccurrenceIssuer.h @brief NetworkRuntime-private checked server-world occurrence producer. */

#include "Horo/Network/ReplicationWorldLifecycle.h"

#include <limits>
#include <thread>

namespace Horo::Network::Detail {
    /** @brief Single mode-owner issuer; failed candidates burn identities and neither counter ever wraps.
     * No number is derived from a peer, connection, host generation or packet. The owning mode must
     * validate its AuthorityServer selection before requesting an occurrence. Counters are local
     * occurrence identities, not credentials; authenticated channels fence old process occurrences.
     */
    class NetworkWorldOccurrenceIssuer final {
    public:
        explicit NetworkWorldOccurrenceIssuer(const std::uint64_t maximumOccurrences = std::numeric_limits<std::uint64_t>::max())
            : maximum_(maximumOccurrences) {}

        NetworkWorldOccurrenceIssuer(const NetworkWorldOccurrenceIssuer &) = delete;
        NetworkWorldOccurrenceIssuer &operator=(const NetworkWorldOccurrenceIssuer &) = delete;
        NetworkWorldOccurrenceIssuer(NetworkWorldOccurrenceIssuer &&) = delete;
        NetworkWorldOccurrenceIssuer &operator=(NetworkWorldOccurrenceIssuer &&) = delete;

        /** @brief Issue once for an actual selected server Scene, retaining exhaustion after every failed candidate.
         * @param scene Exact unpublished local Scene candidate.
         * @param cancellation Host-owned candidate cancellation.
         * @return Complete authority-server descriptor or original typed owner/cancellation/exhaustion failure. */
        [[nodiscard]] Result<ReplicationWorldActivationDescriptor> Issue(Runtime::SceneRuntimeId scene,
                                                                         const CancellationToken &cancellation = {});

    private:
        const std::thread::id owner_{std::this_thread::get_id()};
        const std::uint64_t maximum_;
        std::uint64_t sessions_{};
        std::uint64_t epochs_{};
    };
}  // namespace Horo::Network::Detail
