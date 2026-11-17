#pragma once
/** @file PrefabCompositionOrigin.h @brief Inert canonical composition application evidence shared with value models. */
#include "Horo/Prefab/PrefabIdentity.h"

#include <optional>

namespace Horo::Prefab {
    /** @brief Actual canonical composition boundary through which an authored value was imported. */
    enum class PrefabCompositionOrigin : std::uint8_t {
        Variant,
        Nested
    };

    /** @brief Immutable outer-to-inner application evidence; asset equality never determines value origin. */
    struct PrefabCompositionOriginStep final {
        PrefabCompositionOrigin kind{PrefabCompositionOrigin::Variant};
        Assets::AssetId owner;
        Assets::AssetId source;
        std::optional<LocalObjectId> placement; /**< Present only for the exact nested placement boundary. */
        [[nodiscard]] bool operator==(const PrefabCompositionOriginStep &) const noexcept = default;
    };
}  // namespace Horo::Prefab
