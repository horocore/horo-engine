#pragma once

/** @file HeadlessPrefabIntent.h @brief Local cooked Prefab intent translated to and from authority relationships. */

#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Prefab/PrefabSpawnService.h"

namespace Horo::Application::Internal {
    /** @brief Encodes projected spawn inputs using stable authored or admitted authority object references. */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeHeadlessPrefabIntent(const Prefab::PrefabSpawnRequest &request,
                                                                            const Prefab::PrefabLimitProfile &limits,
                                                                            const Runtime::RuntimeSceneView &scene,
                                                                            const Network::NetworkAuthorityObjectPublication &authority);
    /** @brief Reconstructs local spawn inputs from protected protocol data and the selected local cook target. */
    [[nodiscard]] Result<Prefab::PrefabSpawnRequest> DecodeHeadlessPrefabIntent(const Network::NetworkPrefabLifecycleRecord &record,
                                                                                const AssetCookTargetId &target,
                                                                                const Prefab::PrefabLimitProfile &limits,
                                                                                const Runtime::RuntimeSceneView &scene,
                                                                                const Network::NetworkClientPrefabInbox &inbox);
}  // namespace Horo::Application::Internal
