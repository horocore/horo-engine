#pragma once

#include "Horo/Platform/NativeProjectReadFiles.h"

namespace Horo::Platform::CodeFilesTesting {
    /** @brief Deterministic native access boundaries available only to non-installed contract tests. */
    enum class Boundary : std::uint8_t {
        BeforeComponentOpen,
        AfterFileOpen,
        BeforeRead,
        BeforeDirectoryEntryOpen
    };
    /** @brief Per-capability test observer; no global hook or production host discovery. */
    using Observer = std::function<void(Boundary, std::string_view)>;
    /** @brief Creates a native capability with deterministic boundary observation.
     * @param root Authorized directory. @param identity Project fence. @param generation Project generation.
     * @param observer Synchronous test callback; captured state must outlive the capability.
     * @return Owned native capability or typed admission failure.
     */
    [[nodiscard]] Result<std::shared_ptr<const IProjectReadFiles>> Create(const std::filesystem::path &root, std::string identity,
                                                                          std::uint64_t generation, Observer observer);
}  // namespace Horo::Platform::CodeFilesTesting
