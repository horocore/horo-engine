#include "GameplayBuildInvocation.h"

#include "GameplayBuildLibraries.h"

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
            GameplayLibraryPathToUtf8(request.projectRoot),
            "-B",
            GameplayLibraryPathToUtf8(buildRoot),
            std::format("-DHoroEngineGameplay_DIR={}", GameplayLibraryPathToUtf8(request.environment.gameplaySdkPackage)),
            std::format("-DCMAKE_BUILD_TYPE={}", request.environment.configuration),
            std::format("-DHORO_GAMEPLAY_MANIFEST_OUTPUT={}", GameplayLibraryPathToUtf8(candidateManifest)),
            std::format("-DHORO_GAMEPLAY_LIBRARY_MANIFEST={}",
                        GameplayLibraries(request).empty() ? std::string{}
                                                           : GameplayLibraryPathToUtf8(buildRoot / "gameplay_source_libraries.json")),
        };
        if (request.environment.cxxCompiler.has_value())
            arguments.push_back(std::format("-DCMAKE_CXX_COMPILER={}", GameplayLibraryPathToUtf8(*request.environment.cxxCompiler)));
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
            arguments.push_back(std::format("-DCMAKE_TOOLCHAIN_FILE={}", GameplayLibraryPathToUtf8(*request.environment.toolchainFile)));
        return arguments;
    }

}  // namespace Horo::Application::Detail
