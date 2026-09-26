#pragma once

#include "Horo/Application/GameplayBuildService.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Horo::Application::Detail {
    /** @brief Heterogeneous hash for project keys and compiler identity cache keys. */
    struct TransparentStringHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(const std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    /** @brief Stable identity of the configured compiler for gameplay input hashing. */
    struct CompilerIdentity {
        std::filesystem::path canonicalPath;
        std::uintmax_t size{};
        std::int64_t lastWrite{};
        std::string nativeFileIdentity;
        std::string binaryHash;
    };

    /** @brief Hashes a bounded file without accepting partial reads. */
    [[nodiscard]] Result<std::string> HashFile(const std::filesystem::path &path, std::uintmax_t maximumBytes);
    /** @brief Resolves and hashes the configured compiler identity. */
    [[nodiscard]] Result<CompilerIdentity> ResolveCompilerIdentity(const std::filesystem::path &compiler);
    /** @brief Computes the bounded gameplay build input identity. */
    [[nodiscard]] Result<std::string> ComputeInputHash(const GameplayBuildRequest &request);
    /** @brief Reads the last successfully published input identity. */
    [[nodiscard]] std::optional<std::string> ReadSuccessfulHash(const std::filesystem::path &root);
}  // namespace Horo::Application::Detail
