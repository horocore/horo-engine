#pragma once

#include "Horo/Packages/PackageArchive.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::PackageCommand {
    inline constexpr std::string_view ToolVersion = "1.0.0";
    inline constexpr std::size_t MaximumArtifactBytes = 64U * 1024U * 1024U;

    struct Outcome final {
        bool success{};
        std::string code;
        std::string detail;
    };

    [[nodiscard]] Outcome Success(std::string detail = {});
    [[nodiscard]] Outcome Failure(std::string_view code, std::string_view detail);
    [[nodiscard]] Outcome ReadBounded(const std::filesystem::path &path, std::size_t limit, std::vector<std::byte> &bytes);
    [[nodiscard]] Outcome WriteNew(const std::filesystem::path &path, std::span<const std::byte> bytes);
    [[nodiscard]] Outcome VerifyArchive(std::span<const std::byte> bytes, std::optional<Packages::ValidatedPackageArchive> &archive);

    [[nodiscard]] Outcome Pack(const std::filesystem::path &root, const std::filesystem::path &output);
    [[nodiscard]] Outcome Inspect(const std::filesystem::path &archive);
    [[nodiscard]] Outcome Verify(const std::filesystem::path &archive, const std::filesystem::path &signature,
                                 const std::filesystem::path &trust, std::string_view packageId);
    [[nodiscard]] Outcome Sign(const std::filesystem::path &archive, const std::filesystem::path &key, std::string_view publisher,
                               std::string_view keyId, const std::filesystem::path &output);
}  // namespace Horo::PackageCommand
