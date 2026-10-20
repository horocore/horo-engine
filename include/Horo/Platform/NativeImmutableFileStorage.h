#pragma once

/** @file NativeImmutableFileStorage.h
 * @brief Explicit native composition of immutable storage rooted in one authorized directory identity.
 */

#include "Horo/Foundation/ImmutableFileStorage.h"

#include <filesystem>
#include <memory>

namespace Horo::Platform {
    /** @brief Retains an existing authorized parent and a fixed derived-data namespace without creating output.
     * @param authorizedRoot Absolute directory whose complete ancestor chain is admitted without following redirects.
     * @param relativeNamespace Host-selected portable relative derived-data root; it may be absent and is never caller-selected per call.
     * @return Owned storage capability or a typed failure; no directory or entry is created by this factory.
     * @details Every later operation remains relative to retained directory identities, even if the root's
     * former spelling is replaced. Hosts must revoke/cancel and drain consumers before releasing authority.
     * This factory does not select a project, choose a cache location or grant generation-publication rights.
     */
    [[nodiscard]] Result<std::shared_ptr<const ImmutableFileStorage>> CreateNativeImmutableFileStorage(
        const std::filesystem::path &authorizedRoot, std::string_view relativeNamespace);
}  // namespace Horo::Platform
