#pragma once

#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Packages/GameplaySourceLibrary.h"

namespace Horo::Application::Detail {
    /** @brief Borrows a request's immutable selection owner, or an empty span when imports are omitted. */
    [[nodiscard]] std::span<const Packages::GameplaySourceLibrary> GameplayLibraries(const GameplayBuildRequest &request) noexcept;
    /** @brief Decodes canonical UTF-8 package/manifest paths through the native filesystem representation. */
    [[nodiscard]] std::filesystem::path GameplayLibraryPathFromUtf8(std::string_view text);
    /** @brief Encodes a native path for the UTF-8 CMake/JSON boundary. */
    [[nodiscard]] std::string GameplayLibraryPathToUtf8(const std::filesystem::path &path);

    /** @brief Returns a filesystem-safe hexadecimal cache key without the digest's colon-bearing algorithm prefix. */
    [[nodiscard]] std::string GameplayLibraryCacheKey(const Packages::GameplaySourceLibrary &library);

    /** @brief Validates current portable intent/lock against installed source leases and hashes canonical library selection. */
    [[nodiscard]] Result<std::string> GameplayLibraryInputIdentity(const GameplayBuildRequest &request);

    /** @brief Requires the configured primary SDK module to declare every selected imported source before compilation. */
    [[nodiscard]] Result<void> ValidateGameplayLibraryConsumption(const GameplayBuildRequest &request,
                                                                  const std::filesystem::path &buildRoot);

    /** @brief Materializes exact verified source bytes into the project-owned build cache; never executes package scripts. */
    [[nodiscard]] Result<std::filesystem::path> PrepareGameplayLibraries(const GameplayBuildRequest &request,
                                                                         const std::filesystem::path &buildRoot, DurableFileSystem &files,
                                                                         const CancellationToken &cancellation);
}  // namespace Horo::Application::Detail
