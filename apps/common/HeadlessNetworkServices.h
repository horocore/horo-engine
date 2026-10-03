#pragma once

#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"

#include <memory>

namespace Horo::Application::Internal {
    // Installs the real gameplay-world participants shared by standalone and dedicated products.
    // Network and presentation factories remain explicit application decisions.
    [[nodiscard]] Network::NetworkModeFactories ComposeHeadlessNetworkServices(
        std::shared_ptr<const Runtime::RuntimeSceneDefinition> sceneDefinition, Network::NetworkModeFactories selectedFactories = {});
}  // namespace Horo::Application::Internal
