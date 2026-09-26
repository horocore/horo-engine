#include "GameplayBuildInvocation.h"

#include <format>

namespace Horo::Application::Detail {
    /** @copydoc BuildEnvironment */
    [[nodiscard]] ProcessEnvironment BuildEnvironment() {
        ProcessEnvironment environment;
        environment.unset = {"CC", "CXX", "CMAKE_GENERATOR", "CMAKE_GENERATOR_PLATFORM", "CMAKE_GENERATOR_TOOLSET", "CMAKE_TOOLCHAIN_FILE"};
        return environment;
    }

    /** @copydoc BuildConfigureArguments */
    [[nodiscard]] std::vector<std::string> BuildConfigureArguments(const GameplayBuildRequest &request,
                                                                   const std::filesystem::path &buildRoot,
                                                                   const std::filesystem::path &candidateManifest) {
        std::vector<std::string> arguments{
            "-S",
            request.projectRoot.string(),
            "-B",
            buildRoot.string(),
            std::format("-DHoroEngineGameplay_DIR={}", request.environment.gameplaySdkPackage.string()),
            std::format("-DCMAKE_BUILD_TYPE={}", request.environment.configuration),
            std::format("-DHORO_GAMEPLAY_MANIFEST_OUTPUT={}", candidateManifest.string()),
        };
        if (request.environment.cxxCompiler.has_value())
            arguments.push_back(std::format("-DCMAKE_CXX_COMPILER={}", request.environment.cxxCompiler->string()));
        if (request.environment.generator.has_value()) {
            arguments.emplace_back("-G");
            arguments.push_back(*request.environment.generator);
        }
        if (request.environment.generatorPlatform.has_value()) {
            arguments.emplace_back("-A");
            arguments.push_back(*request.environment.generatorPlatform);
        }
        if (request.environment.generatorToolset.has_value()) {
            arguments.emplace_back("-T");
            arguments.push_back(*request.environment.generatorToolset);
        }
        if (request.environment.toolchainFile.has_value())
            arguments.push_back(std::format("-DCMAKE_TOOLCHAIN_FILE={}", request.environment.toolchainFile->string()));
        return arguments;
    }

}  // namespace Horo::Application::Detail
