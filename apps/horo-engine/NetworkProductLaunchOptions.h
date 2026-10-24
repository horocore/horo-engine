#pragma once

/** @file NetworkProductLaunchOptions.h
 * @brief Sole typed parser boundary for network mode, endpoints, gameplay selection and optional real Scene input.
 */
#include "HeadlessProjectScene.h"
#include "Horo/Network/NetworkAddress.h"
#include "Horo/Network/NetworkModeComposition.h"

#include <cstdint>
#include <optional>
#include <span>

namespace Horo::Application::Internal {
    struct ProductLaunch final {
        Network::NetworkProjectRole role{Network::NetworkProjectRole::Count};
        std::uint32_t frames{};
        Network::NetworkWorldCompositionPurpose purpose{Network::NetworkWorldCompositionPurpose::Gameplay};
        Network::NetworkAddress bind;
        Network::NetworkAddress connect;
        std::optional<GameplayWorldSelection> gameplay;
        HeadlessProjectSceneOptions sceneInput;
    };

    /** @brief Parses existing positional network modes plus explicit optional Scene input without native activation.
     * @param arguments Borrowed UTF-8 argument storage, excluding the executable and --run-network-product switch.
     * @return Owned typed options or actionable endpoint/gameplay/Scene option rejection.
     */
    [[nodiscard]] Result<ProductLaunch> ParseNetworkProductLaunch(std::span<char *> arguments);
}  // namespace Horo::Application::Internal
