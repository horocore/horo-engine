#pragma once

#include "Horo/Application/ProjectCollisionSettings.h"

namespace Horo::Editor {
    /** @brief Builds the explicit new-project template with newly generated stable IDs, never runtime fallback.
     * @param projectId New Project metadata identity. @return Validated ordinary project definitions or typed failure.
     */
    [[nodiscard]] Result<Application::ProjectCollisionSettings> CreateProjectCollisionTemplate(std::string projectId);
}  // namespace Horo::Editor
