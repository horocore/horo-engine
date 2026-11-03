#pragma once
/** @file PrefabOverrideBaseline.h
 * @brief Synchronous borrowed input to the sole canonical prefab
 * value/component delta resolver.
 */
#include "Horo/Prefab/PrefabMetrics.h"
#include "Horo/Prefab/PrefabResolvedObject.h"
#include "Horo/Prefab/PrefabSourceRevision.h"

#include <span>

namespace Horo::Prefab {
    /** @brief Immutable operation borrow derived from a verified cooked baseline or
     * admitted source candidate.
     * @details This is not a source graph, asset loader, identity issuer or
     * publication capability. The caller retains all values through the synchronous
     * operation. No view escapes its result.
     */
    struct PrefabOverrideBaseline final {
        Assets::AssetId asset;
        const PrefabSourceRevision &revision;
        std::span<const ResolvedPrefabObject> objects;
        Sha256Digest priorOverrides{};
        PrefabMetricHandles metrics{};

        /** @brief Returns the exact baseline asset. @return Copied stable identity.
         */
        [[nodiscard]] Assets::AssetId RootAsset() const noexcept {
            return asset;
        }

        /** @brief Returns the actual immutable baseline revision. @return Borrow
         * valid through the operation. */
        [[nodiscard]] const PrefabSourceRevision &SourceRevision() const noexcept {
            return revision;
        }

        /** @brief Returns the complete baseline objects. @return Synchronous
         * immutable borrow. */
        [[nodiscard]] std::span<const ResolvedPrefabObject> Objects() const noexcept {
            return objects;
        }

        /** @brief Returns the prior delta commitment. @return Nonzero prevents
         * applying intent twice. */
        [[nodiscard]] Sha256Digest ValueOverridesDigest() const noexcept {
            return priorOverrides;
        }

        /** @brief Returns prebound observation handles. @return Owned handles without
         * service discovery. */
        [[nodiscard]] const PrefabMetricHandles &Metrics() const noexcept {
            return metrics;
        }
    };
}  // namespace Horo::Prefab
