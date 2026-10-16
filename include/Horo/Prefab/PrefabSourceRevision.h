#pragma once
/** @file PrefabSourceRevision.h
 * @brief Exact immutable prefab source identity shared by source models and authoring services.
 */
#include "Horo/Application/ProjectVersion.h"
#include "Horo/Foundation/Sha256.h"

namespace Horo::Prefab {
    /** @brief Canonical semantic source digest paired with the unified project version. */
    struct PrefabSourceRevision final {
        Application::HoroVersion projectVersion; /**< Unified project version, never a separate prefab counter. */
        Sha256Digest contentDigest;              /**< Digest of canonical semantic source bytes. */
        [[nodiscard]] bool operator==(const PrefabSourceRevision &) const noexcept = default;
    };
}  // namespace Horo::Prefab
