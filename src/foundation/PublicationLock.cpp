/** @file PublicationLock.cpp
 * @brief Owns the backend-neutral publication lease wrapper independently of native adapter linkage.
 */

#include "PublicationLockState.h"

#include <utility>

namespace Horo {
    ExclusiveFileLock::ExclusiveFileLock() noexcept = default;

    ExclusiveFileLock::ExclusiveFileLock(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    ExclusiveFileLock::~ExclusiveFileLock() = default;
    ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock &&) noexcept = default;
    ExclusiveFileLock &ExclusiveFileLock::operator=(ExclusiveFileLock &&) noexcept = default;

    ExclusiveFileLock::operator bool() const noexcept {
        return state_ != nullptr;
    }

    /** @copydoc ExclusiveFileLock::ProtectsPath */
    bool ExclusiveFileLock::ProtectsPath(const std::filesystem::path &path) const {
        return state_ != nullptr && state_->ProtectsPath(path);
    }
}  // namespace Horo
