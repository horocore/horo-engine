#pragma once

#include "Horo/Runtime/Save/SaveArchiveFraming.h"

namespace Horo::Runtime::SaveChunkCompressionDetail {
    [[nodiscard]] Result<std::vector<std::byte>> Decode(const SaveChunkDirectoryEntry &entry, std::span<const std::byte> stored,
                                                        const SaveChunkDirectoryLimits &limits);
    [[nodiscard]] bool Supports(SaveChunkCodec codec) noexcept;
}  // namespace Horo::Runtime::SaveChunkCompressionDetail
