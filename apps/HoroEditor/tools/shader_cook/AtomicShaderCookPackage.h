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

    /** @brief Exact caller-owned bounded auxiliary payload; publication copies only these explicit bytes, never directory discovery. */
    struct AtomicShaderCookAuxiliaryFile final {
        std::string_view relativePath;
        std::span<const std::uint8_t> bytes;
    };

    /** @brief Finite package inventory and a decoder that validates the written complete family before rename. */
    struct AtomicShaderCookPackage final {
        std::span<const AtomicShaderCookProgram> programs;
        std::string_view privateCandidatePrefix;
        std::string_view provenanceFilename;
        Result<void> (*validateWritten)(const std::filesystem::path &, const void *){};
        const void *context{};
        std::span<const AtomicShaderCookAuxiliaryFile>
            auxiliaryFiles; /**< At most 16 files, 32 MiB each, 64 MiB aggregate; validator is read-only. */
    };

    /** @brief Publishes an ordinary owned source directory using native atomic no-replace rename.
     * @param source Absolute existing ordinary directory; remains owned by caller on failure.
     * @param destination Absolute absent target; any concurrently created target is preserved.
     * @return Success transfers the directory, otherwise ScratchIoFailed; no replacing fallback exists.
     */
    [[nodiscard]] Result<void> PublishShaderCookDirectory(const std::filesystem::path &source, const std::filesystem::path &destination);

    [[nodiscard]] Result<void> ValidateAtomicShaderCookDestination(const std::filesystem::path &, std::string_view reservedPrefix);
    [[nodiscard]] Result<void> ValidateAtomicShaderCookPaths(const std::filesystem::path &, const std::filesystem::path &,
                                                             std::string_view reservedPrefix);
    [[nodiscard]] Result<void> PublishAtomicShaderCookPackage(const std::filesystem::path &, const AtomicShaderCookPackage &,
                                                              const Application::Internal::AuthenticatedShaderCookConfiguration &);
}  // namespace Horo::Editor::Offline
