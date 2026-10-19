#pragma once

/** @file NavigationBakeSourceValidation.h
 * @brief Neutral owned-source validation for background navigation bake publication.
 */
#include "Horo/Foundation/Result.h"

namespace Horo {
    class CancellationToken;
}

namespace Horo::Navigation {
    /** @brief Host-owned immutable source evidence that can be checked by background bake work.
     * @details This neutral tooling boundary performs no source discovery or provider selection. Implementations
     * retain their read capability, original deadline and capture identity, and are safe for worker-thread reads.
     * Final verification runs while the application's source-mutation guard is held; it must not reenter that
     * authority, mutate source, publish, or renew trust/deadlines. Ordinary runtime queries never invoke it.
     */
    class INavigationBakeSourceValidation {
    public:
        virtual ~INavigationBakeSourceValidation() = default;
        /** @brief Revalidates actual retained source evidence and its live read authority.
         * @param cancellation Additional invocation cancellation, without replacing captured cancellation.
         * @return Success only for the same current capture. Preserve original typed failure causes; report cancellation
         * as BakeInputCancelled so worker scheduling and host exit classification agree.
         */
        [[nodiscard]] virtual Result<void> ValidateCurrent(const CancellationToken &cancellation) const = 0;
    };

}  // namespace Horo::Navigation
