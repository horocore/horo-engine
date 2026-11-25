#pragma once

/** @file NpcDialogueHttpsProvider.h @brief Host composition of a bounded HTTPS dialogue provider. */

#include "Horo/AI/NpcDialogue.h"

#include <string>

namespace Horo::App {
    /** @brief Endpoint and trust configuration from the host's approved provider catalog. */
    struct NpcDialogueHttpsConfig final {
        AI::NpcDialogueRoute route;
        std::uint64_t generation{};
        std::string endpoint;
        std::string caBundle; /**< Optional host-selected PEM trust bundle. */
    };

    /** @brief Binds the version-one dialogue protocol to one explicit HTTPS endpoint.
     * @param config Host-approved route and immutable connection policy.
     * @return A statically linked provider binding or an error without exposing connection details.
     */
    [[nodiscard]] Result<AI::NpcDialogueProviderBinding> MakeNpcDialogueHttpsProvider(NpcDialogueHttpsConfig config);
}  // namespace Horo::App
