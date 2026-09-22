#pragma once

/**
 * @file UiAssetDependency.h
 * @brief Backend-neutral typed asset requirements used by Runtime UI documents and resources.
 */

#include "Horo/Assets/AssetId.h"

#include <compare>

namespace Horo::Runtime::Ui {
    /**
     * @brief One stable asset required or optionally consumed by a Runtime UI document or image resource.
     *
     * The identity is path-independent and the expected type is captured at the
     * authoring/cook boundary. Runtime UI never resolves paths or chooses a
     * provider; Assets owns that work.
     */
    struct UiAssetDependency final {
        Assets::AssetId asset;            /**< Stable referenced asset identity. */
        Assets::AssetTypeId expectedType; /**< Type required when the asset is resolved. */
        bool required{true};              /**< Whether absence prevents candidate activation. */

        /** @brief Compares canonical dependency evidence. @return Structural ordering and equality. */
        [[nodiscard]] auto operator<=>(const UiAssetDependency &) const noexcept = default;
    };
}  // namespace Horo::Runtime::Ui
