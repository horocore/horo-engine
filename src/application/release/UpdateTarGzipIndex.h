#pragma once

#include "Horo/Release/UpdateArchiveIndex.h"

#include <filesystem>
#include <functional>
#include <span>
#include <vector>

namespace Horo::Release::Detail {
    /** @brief Receives authenticated tar file bytes in order during a bounded index pass. */
    using TarPayloadCallback = std::function<bool(const UpdateArchiveEntry &, std::span<const unsigned char>, std::uint64_t)>;

    /** @brief Reads one already signature-verified tar.gz package without mutating the filesystem. */
    [[nodiscard]] Result<std::vector<UpdateArchiveEntry>> ReadTarGzipIndex(const std::filesystem::path &path,
                                                                           const UpdateArchiveLimits &limits,
                                                                           const TarPayloadCallback &callback = {});
}  // namespace Horo::Release::Detail
