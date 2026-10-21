#pragma once

/** @file NativePublicationFileSystem.h
 * @brief Root-identity-bound native publication authority selected explicitly by a host.
 */
#include "Horo/Foundation/Platform.h"

namespace Horo::Platform {
    /** @brief Retains an authorized directory and grants durable publication only below one fixed namespace.
     * @param authorizedRoot Existing absolute normalized root admitted without following ancestor redirects.
     * @param relativeNamespace Nonempty portable relative publication namespace, which may not exist yet.
     * @return Owned authority or a typed failure; the factory creates no output.
     * @details Calls accept absolute normalized paths under authorizedRoot/relativeNamespace, but resolve them
     * relative to retained native directory identities. Replacing the root's old spelling never redirects access.
     * Namespaces must be host-selected derived output, not authored inputs. The host serializes transactions with
     * the returned writer lease, fences source authority before commit, and drains calls before releasing this owner.
     * Concurrent calls retain independent cursors. OS calls are synchronous; cancellation is checked between reads.
     * Writes and copies exclusively create private files; replacing an existing entry requires explicit atomic replacement.
     * Tracked replacement records native commit before any later durability operation or allocation.
     */
    [[nodiscard]] Result<std::shared_ptr<DurableFileSystem>> CreateNativePublicationFileSystem(const std::filesystem::path &authorizedRoot,
                                                                                               std::string_view relativeNamespace);
}  // namespace Horo::Platform
