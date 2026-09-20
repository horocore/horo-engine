#pragma once

/**
 * @file NavigationAgentJson.h
 * @brief Shared private JSON projection for provider-neutral navigation-agent components.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Scene/NavigationSceneComponents.h"

#include <nlohmann/json_fwd.hpp>

namespace Horo::Editor::Detail {
    /**
     * @brief Parses and validates one provider-neutral navigation-agent JSON object.
     * @param value Canonical JSON component object.
     * @return Typed component or NavigationErrors::AgentDescriptorInvalid.
     */
    [[nodiscard]] Result<Runtime::NavigationAgentComponent> ParseNavigationAgentJson(const nlohmann::json &value);
}  // namespace Horo::Editor::Detail
