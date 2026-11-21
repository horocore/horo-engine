#pragma once

#include "Horo/AI/EnvironmentQueryPhysics.h"

#include <array>
#include <functional>

namespace Horo::Editor {
    /** @brief Captures blocking visibility against the exact authoring Scene on its owner thread. */
    struct EnvironmentQueryPreviewPhysics final {
        using Visibility = std::array<bool, AI::MaximumQueryPhysicsVisibilityRays>;
        std::function<Result<Visibility>(const Runtime::RuntimeSceneView &, const AI::QueryPhysicsVisibilityRequest &)> capture;
    };
}  // namespace Horo::Editor
