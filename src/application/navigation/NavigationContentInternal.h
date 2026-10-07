#pragma once

#include "Horo/Application/NavigationContentIntegration.h"

#include <nlohmann/json.hpp>

namespace Horo::Application::NavigationContentDetail {
    /** @brief Sole canonical manifest projection of actual provider/schema/settings and validated policy evidence. */
    [[nodiscard]] nlohmann::ordered_json ContentRecord(const Navigation::NavMeshAssetContentExpectation &expected);

    /** @brief Private bounded validator shared by candidate smoke and proof-guarded runtime admission.
     * @details Never exported as an unverified provider factory; candidate smoke discards the detached result. */
    [[nodiscard]] Result<AdmittedNavigationReleaseContent> ValidateContent(std::span<const std::uint8_t> archive,
                                                                           const Release::ReleaseArtifactManifest &manifest,
                                                                           std::string_view archivePath, const AssetCookTargetId &target,
                                                                           Release::DistributionProductKind product,
                                                                           const Assets::AssetArchiveLimits &limits);
}  // namespace Horo::Application::NavigationContentDetail
