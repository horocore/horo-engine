#pragma once

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <vector>

namespace Horo::Assets::Internal {
    /** @brief Reads exactly the previously validated artifact size and rejects truncation or cancellation.
     * @param input Open cooked-artifact stream positioned at byte zero.
     * @param expectedBytes Size validated before allocation.
     * @param cancellation Cooperative cancellation token checked between bounded chunks.
     * @return Owned payload or a typed cancellation/truncation failure. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> ReadExactArtifact(std::istream &input, std::size_t expectedBytes,
                                                                      const CancellationToken &cancellation);
}  // namespace Horo::Assets::Internal
