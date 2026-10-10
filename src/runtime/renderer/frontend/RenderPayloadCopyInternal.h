#pragma once

#include "Horo/Foundation/CancellationToken.h"
#include "RenderParallelWorkErrors.h"

#include <algorithm>
#include <span>
#include <vector>

namespace Horo::Render::Detail {
    /**
     * @brief Copies admitted contiguous payload with at most 4 KiB between cooperative checks.
     * @param destination Empty owned output; it remains private and may contain a prefix on cancellation.
     * @param source Immutable source whose full size has already passed the frame byte budget.
     * @param cancellation Frame cancellation; checked after allocation and before every bounded chunk.
     * @return Success or typed cancellation, without publishing a partial payload.
     * @details Allocation itself is not preemptible. Its size is checked before this helper is called.
     */
    template <typename T, typename ContiguousOwner>
    [[nodiscard]] Result<void> CopyCapturedPayload(ContiguousOwner &destination, const std::span<const T> source,
                                                   const CancellationToken &cancellation) {
        destination.reserve(source.size());
        constexpr std::size_t chunkElements = std::max(std::size_t{1}, std::size_t{4'096} / sizeof(T));
        for (std::size_t offset = 0; offset < source.size();) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ParallelWorkErrors::Cancelled));
            const std::size_t count = std::min(chunkElements, source.size() - offset);
            const auto chunk = source.subspan(offset, count);
            destination.insert(destination.end(), chunk.begin(), chunk.end());
            offset += count;
        }
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(ParallelWorkErrors::Cancelled));
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
