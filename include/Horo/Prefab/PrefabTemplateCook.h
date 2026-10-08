#pragma once

/** @file PrefabTemplateCook.h
 * @brief Bounded source-free runtime template transformation over the shared prefab resolver.
 */
#include "Horo/Assets/AssetCook.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Prefab/CookedPrefab.h"
#include "Horo/Prefab/PrefabSourceResolver.h"

namespace Horo::Prefab {
    /** @brief One immutable canonical resource envelope captured from the host's exact cooked generation. */
    struct PrefabTemplateCookResource final {
        Assets::AssetId asset;
        std::span<const std::uint8_t> artifact; /**< Borrowed for the invocation; complete AssetCook bytes, not HPFB payload. */
    };

    /** @brief Host operation cancellation and independent generic resource-envelope ceilings for one template cook. */
    struct PrefabTemplateCookOptions final {
        CancellationToken cancellation;         /**< Cooperative cancellation ancestry retained for the invocation. */
        Assets::AssetCookLimits resourceLimits; /**< Resource-envelope ceilings, independent of prefab payload limits. */
    };

    /**
     * @brief Resolves and encodes a complete runtime template without interpreting authoring composition twice.
     * @param sources Shared validated resolver snapshot used by authored scene conversion.
     * @param registry Exact registry publication captured by sources.
     * @param root Explicitly selected runtime-spawnable root; selection belongs to host policy.
     * @param resources Complete resource closure with canonical envelopes from one pinned generation, in any order.
     * @param target Exact target required for every resource envelope.
     * @param limits Captured immutable prefab ceilings, checked again before encoding.
     * @param options Owning host operation cancellation ancestry and independent resource-envelope limits.
     * @return Verified immutable HPFB template or original typed resolution, dependency, integrity or limit failure.
     * @throws std::bad_alloc on bounded owned-storage allocation failure; no artifact is published.
     * @details Tooling/background transformation only: no I/O, source migration, catalog registration, publication,
     * runtime activation or callbacks. Components and behaviors retain stable provider identities/schema requirements.
     * The host owns cache identity, resource scheduling and all-or-nothing generation activation. Source-only nested
     * and variant dependencies are flattened and excluded from the runtime resource table.
     */
    [[nodiscard]] Result<CookedPrefab> CookPrefabTemplate(const PrefabSourceResolverSnapshot &sources,
                                                          const Assets::AssetRegistrySnapshot &registry, Assets::AssetId root,
                                                          std::span<const PrefabTemplateCookResource> resources,
                                                          const AssetCookTargetId &target, const PrefabLimitProfile &limits,
                                                          const PrefabTemplateCookOptions &options = {});
}  // namespace Horo::Prefab
