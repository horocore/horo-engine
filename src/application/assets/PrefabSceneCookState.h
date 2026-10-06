#pragma once

/** @file PrefabSceneCookState.h @brief Target-private immutable host capture and cooker candidate handoff. */
#include "Horo/Application/PrefabSceneCookHost.h"

namespace Horo::Application::PrefabCookDetail {
    /** @brief Target-private capture owns actual-byte freshness evidence separately from semantic cache identity. */
    struct HostCapture final {
        ProjectMetadata metadata;
        Sha256Digest semanticDigest;
        Sha256Digest projectBytes;
        std::optional<Sha256Digest> packageBytes;
        std::optional<Sha256Digest> packageIntentBytes;
    };

    /** @brief Reads and validates compatibility and package-lock/archive evidence; never migrates or activates providers. */
    [[nodiscard]] Result<HostCapture> CaptureHost(const PrefabSceneCookRequest &request, const ProjectCompatibilityInspector &compatibility,
                                                  const Release::ReleaseExecutionPlan *releasePlan);
    /** @brief Rechecks captured metadata/package bytes while the host retains the project lease. */
    [[nodiscard]] Result<void> VerifyHost(const PrefabSceneCookRequest &request, const HostCapture &capture,
                                          const ProjectCompatibilityInspector &compatibility,
                                          const Release::ReleaseExecutionPlan *releasePlan);
    /** @brief Composes source-free scene strategies and host-key wrappers from pinned canonical source/domain inputs. */
    [[nodiscard]] Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>> PrepareCatalog(
        const PrefabSceneCookRequest &request, const HostCapture &host, const Assets::AssetCookInputSnapshot &inputs,
        const Prefab::PrefabLimitProfile &limits, const Assets::CookerCatalogSnapshot &catalog, const CancellationToken &cancellation);
}  // namespace Horo::Application::PrefabCookDetail
