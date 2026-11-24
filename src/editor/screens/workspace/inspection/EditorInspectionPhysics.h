#pragma once

/** @file EditorInspectionPhysics.h @brief Owner-lane read-only access to published authoring Physics. */
#include "../commands/EditorInspectionCommandEndpoint.h"
#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"

namespace Horo::Runtime {
    class RuntimeSceneView;
}

namespace Horo::Editor {
    /** @brief Samples one exact pair in the active authored Scene without retaining Physics authority. */
    struct EditorInspectionPhysics final {
        std::function<Result<std::optional<InspectionPenetrationSample>>(const Runtime::RuntimeSceneView &,
                                                                         const Runtime::PhysicsBodyReference &,
                                                                         const Runtime::PhysicsBodyReference &, std::string_view)>
            capture;
    };
}  // namespace Horo::Editor
