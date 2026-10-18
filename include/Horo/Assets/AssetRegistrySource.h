#pragma once

/** @file AssetRegistrySource.h
 * @brief Explicit project-root inventory and sidecar input for the canonical registry builder.
 */
#include "Horo/Foundation/Result.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets {
    /** @brief Stable whole-capture cooperative deadline failure. */
    namespace AssetRegistrySourceErrors {
        extern const ErrorCodeDescriptor Deadline;
    }

    /** @brief No-follow physical entry classification; directories and special files are not assets. */
    enum class AssetRegistrySourceKind : std::uint8_t {
        Regular,
        Directory,
        Link,
        Special
    };

    /** @brief Exact project-relative spelling, never an absolute or escaping path. */
    struct AssetRegistrySourceEntry final {
        std::string path;
        AssetRegistrySourceKind kind{};
    };

    /** @brief Complete admitted inventory beneath the actual Assets/ or legacy assets/ directory. */
    struct AssetRegistrySourceManifest final {
        std::string assetRoot;
        std::vector<AssetRegistrySourceEntry> entries;
        std::chrono::steady_clock::time_point
            deadline; /**< Same monotonic deadline for inventory, classification and every sidecar read. */
    };

    /** @brief Host-owned synchronous read authority; no service discovery or concrete platform dependency.
     * @details Every capture is fresh, complete within explicit host budgets, and never follows links.
     * Implementations retain their root identity and translate allocation/native failures to typed errors.
     * The registry alone classifies supported source extensions and decodes identity sidecars.
     */
    class IAssetRegistrySource {
    public:
        virtual ~IAssetRegistrySource() = default;
        /** @brief Captures exact root spelling and all descendants, including ambiguous links.
         * @return Complete owned manifest or typed capacity, stale, deadline, cancellation or I/O failure.
         */
        [[nodiscard]] virtual Result<AssetRegistrySourceManifest> Capture() const = 0;
        /** @brief Reads one contained regular sidecar through the same retained root authority.
         * @param path Exact project-relative metadata spelling. @param deadline Deadline supplied by the exact captured manifest.
         * @return At most 1 MiB of bytes or typed failure; the deadline is never renewed per file.
         */
        [[nodiscard]] virtual Result<std::string> ReadSidecar(std::string_view path,
                                                              std::chrono::steady_clock::time_point deadline) const = 0;
    };
}  // namespace Horo::Assets
