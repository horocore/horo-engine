#pragma once
/** @file EditorPlaySavePhysicsArchive.h
 * @brief Detached archive schema checks; never grants native capture or restore authority.
 */
#include "EditorPlaySaveHostStorage.h"

namespace Horo::Application::Internal {
    /** @brief Requires the exact canonical Physics schema/record when selected by the immutable host policy. */
    [[nodiscard]] Result<PlaySavePhysicsReadPolicy> ValidatePlaySavePhysicsArchive(const Runtime::ValidatedSaveArchive &,
                                                                                   const PlaySaveStorageSchema &);
}  // namespace Horo::Application::Internal
