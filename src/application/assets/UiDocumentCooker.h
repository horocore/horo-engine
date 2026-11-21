#pragma once

/** @file UiDocumentCooker.h @brief Application-private producer for registered Runtime UI documents. */

#include "Horo/Assets/CookCatalog.h"

namespace Horo::Application::PrefabCookDetail {
    /**
     * @brief Creates the UI document resource contribution for one exact project inventory and target.
     * @param registry Immutable inventory pinned by the project cook.
     * @param target Exact target in the same publication attempt.
     * @return Contribution for the selected target.
     */
    [[nodiscard]] Assets::CookerContribution MakeUiDocumentCooker(Assets::AssetRegistrySnapshot registry, const AssetCookTargetId &target);
}  // namespace Horo::Application::PrefabCookDetail
