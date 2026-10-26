#pragma once
/** @file NetworkModeHostAccess.h @brief Non-installed composition access to the stable canonical world owner. */
#include "Horo/Network/NetworkModeComposition.h"
#include "NetworkWorldBindingController.h"

namespace Horo::Network {
    /** @brief Host-only access; a weak borrow grants no world, peer or transport authority. */
    struct NetworkModeHostAccess final {
        /** @brief Borrow the stable owner across supported composition moves. @return Weak canonical owner, or empty for
         * standalone/qualification. */
        [[nodiscard]] static std::weak_ptr<NetworkWorldBindingController> Binding(const NetworkModeComposition &mode) noexcept;
    };
}  // namespace Horo::Network
