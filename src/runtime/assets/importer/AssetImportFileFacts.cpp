#include "AssetImportFileFacts.h"

namespace Horo::Assets::Detail {
    std::filesystem::path NormalizeAssetImportPath(const std::filesystem::path &path) {
        std::error_code error;
        std::filesystem::path absolute = std::filesystem::absolute(path, error);
        if (error)
            return {};
        absolute = absolute.lexically_normal();
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
        return error ? absolute : canonical;
    }

    std::int64_t AssetImportSourceLastWriteTime(const std::filesystem::path &path) {
        std::error_code error;
        const auto value = std::filesystem::last_write_time(path, error);
        return error ? 0 : static_cast<std::int64_t>(value.time_since_epoch().count());
    }
}  // namespace Horo::Assets::Detail
