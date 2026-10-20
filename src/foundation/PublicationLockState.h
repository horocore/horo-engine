#ifndef HORO_PUBLICATION_LOCK_STATE_INTERNAL_H
#define HORO_PUBLICATION_LOCK_STATE_INTERNAL_H

/** @file PublicationLockState.h
 * @brief Private lifetime and observation boundary between a Foundation lease and its native owner.
 */

#include "Horo/Foundation/Platform.h"

#include <utility>

namespace Horo {
    /** @brief Owns one lease implementation without exposing native resources to Foundation consumers.
     * @details Only publication tooling uses the virtual observation; it adds no allocation or frame work.
     * The native implementation and its destructor remain resident until the host releases every lease.
     */
    struct ExclusiveFileLock::State {
        virtual ~State() = default;
        State(const State &) = delete;
        State &operator=(const State &) = delete;

        /** @brief Checks the native owner's exact canonical path authority.
         * @param path Canonical absolute lock-file path.
         * @return Whether this live implementation protects the path.
         */
        [[nodiscard]] virtual bool ProtectsPath(const std::filesystem::path &path) const = 0;

    protected:
        State() noexcept = default;
    };

    /** @brief Non-installed native-owner bridge; adoption transfers the complete lease lifetime exactly once. */
    class PublicationLockAccess final {
    public:
        /** @brief Adopts an already acquired native lease. @param state Owned live lease state. @return Move-only lease owner. */
        [[nodiscard]] static ExclusiveFileLock Adopt(std::unique_ptr<ExclusiveFileLock::State> state) noexcept {
            return ExclusiveFileLock{std::move(state)};
        }
    };
}  // namespace Horo

#endif
