#pragma once

#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Platform/ExternalProcess.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Horo::Application::Detail {
    /** @brief Sanitizes ambient toolchain selectors for a gameplay build process. */
    [[nodiscard]] ProcessEnvironment BuildEnvironment();

    /** @brief Builds typed CMake configure arguments from the selected environment. */
    [[nodiscard]] std::vector<std::string> BuildConfigureArguments(const GameplayBuildRequest &request,
                                                                   const std::filesystem::path &buildRoot,
                                                                   const std::filesystem::path &candidateManifest);
}  // namespace Horo::Application::Detail
