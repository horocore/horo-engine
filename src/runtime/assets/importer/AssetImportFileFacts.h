#pragma once

#include <cstdint>
#include <filesystem>

namespace Horo::Assets::Detail {
    [[nodiscard]] std::filesystem::path NormalizeAssetImportPath(const std::filesystem::path &path);
    [[nodiscard]] std::int64_t AssetImportSourceLastWriteTime(const std::filesystem::path &path);
}  // namespace Horo::Assets::Detail
