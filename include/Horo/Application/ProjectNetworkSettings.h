#pragma once

/** @file ProjectNetworkSettings.h
 * @brief Application composition accessor for persisted optional Network policy.
 */

#include "Horo/Application/ProjectSourceDocument.h"
#include "Horo/Network/NetworkProjectSettings.h"

namespace Horo::Application {
    /** @brief Resolves optional authored policy through the sole portable Network codec without defaults or activation.
     * @param source Canonically captured project source document.
     * @return Validated optional settings, absence, or the original typed Network codec diagnostic.
     * @post Absence remains absence; legacy missing inventory remains Unknown, never authored CompleteEmpty.
     */
    [[nodiscard]] Result<std::optional<Network::NetworkProjectSettings>> ResolveProjectNetworkSettings(const ProjectSourceDocument &source);
}  // namespace Horo::Application
