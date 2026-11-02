#pragma once
/** @file AtomicShaderCookPackage.h @brief Private bounded offline package publication shared by authenticated cook tools. */
#include "AuthenticatedShaderCookConfiguration.h"

#include <span>
#include <string_view>

namespace Horo::Editor::Offline {
    /** @brief Synchronous borrowed canonical compiler output; no compiled data is invented by publication. */
    struct AtomicShaderCookProgram final {
        std::string_view prefix;
        const Render::ShaderManifest *manifest{};
        const Render::ShaderCompilationBatch *compiled{};
    };

    /** @brief Finite package inventory and a decoder that validates the written complete family before rename. */
    struct AtomicShaderCookPackage final {
        std::span<const AtomicShaderCookProgram> programs;
        std::string_view privateCandidatePrefix;
        std::string_view provenanceFilename;
        Result<void> (*validateWritten)(const std::filesystem::path &, const void *){};
        const void *context{};
    };

    [[nodiscard]] Result<void> ValidateAtomicShaderCookDestination(const std::filesystem::path &, std::string_view reservedPrefix);
    [[nodiscard]] Result<void> ValidateAtomicShaderCookPaths(const std::filesystem::path &, const std::filesystem::path &,
                                                             std::string_view reservedPrefix);
    [[nodiscard]] Result<void> PublishAtomicShaderCookPackage(const std::filesystem::path &, const AtomicShaderCookPackage &,
                                                              const Application::Internal::AuthenticatedShaderCookConfiguration &);
}  // namespace Horo::Editor::Offline
