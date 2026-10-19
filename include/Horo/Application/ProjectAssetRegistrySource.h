#pragma once

/** @file ProjectAssetRegistrySource.h
 * @brief Shared read-only project inventory adapter for the canonical Assets registry decoder.
 */

#include "Horo/Assets/AssetRegistrySource.h"
#include "Horo/Platform/ProjectReadFiles.h"

namespace Horo::Application {
    /** @brief Composes a registry source from explicit retained project read authority without native discovery.
     * @param files Owned root-bound capability; inventory and sidecar reads use this same instance.
     * @param context Exact opening identity/generation, cancellation and absolute deadline. A max deadline allows
     * successive captures, each limited by policy.maximumDuration. Any retained authorityStopped callback must own
     * its captured state or be kept alive by the host until this source is released.
     * @param policy Independent finite inventory ceilings, reducible from Platform::ProjectInventoryLimits defaults.
     * @return Owned source or typed invalid/capacity failure; construction performs no filesystem access.
     * @details Capture preserves Assets/ precedence over legacy assets/, no-follow entry classification, and the
     * original deadline for all sidecar reads. The canonical Assets owner alone classifies extensions and decodes
     * identities. This factory grants no trust, write, migration, package or provider activation authority.
     */
    [[nodiscard]] Result<std::shared_ptr<const Assets::IAssetRegistrySource>> CreateProjectAssetRegistrySource(
        std::shared_ptr<const Platform::IProjectReadFiles> files, Platform::ProjectReadContext context,
        const Platform::ProjectInventoryLimits &policy = {});
}  // namespace Horo::Application
