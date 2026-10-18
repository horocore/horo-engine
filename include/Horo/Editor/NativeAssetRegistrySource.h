#pragma once
/** @file NativeAssetRegistrySource.h
 * @brief Upper-editor composition of a native, root-bound asset-registry source.
 */
#include "Horo/Assets/AssetRegistrySource.h"

#include <chrono>
#include <filesystem>
#include <memory>

namespace Horo::Editor {
    /** @brief Independent tooling inventory admission; never changes interactive code-query limits. */
    struct AssetRegistryInventoryPolicy final {
        std::size_t maximumEntries{1U << 20U};
        std::size_t maximumManifestBytes{64U << 20U};
        std::chrono::milliseconds maximumDuration{30000};
    };

    /** @brief Retains native directory authority for one explicit project opening.
     * @param root Absolute authorized project root. @param identity Exact project-opening identity.
     * @param generation Nonzero host-issued opening generation. @param policy Finite inventory limits.
     * @return Owned source or typed admission failure. Close collision sessions before releasing this source.
     * @details Host limits may only reduce the documented ceilings. A capture exceeding capacity or time
     * fails completely; it never returns a truncated registry. Sidecars remain bounded to 1 MiB each.
     */
    [[nodiscard]] Result<std::shared_ptr<const Assets::IAssetRegistrySource>> CreateNativeAssetRegistrySource(
        const std::filesystem::path &root, std::string identity, std::uint64_t generation, const AssetRegistryInventoryPolicy &policy = {});
}  // namespace Horo::Editor
